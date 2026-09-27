#include "ScreenPresentation.h"

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <dwmapi.h>
#include <windows.h>
#endif

namespace exosnap {

namespace {

WindowMonitorFunction& windowMonitorOverride() {
    static WindowMonitorFunction fn;
    return fn;
}

WindowRectFunction& windowRectOverride() {
    static WindowRectFunction fn;
    return fn;
}

} // namespace

ScreenPresentation QueryScreenPresentation(std::uintptr_t native_id) {
    ScreenPresentation meta;

#if defined(_WIN32)
    const auto monitor = reinterpret_cast<HMONITOR>(native_id);
    if (monitor == nullptr) {
        return meta;
    }

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        // A monitor that was unplugged between enumeration and this call. Not an
        // error: the caller falls back to leaving the overlay where it was.
        return meta;
    }

    meta.available = true;
    meta.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    meta.width = info.rcMonitor.right - info.rcMonitor.left;
    meta.height = info.rcMonitor.bottom - info.rcMonitor.top;
    meta.origin_x = info.rcMonitor.left;
    meta.origin_y = info.rcMonitor.top;
#else
    (void)native_id;
#endif

    return meta;
}

std::uintptr_t ResolveTargetMonitor(bool is_window_target, std::uintptr_t native_id) {
    if (!is_window_target)
        return native_id;
    if (const WindowMonitorFunction& fn = windowMonitorOverride(); fn)
        return fn(native_id);
#if defined(_WIN32)
    const HMONITOR monitor = MonitorFromWindow(reinterpret_cast<HWND>(native_id), MONITOR_DEFAULTTONEAREST);
    return reinterpret_cast<std::uintptr_t>(monitor);
#else
    return native_id;
#endif
}

void SetWindowMonitorFunctionForTest(WindowMonitorFunction fn) {
    windowMonitorOverride() = std::move(fn);
}

void ResetWindowMonitorFunctionForTest() {
    windowMonitorOverride() = WindowMonitorFunction();
}

WindowScreenRect QueryWindowScreenRect(std::uintptr_t hwnd) {
    if (const WindowRectFunction& fn = windowRectOverride(); fn)
        return fn(hwnd);

    WindowScreenRect rect;
#if defined(_WIN32)
    const auto handle = reinterpret_cast<HWND>(hwnd);
    if (handle == nullptr || !IsWindow(handle))
        return rect;

    RECT bounds{};
    if (FAILED(DwmGetWindowAttribute(handle, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
        return rect;

    rect.available = true;
    rect.x = bounds.left;
    rect.y = bounds.top;
    rect.width = bounds.right - bounds.left;
    rect.height = bounds.bottom - bounds.top;
#else
    (void)hwnd;
#endif
    return rect;
}

void SetWindowRectFunctionForTest(WindowRectFunction fn) {
    windowRectOverride() = std::move(fn);
}

void ResetWindowRectFunctionForTest() {
    windowRectOverride() = WindowRectFunction();
}

} // namespace exosnap
