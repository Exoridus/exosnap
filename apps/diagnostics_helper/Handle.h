#pragma once

#include <windows.h>

#include <utility>

namespace exosnap::diagnostics::ipc {

class Handle {
  public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value == INVALID_HANDLE_VALUE ? nullptr : value) {
    }
    ~Handle() {
        reset();
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {
    }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }
    explicit operator bool() const noexcept {
        return value_ != nullptr;
    }
    void reset() noexcept {
        if (value_)
            CloseHandle(std::exchange(value_, nullptr));
    }

  private:
    HANDLE value_ = nullptr;
};

} // namespace exosnap::diagnostics::ipc
