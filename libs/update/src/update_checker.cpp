// update_checker.cpp -- GitHub Releases API update check (WinHTTP).

#include <update/release_locator.h>
#include <update/update_checker.h>

// WinHTTP is available on all supported Windows versions (Vista+).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <optional>
#include <string>

namespace exosnap::update {
namespace {

// Perform a simple HTTPS GET and return the response body, or nullopt on failure.
std::optional<std::string> HttpsGet(std::wstring_view host, std::wstring_view path, std::string& out_error) noexcept {
    HINTERNET session = WinHttpOpen(L"ExoSnap-UpdateChecker/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        out_error = "WinHttpOpen failed";
        return std::nullopt;
    }

    HINTERNET conn = WinHttpConnect(session, std::wstring(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
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
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
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
// CheckForUpdate
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

    // Build API URL: /releases for all, then filter client-side
    // api_base_url = "https://api.github.com/repos/Exoridus/exosnap/releases"
    // We fetch the first page (30 items) and pick the right channel.
    const std::wstring host = L"api.github.com";
    const std::wstring path = L"/repos/Exoridus/exosnap/releases?per_page=30";

    std::string http_error;
    auto body = HttpsGet(host, path, http_error);
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

    UpdateCheckResult r{};
    r.check_failed = false;
    if (release && release->version > params.current_version) {
        r.update_available = true;
        r.available_version = release->version;
        r.releases_page_url = release->releases_page_url;
        // Gap-aware What's-new notes: every release in (current, best] for this
        // channel, newest first — read from the SAME fetched JSON (no extra call).
        r.gap_notes = CollectReleaseNotes(*body, params.current_version, release->version, params.channel);
    }
    return r;
}

} // namespace exosnap::update
