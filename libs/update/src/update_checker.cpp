// update_checker.cpp -- GitHub Releases API update check (WinHTTP).

#include <nlohmann/json.hpp>
#include <update/update_checker.h>

// WinHTTP is available on all supported Windows versions (Vista+).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <optional>
#include <sstream>
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

// Parse a tag name like "v1.2.3" or "1.2.3" into SemVer.
std::optional<SemVer> TagToSemVer(const std::string& tag) noexcept {
    std::string_view sv = tag;
    if (!sv.empty() && sv[0] == 'v')
        sv.remove_prefix(1);
    return ParseSemVer(sv);
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

    // Parse JSON array
    std::optional<SemVer> best_ver;
    std::optional<std::string> best_html_url;

    try {
        auto releases = nlohmann::json::parse(*body);
        for (const auto& rel : releases) {
            bool is_prerelease = rel.value("prerelease", false);
            bool is_draft = rel.value("draft", false);
            if (is_draft)
                continue;

            bool channel_match = (params.channel == UpdateChannel::Preview) ? is_prerelease : !is_prerelease;
            if (!channel_match)
                continue;

            auto tag = rel.value("tag_name", std::string{});
            auto sv = TagToSemVer(tag);
            if (!sv)
                continue;

            if (!best_ver || *sv > *best_ver) {
                best_ver = sv;
                best_html_url = rel.value("html_url", std::string{});
            }
        }
    } catch (...) {
        UpdateCheckResult r{};
        r.check_failed = true;
        r.error_message = "JSON parse error from GitHub releases API";
        return r;
    }

    UpdateCheckResult r{};
    r.check_failed = false;
    if (best_ver && *best_ver > params.current_version) {
        r.update_available = true;
        r.available_version = best_ver;
        r.releases_page_url = best_html_url;
    }
    return r;
}

} // namespace exosnap::update
