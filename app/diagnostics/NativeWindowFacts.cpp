#include "NativeWindowFacts.h"

#include <windows.h>

namespace exosnap::diagnostics {

int CountChildWindows(void* hwnd) {
    if (hwnd == nullptr)
        return -1;
    int count = 0;
    EnumChildWindows(
        static_cast<HWND>(hwnd),
        [](HWND, LPARAM value) {
            ++*reinterpret_cast<int*>(value);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&count));
    return count;
}

NonClientInset QueryNonClientInset(void* hwnd) {
    if (hwnd == nullptr)
        return {};
    HWND window_handle = static_cast<HWND>(hwnd);
    RECT window{};
    RECT client{};
    if (GetWindowRect(window_handle, &window) == FALSE || GetClientRect(window_handle, &client) == FALSE)
        return {};
    // GetClientRect is client-relative; map its corners into screen space so the
    // two rectangles are comparable.
    POINT top_left{client.left, client.top};
    POINT bottom_right{client.right, client.bottom};
    if (ClientToScreen(window_handle, &top_left) == FALSE || ClientToScreen(window_handle, &bottom_right) == FALSE)
        return {};
    return {static_cast<int>(top_left.x - window.left), static_cast<int>(top_left.y - window.top),
            static_cast<int>(window.right - bottom_right.x), static_cast<int>(window.bottom - bottom_right.y)};
}

NativeWindowFacts QueryNativeWindowFacts(void* hwnd) {
    NativeWindowFacts facts;
    if (hwnd == nullptr)
        return facts;
    HWND window_handle = static_cast<HWND>(hwnd);
    if (IsWindow(window_handle) == FALSE)
        return facts;

    facts.valid = true;
    facts.style = static_cast<quint64>(GetWindowLongPtrW(window_handle, GWL_STYLE));
    facts.ex_style = static_cast<quint64>(GetWindowLongPtrW(window_handle, GWL_EXSTYLE));
    facts.layered = (facts.ex_style & WS_EX_LAYERED) != 0;
    facts.transparent_for_input = (facts.ex_style & WS_EX_TRANSPARENT) != 0;
    facts.inset = QueryNonClientInset(hwnd);
    facts.child_hwnds = CountChildWindows(hwnd);

    DWORD affinity = 0;
    if (GetWindowDisplayAffinity(window_handle, &affinity) != FALSE) {
        facts.affinity_known = true;
        facts.display_affinity = static_cast<quint32>(affinity);
    }

    RECT rect{};
    if (GetWindowRect(window_handle, &rect) != FALSE) {
        facts.x = static_cast<int>(rect.left);
        facts.y = static_cast<int>(rect.top);
        facts.width = static_cast<int>(rect.right - rect.left);
        facts.height = static_cast<int>(rect.bottom - rect.top);
    }
    return facts;
}

} // namespace exosnap::diagnostics
