#include "WindowPresencePolicy.h"

namespace exosnap {

MinimizeOutcome EvaluateMinimize(bool minimize_to_tray, bool tray_available) noexcept {
    if (!minimize_to_tray)
        return MinimizeOutcome::Taskbar;
    // Without a notification area there is no way back to a hidden window, so
    // the preference is honoured only where the restore path exists.
    return tray_available ? MinimizeOutcome::HideToTray : MinimizeOutcome::Taskbar;
}

bool IsMinimizeSysCommand(quint64 wparam) noexcept {
    constexpr quint64 kScMinimize = 0xF020u;
    constexpr quint64 kSysCommandMask = 0xFFF0u;
    return (wparam & kSysCommandMask) == kScMinimize;
}

quint32 DesiredWindowCaptureAffinity(bool hide_window_from_capture) noexcept {
    return hide_window_from_capture ? kWindowAffinityExcludeFromCapture : kWindowAffinityNone;
}

} // namespace exosnap
