// update_checker.cpp -- GitHub Releases API update check (WinHTTP).

#include <update/release_locator.h>
#include <update/update_checker.h>

// WinHTTP is available on all supported Windows versions (Vista+).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <chrono>
#include <optional>
#include <string>

namespace exosnap::update {
namespace {

// Per-operation WinHTTP deadlines. These bound each individual step; the
// wall-clock guard on the read loop below is what bounds the total, because a
// server that trickles one byte inside every receive window would otherwise
// keep the per-read timeout from ever firing.
constexpr int kResolveTimeoutMs = 10'000;
constexpr int kConnectTimeoutMs = 15'000;
constexpr int kSendTimeoutMs = 15'000;
constexpr int kReceiveTimeoutMs = 30'000;
// Total budget for reading the response body. An update check is a few hundred
// KB of JSON; anything past this is a stalled connection, not a slow one.
constexpr auto kBodyReadBudget = std::chrono::seconds(60);

// Perform a simple HTTPS GET and return the response body, or nullopt on failure.
std::optional<std::string> HttpsGet(std::wstring_view host, std::wstring_view path, INTERNET_PORT port,
                                    std::string& out_error) noexcept {
    HINTERNET session = WinHttpOpen(L"ExoSnap-UpdateChecker/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        out_error = "WinHttpOpen failed";
        return std::nullopt;
    }

    // Without this WinHTTP applies its own defaults, and the resolve timeout
    // among them is INFINITE -- a black-holed DNS server blocks the caller
    // forever. The caller is UpdateService, whose thread pool is joined on the
    // GUI thread at shutdown, so an unbounded GET here is an application that
    // will not close, with no progress indication and no cancel affordance.
    WinHttpSetTimeouts(session, kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs);

    HINTERNET conn = WinHttpConnect(session, std::wstring(host).c_str(), port, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        out_error = "WinHttpConnect failed";
        return std::nullopt;
    }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", std::wstring(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        out_error = "WinHttpOpenRequest failed";
        return std::nullopt;
    }

    // Request JSON
    BOOL sent = WinHttpSendRequest(req,
                                   L"Accept: application/vnd.github+json\r\n"
                                   L"X-GitHub-Api-Version: 2022-11-28\r\n",
                                   static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(req, nullptr)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        out_error = "WinHttp send/receive failed";
        return std::nullopt;
    }

    // Check HTTP status
    DWORD status = 0;
    DWORD sz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        out_error = "HTTP " + std::to_string(status);
        return std::nullopt;
    }

