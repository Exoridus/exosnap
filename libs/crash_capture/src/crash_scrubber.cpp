// crash_scrubber.cpp — Client-side event scrubbing for ExoSnap crash reports.
//
// All scrubbing is pure string manipulation — no sentry.h dependency.
// This keeps the scrubber independently testable.

#include <crash_capture/crash_scrubber.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

// Windows headers required for username / machine name resolution.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <lmcons.h> // UNLEN
#include <shlobj.h> // SHGetKnownFolderPath
#include <windows.h>

namespace exosnap::crash_capture {

// ---------------------------------------------------------------------------
// Allow-listed tag keys (structured context allowed through before_send).
// Any tag whose key does NOT appear here is stripped before upload.
// ---------------------------------------------------------------------------
static constexpr std::array<std::string_view, 10> kAllowedTagKeys = {
    "os.name",     "os.version",      "gpu.model", "gpu.vendor",  "gpu.driver",
    "app.version", "encoder_backend", "container", "video_codec", "audio_codec",
};

bool IsAllowedTagKey(std::string_view key) {
    for (auto& allowed : kAllowedTagKeys) {
        if (key == allowed)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// SensitiveValueCache implementation
// ---------------------------------------------------------------------------
static std::mutex s_cache_mutex;
static SensitiveValueCache* s_instance = nullptr;

SensitiveValueCache& SensitiveValueCache::Instance() {
    std::lock_guard lock(s_cache_mutex);
    if (!s_instance) {
        s_instance = new SensitiveValueCache();
        s_instance->Refresh();
    }
    return *s_instance;
}

void SensitiveValueCache::Reset() {
    std::lock_guard lock(s_cache_mutex);
    delete s_instance;
    s_instance = nullptr;
}

static std::string WideToUtf8(const wchar_t* wide) {
    if (!wide || wide[0] == L'\0')
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return {};
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), len, nullptr, nullptr);
    return result;
}

void SensitiveValueCache::Refresh() {
    // Username
    wchar_t username_buf[UNLEN + 1] = {};
    DWORD username_len = UNLEN + 1;
    if (GetUserNameW(username_buf, &username_len)) {
        username = WideToUtf8(username_buf);
    }

    // Machine name
    wchar_t machine_buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD machine_len = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(machine_buf, &machine_len)) {
        machine_name = WideToUtf8(machine_buf);
    }

    // USERPROFILE (home directory, e.g. C:\Users\Alice)
    wchar_t* profile_path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile_path))) {
        userprofile = WideToUtf8(profile_path);
        CoTaskMemFree(profile_path);
    }
    // Fallback: USERPROFILE environment variable
    if (userprofile.empty()) {
        wchar_t env_buf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", env_buf, MAX_PATH)) {
            userprofile = WideToUtf8(env_buf);
        }
    }
}

// ---------------------------------------------------------------------------
// Case-insensitive substring replacement helper.
// Replaces all occurrences of `needle` in `haystack` with `replacement`.
// Returns the modified string.
// ---------------------------------------------------------------------------
static std::string ReplaceAllCI(std::string haystack, std::string_view needle, std::string_view replacement) {
    if (needle.empty())
        return haystack;

    std::string lower_hay = haystack;
    std::transform(lower_hay.begin(), lower_hay.end(), lower_hay.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string lower_needle(needle);
    std::transform(lower_needle.begin(), lower_needle.end(), lower_needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string result;
    result.reserve(haystack.size());
    size_t pos = 0;
    while (true) {
        size_t found = lower_hay.find(lower_needle, pos);
        if (found == std::string::npos) {
            result.append(haystack, pos, std::string::npos);
            break;
        }
        result.append(haystack, pos, found - pos);
        result.append(replacement);
        pos = found + needle.size();
    }
    return result;
}

// ---------------------------------------------------------------------------
// Strip absolute Windows paths using a regex.
//
// Matches:
//   C:\something  (drive letter + backslash)
//   \\server\share (UNC path)
// ---------------------------------------------------------------------------
static std::string StripWindowsPaths(const std::string& input) {
    // Match drive paths (C:\...) and UNC paths (\\...)
    // We use a simple approach: find occurrences that look like paths and
    // replace up to the next whitespace or end of string.
    static const std::regex kPathRegex(R"([A-Za-z]:\\[^\s\"\'\,\;\n\r]*|\\\\[^\s\"\'\,\;\n\r]+)",
                                       std::regex::ECMAScript | std::regex::optimize);
    return std::regex_replace(input, kPathRegex, "[path]");
}

// ---------------------------------------------------------------------------
// ScrubString — main entry point.
// Order of operations:
//   1. Strip USERPROFILE path (long paths first, before username replacement
//      would corrupt the path match).
//   2. Strip absolute Windows paths generically.
//   3. Strip username (appears in paths, but may also appear standalone).
//   4. Strip machine name.
// ---------------------------------------------------------------------------
std::string ScrubString(std::string_view input) {
    if (input.empty())
        return {};

    std::string result(input);
    const auto& cache = SensitiveValueCache::Instance();

    // Step 1: Replace full USERPROFILE path before generic path stripping
    // so we get the "[user]" placeholder rather than "[path]".
    if (!cache.userprofile.empty()) {
        result = ReplaceAllCI(result, cache.userprofile, "[path]");
    }

    // Step 2: Generic Windows path stripping (C:\..., \\...)
    result = StripWindowsPaths(result);

    // Step 3: Standalone username (after path stripping to avoid double-replace)
    if (!cache.username.empty()) {
        result = ReplaceAllCI(result, cache.username, "[user]");
    }

    // Step 4: Machine name
    if (!cache.machine_name.empty()) {
        result = ReplaceAllCI(result, cache.machine_name, "[machine]");
    }

    return result;
}

// ---------------------------------------------------------------------------
// GenerateCorrelationId — UUID v4 (random), lowercase hex, no dashes.
// Not persistent; regenerated per crash report.
// ---------------------------------------------------------------------------
std::string GenerateCorrelationId() {
    static std::mutex s_rng_mutex;
    std::lock_guard lock(s_rng_mutex);

    static std::mt19937_64 s_rng([]() -> uint64_t {
        std::random_device rd;
        uint64_t seed = 0;
        seed |= (uint64_t)rd() << 32;
        seed |= (uint64_t)rd();
        return seed;
    }());

    // Generate 16 random bytes
    uint64_t hi = s_rng();
    uint64_t lo = s_rng();

    // Format as UUID v4: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    // Set version (4) and variant (10xx)
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buf[37];
    snprintf(buf, sizeof(buf), "%08llx-%04llx-%04llx-%04llx-%012llx", (unsigned long long)(hi >> 32),
             (unsigned long long)((hi >> 16) & 0xFFFF), (unsigned long long)(hi & 0xFFFF),
             (unsigned long long)(lo >> 48), (unsigned long long)(lo & 0x0000FFFFFFFFFFFFULL));
    return std::string(buf);
}

} // namespace exosnap::crash_capture
