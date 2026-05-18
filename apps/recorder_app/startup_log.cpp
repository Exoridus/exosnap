#include "startup_log.h"

#include <windows.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace exosnap::startup_log {
namespace {

std::mutex& LogMutex() {
    static std::mutex m;
    return m;
}

std::wstring LogPath() {
    std::array<wchar_t, MAX_PATH> temp{};
    auto len = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    std::wstring base = (len > 0 && len < temp.size()) ? std::wstring(temp.data(), len) : L".\\";
    return base + L"exosnap-recorder-app-startup.log";
}

std::wstring Timestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::wostringstream oss;
    oss << std::setfill(L'0')
        << std::setw(2) << st.wHour << L":"
        << std::setw(2) << st.wMinute << L":"
        << std::setw(2) << st.wSecond << L"."
        << std::setw(3) << st.wMilliseconds;
    return oss.str();
}

void Append(std::wstring const& message) noexcept {
    try {
        std::lock_guard<std::mutex> lock(LogMutex());
        std::wofstream out(LogPath(), std::ios::app);
        if (out.is_open()) {
            out << L"[" << Timestamp() << L"] " << message << L"\n";
            out.flush();
        }
        std::wstring dbg = L"[recorder_app] " + message + L"\n";
        OutputDebugStringW(dbg.c_str());
    } catch (...) {
        // Best-effort diagnostics only.
    }
}

} // namespace

void Write(std::wstring_view message) noexcept {
    Append(std::wstring(message));
}

void WriteNarrow(char const* message) noexcept {
    if (!message) {
        Append(L"(null)");
        return;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, message, -1, nullptr, 0);
    if (n <= 0) {
        Append(L"(failed to decode narrow message)");
        return;
    }
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, message, -1, wide.data(), n);
    if (!wide.empty() && wide.back() == L'\0') {
        wide.pop_back();
    }
    Append(wide);
}

void WriteHResult(char const* context, winrt::hresult_error const& ex) noexcept {
    std::ostringstream oss;
    oss << context << " hresult=0x" << std::hex << std::uppercase << static_cast<uint32_t>(ex.code().value);
    WriteNarrow(oss.str().c_str());
    Write(ex.message().c_str());
}

} // namespace exosnap::startup_log
