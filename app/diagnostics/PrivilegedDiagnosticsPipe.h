#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace exosnap::diagnostics::privileged {

inline constexpr wchar_t kPipePrefix[] = LR"(\\.\pipe\ExoSnapDiagnostics.)";
inline constexpr DWORD kIoTimeoutMs = 5000;
inline constexpr DWORD kPipeClientAccess = FILE_READ_DATA | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;

class Handle {
  public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {
    }
    ~Handle() {
        if (*this)
            ::CloseHandle(value_);
    }
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {
    }
    Handle& operator=(Handle&& other) noexcept {
        std::swap(value_, other.value_);
        return *this;
    }
    explicit operator bool() const {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE get() const {
        return value_;
    }

  private:
    HANDLE value_ = nullptr;
};

[[nodiscard]] inline bool IsPipeNonce(std::wstring_view value) {
    return value.size() == 32 && value.find_first_not_of(L"0123456789abcdef") == std::wstring_view::npos;
}

[[nodiscard]] inline std::wstring NewNonce() {
    std::array<BYTE, 16> bytes{};
    if (::BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) !=
        0)
        return {};
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(32);
    for (const BYTE value : bytes) {
        result += digits[value >> 4];
        result += digits[value & 0x0f];
    }
    return result;
}

[[nodiscard]] inline uint64_t QpcNow() {
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

[[nodiscard]] inline int64_t QpcFrequency() {
    LARGE_INTEGER frequency{};
    ::QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart;
}

[[nodiscard]] inline uint64_t ProcessBirth(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!::GetProcessTimes(process, &created, &exited, &kernel, &user))
        return 0;
    return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

[[nodiscard]] inline bool SameSession(DWORD peer_pid) {
    DWORD ours = 0, theirs = 0;
    return ::ProcessIdToSessionId(::GetCurrentProcessId(), &ours) && ::ProcessIdToSessionId(peer_pid, &theirs) &&
           ours != 0 && ours == theirs;
}

[[nodiscard]] inline Handle OpenPipeClient(const std::wstring& name) {
    Handle pipe(::CreateFileW(name.c_str(), kPipeClientAccess, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!pipe || !::SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr))
        return {};
    return pipe;
}

[[nodiscard]] inline bool WaitIo(HANDLE pipe, OVERLAPPED& operation, HANDLE peer, HANDLE cancel, DWORD& bytes) {
    HANDLE signals[]{operation.hEvent, peer, cancel};
    const DWORD count = cancel ? 3 : peer ? 2 : 1;
    if (::WaitForMultipleObjects(count, signals, FALSE, kIoTimeoutMs) == WAIT_OBJECT_0)
        return ::GetOverlappedResult(pipe, &operation, &bytes, FALSE) != FALSE;
    // Cancellation must complete before the kernel can no longer reference the
    // frame and OVERLAPPED stored in the caller's stack.
    ::CancelIoEx(pipe, &operation);
    ::GetOverlappedResult(pipe, &operation, &bytes, TRUE);
    return false;
}

[[nodiscard]] inline bool Connect(HANDLE pipe, HANDLE peer, HANDLE cancel) {
    if (cancel && ::WaitForSingleObject(cancel, 0) != WAIT_TIMEOUT)
        return false;
    const Handle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        return false;
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    if (::ConnectNamedPipe(pipe, &operation))
        return true;
    const DWORD error = ::GetLastError();
    DWORD ignored = 0;
    return error == ERROR_PIPE_CONNECTED ||
           (error == ERROR_IO_PENDING && WaitIo(pipe, operation, peer, cancel, ignored));
}

template <class Frame>
[[nodiscard]] bool Transfer(HANDLE pipe, Frame& frame, bool write, HANDLE peer, HANDLE cancel = nullptr) {
    if (cancel && ::WaitForSingleObject(cancel, 0) != WAIT_TIMEOUT)
        return false;
    const Handle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        return false;
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD bytes = 0;
    const auto size = static_cast<DWORD>(sizeof(Frame));
    const BOOL done = write ? ::WriteFile(pipe, &frame, size, &bytes, &operation)
                            : ::ReadFile(pipe, &frame, size, &bytes, &operation);
    if (!done && (::GetLastError() != ERROR_IO_PENDING || !WaitIo(pipe, operation, peer, cancel, bytes)))
        return false;
    return bytes == size;
}

} // namespace exosnap::diagnostics::privileged
