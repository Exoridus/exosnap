#pragma once

#include <windows.h>

#include <string>

namespace exosnap::update {

inline constexpr wchar_t kRuntimeSwapLockFile[] = L".runtime-swap.lock";

// The root stays in place while runtime is renamed. An exclusive file handle
// also coordinates restores on a share and closes when its process exits.
class RuntimeSwapLock {
  public:
    explicit RuntimeSwapLock(const std::wstring& package_root)
        : handle_(CreateFileW((package_root + L"\\" + kRuntimeSwapLockFile).c_str(), DELETE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr)) {
    }

    ~RuntimeSwapLock() {
        if (*this)
            CloseHandle(handle_);
    }

    RuntimeSwapLock(const RuntimeSwapLock&) = delete;
    RuntimeSwapLock& operator=(const RuntimeSwapLock&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE handle_;
};

} // namespace exosnap::update