    std::string body;
    DWORD avail = 0;
    // The per-read timeout above is per operation, so a server dripping a byte
    // per window would keep this loop alive indefinitely. The total budget is
    // what actually terminates that case.
    const auto read_deadline = std::chrono::steady_clock::now() + kBodyReadBudget;
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
        if (std::chrono::steady_clock::now() > read_deadline) {
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            WinHttpCloseHandle(session);
            out_error = "Update server response timed out";
            return std::nullopt;
        }
        std::string chunk(avail, '\0');
        DWORD read = 0;
        WinHttpReadData(req, chunk.data(), avail, &read);
        body.append(chunk.data(), read);
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return body;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// FetchReleasesJson
// ---------------------------------------------------------------------------
std::optional<std::string> FetchReleasesJson(const std::string& base_url, std::string& out_error) noexcept {
    // Split "https://host[:port]/path" for WinHTTP. https-only by design
    // (mirrors DownloadToFile); the URLs here are ASCII (GitHub API / dev
    // overrides), so a plain char->wchar widen is sufficient.
    constexpr std::string_view kScheme = "https://";
    if (base_url.compare(0, kScheme.size(), kScheme) != 0) {
        out_error = "base URL must be https://";
        return std::nullopt;
    }

    const std::string rest = base_url.substr(kScheme.size());
    const size_t slash = rest.find('/');
    std::string host_port = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string path = (slash == std::string::npos) ? std::string("/") : rest.substr(slash);

    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    if (const size_t colon = host_port.find(':'); colon != std::string::npos) {
        const std::string port_str = host_port.substr(colon + 1);
        host_port.resize(colon);
        unsigned long parsed = 0;
        for (const char ch : port_str) {
            if (ch < '0' || ch > '9') {
                parsed = 0;
                break;
            }
            parsed = parsed * 10 + static_cast<unsigned long>(ch - '0');
            if (parsed > 65535) {
                break; // out of range already -- stop before the value wraps
            }
        }
        if (port_str.empty() || parsed == 0 || parsed > 65535) {
            out_error = "invalid port in base URL";
            return std::nullopt;
        }
        port = static_cast<INTERNET_PORT>(parsed);
    }
    if (host_port.empty()) {
        out_error = "missing host in base URL";
        return std::nullopt;
    }

    // First page only; the newest qualifying release is always near the top.
    path += (path.find('?') == std::string::npos) ? "?per_page=30" : "&per_page=30";

    const std::wstring whost(host_port.begin(), host_port.end());
    const std::wstring wpath(path.begin(), path.end());
    return HttpsGet(whost, wpath, port, out_error);
}

// ---------------------------------------------------------------------------
// DecideOffer
// ---------------------------------------------------------------------------
UpdateOffer DecideOffer(const SemVer& release_version, std::string_view release_version_raw,
                        const CheckParams& params) noexcept {
    // A genuinely newer release is always a normal update, in every mode.
    if (release_version > params.current_version)
        return UpdateOffer::Update;

    // Verification reinstall (ADR 0055): only the byte-identical version, and
    // only when the mode is explicitly on for this app run. Everything else --
    // in particular anything older -- stays invisible.
    if (params.allow_same_version_reinstall && !params.current_version_raw.empty() && !release_version_raw.empty() &&
        release_version_raw == params.current_version_raw)
        return UpdateOffer::VerificationReinstall;

    return UpdateOffer::None;
}

// ---------------------------------------------------------------------------
// BuildCheckResult
// ---------------------------------------------------------------------------
UpdateCheckResult BuildCheckResult(std::string_view releases_json, const std::optional<ReleaseAssets>& release,
                                   const CheckParams& params) noexcept {
    UpdateCheckResult r{};
    r.check_failed = false;

    // Reference list for the pre-update "See what's new" link: every non-draft release
    // on this channel, independent of whether an update is offered.
    r.all_channel_notes = CollectAllReleaseNotesForChannel(releases_json, params.channel);

    // The trust-anchor assets of whichever release qualified, offered or not.
    // LocateRelease only returns a release that carries BOTH, so a located
    // release always fills both fields; nothing else in the result depends on
    // them, which is why they are set before the offer decision.
    if (release) {
        r.manifest_url = release->manifest_url;
        r.manifest_signature_url = release->signature_url;
    }

    const UpdateOffer offer = release ? DecideOffer(release->version, release->version_tag, params) : UpdateOffer::None;
    if (offer != UpdateOffer::None) {
        r.update_available = true;
        r.verification_reinstall = (offer == UpdateOffer::VerificationReinstall);
        r.available_version = release->version;
        // The tag verbatim, not SemVer::ToString(): the target-version pin the
        // app hands the updater has to be the string the signed manifest carries.
        r.available_version_raw = release->version_tag;
        r.releases_page_url = release->releases_page_url;
        // Gap-aware What's-new notes: every release in (current, best] for this
        // channel, newest first -- read from the SAME fetched JSON (no extra call).
        // A verification reinstall spans an empty range (current == target), so
        // the notes stay empty by construction -- there is nothing "new" to show.
        if (offer == UpdateOffer::Update)
            r.gap_notes = CollectReleaseNotes(releases_json, params.current_version, release->version, params.channel);
    }
    return r;
}

// ---------------------------------------------------------------------------
// CheckAgainstFeed -- the mechanics, without any policy
// ---------------------------------------------------------------------------
UpdateCheckResult CheckAgainstFeed(const CheckParams& params) noexcept {
    // Fetch the first releases page (30 items) and pick the right channel.
    // Honours params.api_base_url so tests / dev servers can redirect the call.
    std::string http_error;
    auto body = FetchReleasesJson(params.api_base_url, http_error);
    if (!body) {
        UpdateCheckResult r{};
        r.check_failed = true;
        r.error_message = "Network error: " + http_error;
        return r;
    }

    // Select the newest qualifying release for the channel. The channel/draft
    // filtering and asset extraction live once in LocateRelease (DRY).
    std::string parse_error;
    auto release = LocateRelease(*body, params.channel, &parse_error);
    if (!release && !parse_error.empty()) {
        UpdateCheckResult r{};
        r.check_failed = true;
        r.error_message = "JSON parse error from GitHub releases API";
        return r;
    }

    return BuildCheckResult(*body, release, params);
}

// ---------------------------------------------------------------------------
// CheckForUpdate -- the product policy, then the mechanics
// ---------------------------------------------------------------------------
UpdateCheckResult CheckForUpdate(const CheckParams& params) noexcept {
    // Recording guard runs first — must block regardless of build gate,
    // so the guard contract is enforced even in unofficial (non-network) builds.
    if (params.recording_guard) {
        auto reason = params.recording_guard();
        if (reason != UpdateBlockReason::NotBlocked) {
            UpdateCheckResult r{};
            r.check_failed = true;
            r.error_message = (reason == UpdateBlockReason::ActiveRecording)
                                  ? "Update check blocked: recording in progress"
                                  : "Update check blocked: recording finalizing";
            return r;
        }
    }

    // Compile-time gate (after recording guard so the guard is always enforced).
    if (!IsUpdateCheckEnabled()) {
        UpdateCheckResult r{};
        r.check_failed = true;
        r.error_message = "Update checking disabled (unofficial build)";
        return r;
    }

    return CheckAgainstFeed(params);
}

} // namespace exosnap::update
