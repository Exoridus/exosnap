#pragma once

// What the shell window does when it is not wanted on screen.
//
// Two independent product settings share this header because they answer the
// same question from opposite ends: `minimize_to_tray` decides where the window
// GOES when the user puts it away, `hide_window_from_capture` decides whether it
// is in a recording while it stays. Both are read on paths that own a window
// handle and a message pump, which is exactly where a policy stops being
// testable — so the decisions live here, pure, and the callers only carry them
// out.
//
// Closing is deliberately NOT here: a close is decided by models/CloseGuardPolicy
// and by nothing else. There is no preference that turns a close into something
// other than a close.

#include <QtGlobal>

namespace exosnap {

// Where a minimize request ends up.
enum class MinimizeOutcome {
    // The ordinary Windows minimize: the window goes to the taskbar and the
    // taskbar button brings it back.
    Taskbar,
    // The window is hidden outright; the tray icon is the only way back.
    HideToTray,
};

// `tray_available` is the fail-safe and the reason this is a function rather
// than a bare setting read: hiding a window whose only restore path does not
// exist strands the user with a running process and nothing to click. A session
// without a notification area therefore minimizes normally however the
// preference is set.
[[nodiscard]] MinimizeOutcome EvaluateMinimize(bool minimize_to_tray, bool tray_available) noexcept;

// Whether a WM_SYSCOMMAND wParam is a minimize request.
//
// Windows reserves the low four bits of the command value for its own use, so the
// wParam must be masked before it is compared: an unmasked comparison keeps
// working until Windows sets one of those bits, and then stops recognising the
// command with nothing to show for it. SC_MINIMIZE (0xF020) is spelled out rather
// than taken from windows.h so this header stays usable from a translation unit
// that does not include it.
[[nodiscard]] bool IsMinimizeSysCommand(quint64 wparam) noexcept;

// SetWindowDisplayAffinity constants, stated platform-neutrally so this header
// and the tests around it stay free of windows.h. WDA_EXCLUDEFROMCAPTURE arrived
// in Windows 10 2004 (build 19041).
inline constexpr quint32 kWindowAffinityNone = 0x00000000u;
inline constexpr quint32 kWindowAffinityExcludeFromCapture = 0x00000011u;

// The affinity the shell window should carry right now.
//
// Unconditional on the recording state, and that is the product decision: the
// setting reads "hide the ExoSnap window from screen capture", so scoping it to
// a recording would make the label true only some of the time. It also removes
// the failure mode a scoped version has — a crash or an error path between start
// and stop leaves the window excluded anyway, which is the unscoped behaviour
// arrived at by accident.
[[nodiscard]] quint32 DesiredWindowCaptureAffinity(bool hide_window_from_capture) noexcept;

} // namespace exosnap
