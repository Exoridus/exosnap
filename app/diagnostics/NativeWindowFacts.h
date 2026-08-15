#pragma once

// NativeWindowFacts.h -- what Windows itself says about one of our windows.
//
// Every fact here is invisible to the two instruments that otherwise judge this
// application: the widget/adapter tests see objects, and --visual-test sees
// pixels. Neither can see which WINDOW owns a pixel, how much non-client area
// the system reserved, whether WS_EX_LAYERED is set, or what display affinity
// the compositor was told to apply. Those are exactly the properties the
// frameless shell and the five capture-excluded overlays are built on.
//
// Extracted so --hwnd-audit and the Live Verify window/overlay snapshots read
// ONE implementation. They answer different questions (a startup gate vs.
// observation during a live run) and must never answer them differently.

#include <QtGlobal>

namespace exosnap::diagnostics {

// How much non-client area Windows still reserves on each edge. A shell whose
// title band is its own draws NOTHING outside the client rect, so every value
// must be 0; a non-zero top is a native caption above the product's own.
struct NonClientInset {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    [[nodiscard]] bool isEmpty() const noexcept {
        return left == 0 && top == 0 && right == 0 && bottom == 0;
    }
};

struct NativeWindowFacts {
    bool valid = false;
    quint64 style = 0;
    quint64 ex_style = 0;
    NonClientInset inset;
    int child_hwnds = -1;
    // WS_EX_LAYERED (0x00080000). Broken out because it is the single bit that
    // decides whether a DirectComposition-backed overlay composes with real
    // per-pixel alpha or as an opaque rectangle -- and the harness cannot see
    // the difference.
    bool layered = false;
    // WS_EX_TRANSPARENT (0x00000020). Broken out for the same reason as
    // `layered`: it decides whether an overlay can be operated at all, and no
    // other instrument here can see it. A window carrying it swallows nothing
    // and receives nothing -- every control on it is drawn, hit-testable in the
    // scene graph, and dead to the mouse. That shipped once, on the toast.
    bool transparent_for_input = false;
    // GetWindowDisplayAffinity result (WDA_NONE 0 / WDA_MONITOR 1 /
    // WDA_EXCLUDEFROMCAPTURE 0x11). `affinity_known` is false when the call
    // failed, which is not the same as WDA_NONE.
    bool affinity_known = false;
    quint32 display_affinity = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// `hwnd` is an HWND passed as an opaque pointer so this header stays free of
// windows.h.
[[nodiscard]] int CountChildWindows(void* hwnd);
[[nodiscard]] NonClientInset QueryNonClientInset(void* hwnd);
[[nodiscard]] NativeWindowFacts QueryNativeWindowFacts(void* hwnd);

} // namespace exosnap::diagnostics
