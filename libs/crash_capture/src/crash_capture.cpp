// crash_capture.cpp — ExoSnap crash-capture engine implementation.
//
// EXOSNAP_OFFICIAL_BUILD gate:
//   When defined: sentry_init() is called with the hardcoded EU DSN.
//                 Uploads are still gated by require_user_consent=1 until the
//                 user opts in via GiveUserConsent().
//   When NOT defined: sentry_init() is called WITHOUT a DSN.
//                     No network traffic occurs; local minidumps only.
//
// The before_send hook runs in-process (crashpad out-of-process handler still
// writes the minidump; before_send runs for structured Sentry events).

#include <crash_capture/crash_capture.h>
#include <crash_capture/crash_scrubber.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <shlobj.h> // SHGetKnownFolderPath / FOLDERID_LocalAppData
#include <windows.h>

// ---------------------------------------------------------------------------
// Conditional sentry.h inclusion.
//
// When EXOSNAP_CRASH_CAPTURE_AVAILABLE is defined by CMake (sentry-native was
// found and linked), we include the real sentry.h. Otherwise we compile a
// no-op stub that satisfies the same API so the rest of the codebase compiles
// without sentry-native present.
// ---------------------------------------------------------------------------
#if defined(EXOSNAP_CRASH_CAPTURE_AVAILABLE)
#include <sentry.h>
#define EXOSNAP_SENTRY_AVAILABLE 1
#else
#define EXOSNAP_SENTRY_AVAILABLE 0
#endif

namespace exosnap::crash_capture {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static std::mutex s_init_mutex;
static bool s_initialized = false;
static std::string s_crash_dir;

// ---------------------------------------------------------------------------
// before_send hook — scrubs the event before upload.
// Only called when sentry-native is linked (EXOSNAP_SENTRY_AVAILABLE).
// ---------------------------------------------------------------------------
#if EXOSNAP_SENTRY_AVAILABLE
static sentry_value_t BeforeSendHook(sentry_value_t event, void* /*hint*/, void* /*closure*/) {
    // Scrub the exception value (message / description)
    sentry_value_t exception = sentry_value_get_by_key(event, "exception");
    if (!sentry_value_is_null(exception)) {
        sentry_value_t values = sentry_value_get_by_key(exception, "values");
        size_t count = sentry_value_get_length(values);
        for (size_t i = 0; i < count; ++i) {
            sentry_value_t exc = sentry_value_get_by_index(values, i);

            // Scrub "value" field (exception message)
            sentry_value_t val = sentry_value_get_by_key(exc, "value");
            if (!sentry_value_is_null(val)) {
                const char* raw = sentry_value_as_string(val);
                if (raw) {
                    std::string scrubbed = ScrubString(raw);
                    sentry_value_set_by_key(exc, "value", sentry_value_new_string(scrubbed.c_str()));
                }
            }

            // Scrub "module" field (may contain paths)
            sentry_value_t mod = sentry_value_get_by_key(exc, "module");
            if (!sentry_value_is_null(mod)) {
                const char* raw = sentry_value_as_string(mod);
                if (raw) {
                    std::string scrubbed = ScrubString(raw);
                    sentry_value_set_by_key(exc, "module", sentry_value_new_string(scrubbed.c_str()));
                }
            }
        }
    }

    // Attach per-report correlation ID (not a persistent install ID)
    std::string corr_id = GenerateCorrelationId();
    sentry_value_set_by_key(event, "correlation_id", sentry_value_new_string(corr_id.c_str()));

    // Strip any user-context fields that may have leaked in
    sentry_value_remove_by_key(event, "user");

    // Strip breadcrumbs that may contain path data
    // (we do not populate breadcrumbs in this slice, but guard anyway)
    sentry_value_remove_by_key(event, "breadcrumbs");

    // Scrub tags: only allow-listed keys survive
    sentry_value_t tags = sentry_value_get_by_key(event, "tags");
    if (!sentry_value_is_null(tags)) {
        // Build a replacement tags object with only allowed keys
        sentry_value_t clean_tags = sentry_value_new_object();
        // sentry tags are stored as a dict; iterate by known allowed keys
        for (const auto& key : {"os.name", "os.version", "gpu.model", "gpu.vendor", "gpu.driver", "app.version",
                                "encoder_backend", "container", "video_codec", "audio_codec"}) {
            sentry_value_t v = sentry_value_get_by_key(tags, key);
            if (!sentry_value_is_null(v)) {
                // Scrub the value even for allowed keys
                const char* raw = sentry_value_as_string(v);
                if (raw) {
                    std::string scrubbed = ScrubString(raw);
                    sentry_value_set_by_key(clean_tags, key, sentry_value_new_string(scrubbed.c_str()));
                }
            }
        }
        sentry_value_set_by_key(event, "tags", clean_tags);
    }

    return event;
}
#endif // EXOSNAP_SENTRY_AVAILABLE

// ---------------------------------------------------------------------------
// ResolveCrashDir
// ---------------------------------------------------------------------------
std::string ResolveCrashDir() {
    // EXOSNAP_CONFIG_DIR override for test isolation
    wchar_t env_buf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"EXOSNAP_CONFIG_DIR", env_buf, MAX_PATH) > 0) {
        char narrow[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_UTF8, 0, env_buf, -1, narrow, sizeof(narrow), nullptr, nullptr);
        std::string base(narrow);
        // Normalize separator
        std::replace(base.begin(), base.end(), '/', '\\');
        return base + "\\crashes";
    }

    // Default: %LOCALAPPDATA%\ExoSnap\crashes
    wchar_t* local_appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_appdata))) {
        char narrow[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_UTF8, 0, local_appdata, -1, narrow, sizeof(narrow), nullptr, nullptr);
        CoTaskMemFree(local_appdata);
        return std::string(narrow) + "\\ExoSnap\\crashes";
    }

    // Fallback: LOCALAPPDATA env var
    wchar_t la_env[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la_env, MAX_PATH) > 0) {
        char narrow[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_UTF8, 0, la_env, -1, narrow, sizeof(narrow), nullptr, nullptr);
        return std::string(narrow) + "\\ExoSnap\\crashes";
    }

    return {};
}

