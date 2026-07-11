// http_download.cpp -- streaming HTTPS download over WinHTTP (progress, cancel).

#include <update/http_download.h>

#include "url_utils.h"

// WinHTTP is available on all supported Windows versions (Vista+).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <cstdlib>
#include <iterator>
#include <string>

namespace exosnap::update {
namespace {

constexpr DWORD kChunkSize = 64 * 1024; // 64 KiB

// RAII guard for a WinHTTP handle so every early-return path stays leak-free
// without repeating WinHttpCloseHandle at each error site (mirrors the guard
// idiom used by HttpsGet in update_checker.cpp).
class HInternetGuard {
  public:
    explicit HInternetGuard(HINTERNET h = nullptr) noexcept : handle_(h) {
    }
    ~HInternetGuard() {
        if (handle_)
            WinHttpCloseHandle(handle_);
    }
    HInternetGuard(const HInternetGuard&) = delete;
    HInternetGuard& operator=(const HInternetGuard&) = delete;

    HINTERNET get() const noexcept {
        return handle_;
    }
    explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

  private:
    HINTERNET handle_;
};

// Deletes the partial destination file, if any, and returns the given error.
std::optional<std::string> FailWithCleanup(const std::wstring& dest_path, const std::string& error) {
    DeleteFileW(dest_path.c_str());
    return error;
}

} // anonymous namespace

std::optional<std::string> DownloadToFile(const std::string& url, const std::wstring& dest_path,
                                          const DownloadProgressFn& progress, const std::atomic<bool>& cancel) {
    // Checked before any network/file work so a pre-set cancel needs neither
    // a connection nor a partial file on disk.
    if (cancel.load())
        return FailWithCleanup(dest_path, "canceled");

    // Widen the URL for WinHttpCrackUrl (it operates on wide strings). Must be a
    // real UTF-8 -> UTF-16 conversion, not byte-wise widening: a non-ASCII path
    // segment is multiple UTF-8 bytes that byte-wise widening would turn into
    // multiple garbled wide chars instead of the one correct code unit.
    std::wstring wide_url = Utf8ToWide(url);

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = path;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &components))
        return FailWithCleanup(dest_path, "malformed URL");

    if (components.nScheme != INTERNET_SCHEME_HTTPS)
        return FailWithCleanup(dest_path, "only https:// URLs are supported");

    // Re-crack with a path buffer that also captures any query/extra info, since
    // WinHttpOpenRequest needs the full path+query as one string.
    std::wstring full_path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.lpszExtraInfo && components.dwExtraInfoLength > 0)
        full_path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    if (full_path.empty())
        full_path = L"/";

    HInternetGuard session(WinHttpOpen(L"ExoSnap-Updater/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
        return FailWithCleanup(dest_path, "WinHttpOpen failed");

    HInternetGuard conn(WinHttpConnect(session.get(), components.lpszHostName, components.nPort, 0));
    if (!conn)
        return FailWithCleanup(dest_path, "WinHttpConnect failed");

    HInternetGuard req(WinHttpOpenRequest(conn.get(), L"GET", full_path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!req)
        return FailWithCleanup(dest_path, "WinHttpOpenRequest failed");

    if (cancel.load())
        return FailWithCleanup(dest_path, "canceled");

    BOOL sent = WinHttpSendRequest(req.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(req.get(), nullptr))
        return FailWithCleanup(dest_path, "WinHttp send/receive failed");

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300)
        return FailWithCleanup(dest_path, "HTTP " + std::to_string(status));

    // Content-Length is optional; when absent bytes_total stays 0.
    uint64_t content_length = 0;
    wchar_t length_buf[32]{};
    DWORD length_buf_size = sizeof(length_buf);
    if (WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length_buf,
                            &length_buf_size, WINHTTP_NO_HEADER_INDEX)) {
        content_length = _wcstoui64(length_buf, nullptr, 10);
    }

    if (cancel.load())
        return FailWithCleanup(dest_path, "canceled");

    HANDLE file =
        CreateFileW(dest_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return FailWithCleanup(dest_path, "failed to open destination file");

    DownloadProgress dl_progress{};
    dl_progress.bytes_total = content_length;

    std::string chunk;
    chunk.resize(kChunkSize);

    for (;;) {
        if (cancel.load()) {
            CloseHandle(file);
            return FailWithCleanup(dest_path, "canceled");
        }

        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req.get(), &available)) {
            CloseHandle(file);
            return FailWithCleanup(dest_path, "WinHttpQueryDataAvailable failed");
        }
        if (available == 0)
            break; // end of response body

        DWORD to_read = available < kChunkSize ? available : kChunkSize;
        DWORD bytes_read = 0;
        if (!WinHttpReadData(req.get(), chunk.data(), to_read, &bytes_read)) {
            CloseHandle(file);
            return FailWithCleanup(dest_path, "WinHttpReadData failed");
        }
        if (bytes_read == 0)
            break;

        if (cancel.load()) {
            CloseHandle(file);
            return FailWithCleanup(dest_path, "canceled");
        }

        DWORD bytes_written = 0;
        if (!WriteFile(file, chunk.data(), bytes_read, &bytes_written, nullptr) || bytes_written != bytes_read) {
            CloseHandle(file);
            return FailWithCleanup(dest_path, "failed to write destination file");
        }

        dl_progress.bytes_received += bytes_written;
        if (progress)
            progress(dl_progress);
    }

    CloseHandle(file);
    return std::nullopt;
}

} // namespace exosnap::update
