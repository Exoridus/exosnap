#pragma once

#include <string>
#include <vector>

#include <windows.h>

#include <capability/runtime_snapshot.h>
#include <recorder_core/recorder_session.h>

namespace exosnap {

// Resolve the HDR facts of the display hosting a capture target, for both target
// kinds:
//   - Monitor: its HMONITOR (target.native_id) directly.
//   - Window:  the hosting monitor via MonitorFromWindow(HWND), resolved at call
//     time (MONITOR_DEFAULTTONEAREST). A later move to another monitor is not
//     tracked — the recording session likewise keeps its initial HDR decision.
//
// The impure HMONITOR/HWND -> Windows display-device-name step lives here; the pure
// lookup over already-probed facts is capability::FindDisplayByName. Returns nullptr
// when the display cannot be matched (headless, no probed facts, or an unknown
// device name). Shared by the diagnostics HDR-blocker gating and the recording
// coordinator's native-HDR10 metadata assembly so both treat windows identically.
//
// Header-only (mirrors capability::FindDisplayByName) so every test target that
// pulls in DiagnosticsPage.cpp / RecordingCoordinator.cpp gets it without extra
// build wiring. Both callers already include <windows.h>.
[[nodiscard]] inline const capability::DisplayHdrFacts*
FindTargetDisplayFacts(const recorder_core::CaptureTarget& target,
                       const std::vector<capability::DisplayHdrFacts>& displays) {
    HMONITOR monitor = nullptr;
    switch (target.kind) {
    case recorder_core::CaptureTarget::Kind::Monitor:
        monitor = reinterpret_cast<HMONITOR>(target.native_id);
        break;
    case recorder_core::CaptureTarget::Kind::Window:
        monitor = MonitorFromWindow(reinterpret_cast<HWND>(target.native_id), MONITOR_DEFAULTTONEAREST);
        break;
    }
    if (!monitor) {
        return nullptr;
    }
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, &mi) == FALSE) {
        return nullptr;
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return nullptr;
    }
    std::string device_name(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, device_name.data(), len, nullptr, nullptr);
    return capability::FindDisplayByName(displays, device_name);
}

} // namespace exosnap