// ---------------------------------------------------------------------------
// ResolveHandlerExePath
// ---------------------------------------------------------------------------
std::string ResolveHandlerExePath() {
    wchar_t module_path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return {};

    // Extract the directory
    std::wstring path(module_path, len);
    size_t last_sep = path.rfind(L'\\');
    if (last_sep == std::wstring::npos)
        return {};

    std::wstring dir = path.substr(0, last_sep + 1);
    std::wstring handler = dir + L"crashpad_handler.exe";

    char narrow[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, handler.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
    return std::string(narrow);
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
bool Initialize(const CrashCaptureConfig& config) {
    std::lock_guard lock(s_init_mutex);
    if (s_initialized)
        return true;

    // Store crash dir for sentinel use
    s_crash_dir = config.crash_dir;

    // Ensure crash dir exists
    if (!s_crash_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(s_crash_dir, ec);
        // Non-fatal; sentry will fail gracefully if the dir is missing
    }

#if EXOSNAP_SENTRY_AVAILABLE
    sentry_options_t* options = sentry_options_new();

    // DSN — only compiled in for official builds
#if defined(EXOSNAP_OFFICIAL_BUILD)
    // EU/.de ingest; write-only key (not a secret)
    sentry_options_set_dsn(
        options, "https://821c04d67576831b7c77efce2dc13bbc@o4511566018576384.ingest.de.sentry.io/4511566055931984");
#else
    // No DSN; local minidumps only, no network traffic
    sentry_options_set_dsn(options, "");
#endif

    // Release tag
    std::string release_tag = "exosnap@" + config.app_version;
    sentry_options_set_release(options, release_tag.c_str());

    // Crash dir (override sentry's default .sentry-native relative path)
    sentry_options_set_database_path(options, config.crash_dir.c_str());

    // Crashpad handler executable
    if (!config.handler_exe_path.empty()) {
        sentry_options_set_handler_path(options, config.handler_exe_path.c_str());
    }

    // Consent gate: nothing uploaded until GiveUserConsent() is called
    sentry_options_set_require_user_consent(options, 1);

    // Disable log output in release; debug mode only in dev builds
    sentry_options_set_debug(options, config.debug_mode ? 1 : 0);

    // Disable sentry's own logger to avoid log spam (CLAUDE.md: enable_logs(0))
    // sentry_options_set_logger is not always available; rely on debug=0

    // Before-send hook: scrub sensitive data
    sentry_options_set_before_send(options, BeforeSendHook, nullptr);

    int rc = sentry_init(options);
    if (rc != 0) {
        // Init failed — crash capture disabled, process continues normally
        return false;
    }
#endif // EXOSNAP_SENTRY_AVAILABLE

    s_initialized = true;
    return true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------
void Shutdown() {
    std::lock_guard lock(s_init_mutex);
    if (!s_initialized)
        return;

#if EXOSNAP_SENTRY_AVAILABLE
    sentry_close();
#endif

    s_initialized = false;
}

// ---------------------------------------------------------------------------
// Consent gate
// ---------------------------------------------------------------------------
void GiveUserConsent() {
#if EXOSNAP_SENTRY_AVAILABLE
    if (s_initialized)
        sentry_user_consent_give();
#endif
}

void RevokeUserConsent() {
#if EXOSNAP_SENTRY_AVAILABLE
    if (s_initialized)
        sentry_user_consent_revoke();
#endif
}

void SendTestEvent(std::string_view message) {
#if EXOSNAP_SENTRY_AVAILABLE
    if (s_initialized) {
        std::string msg(message);
        sentry_capture_event(sentry_value_new_message_event(SENTRY_LEVEL_INFO, "verify", msg.c_str()));
    }
#else
    (void)message;
#endif
}

// ---------------------------------------------------------------------------
// SetTag
// ---------------------------------------------------------------------------
void SetTag(std::string_view key, std::string_view value) {
    if (!IsAllowedTagKey(key))
        return; // silently drop non-allowlisted tags
#if EXOSNAP_SENTRY_AVAILABLE
    if (s_initialized) {
        std::string k(key);
        std::string scrubbed_val = ScrubString(value);
        sentry_set_tag(k.c_str(), scrubbed_val.c_str());
    }
#else
    (void)value; // unused in stub build
#endif
}

// ---------------------------------------------------------------------------
// SetEncoderContext
// ---------------------------------------------------------------------------
void SetEncoderContext(std::string_view encoder_backend, std::string_view container, std::string_view video_codec,
                       std::string_view audio_codec) {
    SetTag("encoder_backend", encoder_backend);
    SetTag("container", container);
    SetTag("video_codec", video_codec);
    SetTag("audio_codec", audio_codec);
}

// ---------------------------------------------------------------------------
// IsActive
// ---------------------------------------------------------------------------
bool IsActive() noexcept {
    return s_initialized;
}

// ---------------------------------------------------------------------------
// "Dump handled" sentinel coordination with Recovery
// ---------------------------------------------------------------------------
static std::string SentinelPath(const std::string& crash_dir) {
    return crash_dir + "\\dump_handled.json";
}

bool WriteDumpHandledSentinel(const std::string& crash_dir) {
    if (crash_dir.empty())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(crash_dir, ec);
    if (ec)
        return false;

    std::ofstream f(SentinelPath(crash_dir), std::ios::trunc);
    if (!f.is_open())
        return false;
    f << "{\"dump_handled\":true}\n";
    return f.good();
}

bool ReadAndClearDumpHandledSentinel(const std::string& crash_dir) {
    if (crash_dir.empty())
        return false;
    std::string path = SentinelPath(crash_dir);
    std::error_code ec;
    bool exists = std::filesystem::exists(path, ec);
    if (!exists || ec)
        return false;
    std::filesystem::remove(path, ec);
    return true; // it was present (we don't parse the JSON; presence is the signal)
}

// ---------------------------------------------------------------------------
// Session context + clean-exit detection
//
// The sidecar is hand-rolled JSON, matching the simplicity of the dump_handled
// sentinel above (no JSON library). All string values are scrubbed before
// being written. The reader is a tiny tolerant parser that looks for
// "key":"value" pairs and the "clean_exit":true|false flag.
// ---------------------------------------------------------------------------
static std::string SessionPath(const std::string& crash_dir) {
    return crash_dir + "\\last_session.json";
}

// Escape the characters JSON forbids in a string value. The values here are
// short identifiers (versions/codecs) but we escape defensively so a stray
// quote/backslash from a scrubbed value cannot corrupt the sidecar.
static std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

// Tolerant extractor for a string field: finds "key" then the next ':' then the
// quoted value, undoing the minimal escaping JsonEscape produces.
static std::optional<std::string> JsonFindString(const std::string& doc, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t kpos = doc.find(needle);
    if (kpos == std::string::npos)
        return std::nullopt;
    size_t colon = doc.find(':', kpos + needle.size());
    if (colon == std::string::npos)
        return std::nullopt;
    size_t open = doc.find('"', colon + 1);
    if (open == std::string::npos)
        return std::nullopt;
    std::string value;
    for (size_t i = open + 1; i < doc.size(); ++i) {
        char c = doc[i];
        if (c == '\\' && i + 1 < doc.size()) {
            char n = doc[i + 1];
            switch (n) {
            case 'n':
                value += '\n';
                break;
            case 'r':
                value += '\r';
                break;
            case 't':
                value += '\t';
                break;
            default:
                // Covers an escaped quote or an escaped backslash: emit the
                // following char verbatim.
                value += n;
                break;
            }
            ++i;
            continue;
        }
        if (c == '"')
            return value; // closing quote
        value += c;
    }
    return std::nullopt; // unterminated string
}

// Write the sidecar with clean_exit set to the given value. All context strings
// are scrubbed before serialization.
static bool WriteSession(const std::string& crash_dir, const SessionContext& ctx, bool clean_exit) {
    if (crash_dir.empty())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(crash_dir, ec);
    if (ec)
        return false;

    std::ofstream f(SessionPath(crash_dir), std::ios::trunc);
    if (!f.is_open())
        return false;

    f << "{\"clean_exit\":" << (clean_exit ? "true" : "false") << ",\"app_version\":\""
      << JsonEscape(ScrubString(ctx.app_version)) << "\""
      << ",\"encoder_backend\":\"" << JsonEscape(ScrubString(ctx.encoder_backend)) << "\""
      << ",\"container\":\"" << JsonEscape(ScrubString(ctx.container)) << "\""
      << ",\"video_codec\":\"" << JsonEscape(ScrubString(ctx.video_codec)) << "\""
      << ",\"audio_codec\":\"" << JsonEscape(ScrubString(ctx.audio_codec)) << "\""
      << "}\n";
    return f.good();
}

bool BeginSession(const std::string& crash_dir, const SessionContext& ctx) {
    return WriteSession(crash_dir, ctx, /*clean_exit=*/false);
}

bool UpdateSessionContext(const std::string& crash_dir, const SessionContext& ctx) {
    return WriteSession(crash_dir, ctx, /*clean_exit=*/false);
}

bool MarkCleanExit(const std::string& crash_dir) {
    if (crash_dir.empty())
        return false;
    std::string path = SessionPath(crash_dir);

    // Preserve the existing context fields; only flip clean_exit to true. If the
    // sidecar is missing (BeginSession was never called), write a minimal
    // clean record so a later read still reports a clean prior session.
    SessionContext ctx;
    std::ifstream in(path);
    if (in.is_open()) {
        std::stringstream ss;
        ss << in.rdbuf();
        std::string doc = ss.str();
        ctx.app_version = JsonFindString(doc, "app_version").value_or("");
        ctx.encoder_backend = JsonFindString(doc, "encoder_backend").value_or("");
        ctx.container = JsonFindString(doc, "container").value_or("");
        ctx.video_codec = JsonFindString(doc, "video_codec").value_or("");
        ctx.audio_codec = JsonFindString(doc, "audio_codec").value_or("");
    }
    return WriteSession(crash_dir, ctx, /*clean_exit=*/true);
}

std::optional<SessionContext> ReadPreviousCrashContext(const std::string& crash_dir) {
    if (crash_dir.empty())
        return std::nullopt;
    std::string path = SessionPath(crash_dir);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
        return std::nullopt;

    std::ifstream in(path);
    if (!in.is_open())
        return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string doc = ss.str();

    // Only a previous session that did NOT mark a clean exit is a crash.
    // Look for the literal token "clean_exit":false; anything else (true, or a
    // malformed/missing flag) is treated as "not a crash".
    bool is_crash = doc.find("\"clean_exit\":false") != std::string::npos;
    if (!is_crash)
        return std::nullopt;

    SessionContext ctx;
    ctx.app_version = JsonFindString(doc, "app_version").value_or("");
    ctx.encoder_backend = JsonFindString(doc, "encoder_backend").value_or("");
    ctx.container = JsonFindString(doc, "container").value_or("");
    ctx.video_codec = JsonFindString(doc, "video_codec").value_or("");
    ctx.audio_codec = JsonFindString(doc, "audio_codec").value_or("");
    return ctx;
}

} // namespace exosnap::crash_capture
