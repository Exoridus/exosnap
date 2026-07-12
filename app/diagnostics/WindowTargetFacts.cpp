#include "WindowTargetFacts.h"

#include <dwmapi.h>
#include <shlobj.h> // SHQueryUserNotificationState

namespace exosnap::diagnostics {

WindowShape ClassifyWindowShape(const WindowTargetFacts& facts) noexcept {
    // A window that is gone, hidden, minimized or cloaked is never treated as
    // fullscreen-shaped: it is not presenting a fullscreen surface right now.
    if (!facts.valid || !facts.visible || facts.minimized || facts.cloaked) {
        return WindowShape::Normal;
    }

    // The window must cover its whole monitor. "Cover" (not "equal") tolerates
    // the 1-px overhang some borderless windows use and any rounding, while a
    // windowed app that leaves any monitor edge exposed stays Normal.
    const RECT& w = facts.window_rect;
    const RECT& m = facts.monitor_rect;
    const bool covers_monitor = w.left <= m.left && w.top <= m.top && w.right >= m.right && w.bottom >= m.bottom;
    if (!covers_monitor) {
        return WindowShape::Normal;
    }

    // A borderless/FSE window carries neither a title-bar caption nor a sizing
    // frame. A maximized ordinary window still has WS_CAPTION and is excluded.
    const bool has_caption = (facts.style & WS_CAPTION) == WS_CAPTION;
    const bool has_frame = (facts.style & WS_THICKFRAME) != 0;
    if (has_caption || has_frame) {
        return WindowShape::Normal;
    }

    return WindowShape::FullscreenShaped;
}

ExclusiveEvidence CombineFullscreenEvidence(WindowShape shape, const WindowHubEvidence& hub,
                                            bool fullscreen_signal) noexcept {
    if (shape != WindowShape::FullscreenShaped) {
        return ExclusiveEvidence::None;
    }

    // Proven black: the same WGC API the recording would use has demonstrably
    // produced nothing usable for the selected window.
    const bool never_produced =
        hub.kind == recorder_core::HubFrameKind::None && hub.seconds_subscribed >= kEvidenceMinSeconds;
    const bool froze_at_transition = hub.kind == recorder_core::HubFrameKind::Held &&
                                     !hub.fresh_frame_since_fullscreen_shape &&
                                     hub.seconds_since_fresh_frame >= kEvidenceMinSeconds;
    if (never_produced || froze_at_transition) {
        return ExclusiveEvidence::ProvenBlack;
    }

    if (fullscreen_signal) {
        return ExclusiveEvidence::Suspected;
    }
    return ExclusiveEvidence::None;
}

WindowTargetFacts GatherWindowTargetFacts(HWND hwnd) {
    WindowTargetFacts facts;
    if (hwnd == nullptr || IsWindow(hwnd) == FALSE) {
        return facts; // valid == false
    }
    facts.valid = true;
    facts.visible = IsWindowVisible(hwnd) != FALSE;
    facts.minimized = IsIconic(hwnd) != FALSE;
    facts.is_foreground = (GetForegroundWindow() == hwnd);
    facts.style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    facts.ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    GetWindowRect(hwnd, &facts.window_rect);

    if (HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            facts.monitor_rect = mi.rcMonitor;
        }
    }

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        facts.cloaked = (cloaked != 0);
    }

    // Documented Shell signal, no elevation. Only reliable for the primary
    // monitor's foreground D3D-exclusive app; used solely as a corroborating
    // fullscreen signal, never on its own.
    QUERY_USER_NOTIFICATION_STATE quns{};
    if (SUCCEEDED(SHQueryUserNotificationState(&quns))) {
        facts.quns_d3d_fullscreen = (quns == QUNS_RUNNING_D3D_FULL_SCREEN);
    }

    return facts;
}

} // namespace exosnap::diagnostics
