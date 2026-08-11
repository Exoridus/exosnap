#include "ScreenPresentation.h"

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <windows.h>
#endif

namespace exosnap {

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

} // namespace exosnap
