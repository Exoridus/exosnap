#pragma once

#include "GlobalHotkeyService.h"

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace exosnap {

#if defined(Q_OS_WIN)
// Win32 registrar — wraps a live HWND. Extracted from MainWindow so both the
// Widgets frontend and the Qt Quick frontend register global hotkeys through
// one implementation; the HWND owner differs, the Win32 calls do not.
class Win32HotkeyRegistrar : public IHotkeyRegistrar {
  public:
    explicit Win32HotkeyRegistrar(HWND hwnd) : hwnd_(hwnd) {
    }

    bool Register(int id, unsigned int modifiers, unsigned int vk) override {
        return ::RegisterHotKey(hwnd_, id, static_cast<UINT>(modifiers), static_cast<UINT>(vk)) != FALSE;
    }

    void Unregister(int id) override {
        ::UnregisterHotKey(hwnd_, id);
    }

    [[nodiscard]] HWND Hwnd() const noexcept {
        return hwnd_;
    }

  private:
    HWND hwnd_ = nullptr;
};
#endif

} // namespace exosnap
