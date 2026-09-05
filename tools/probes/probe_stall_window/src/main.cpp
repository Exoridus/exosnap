// A capture target that stalls on purpose.
//
// Windows Graphics Capture stops producing frames for a minimised window: the
// compositor has nothing to present, so the capture goes quiet without failing.
// That is the condition the recording pipeline must report honestly, and there
// is no seam that fakes it -- a synthetic stall would exercise the seam instead
// of the detector.
//
// This probe supplies the condition and nothing else. It owns its window, shows
// it without taking focus, paints a changing colour so a capture of it is
// visibly live, and then stops producing frames ITSELF on a timer, so no part of
// the sequence synthesises input or touches a window belonging to someone else.
// It exits on its own deadline, so an abandoned run cannot leave a window behind.
//
// Two modes, because the product treats them as opposite cases and both are
// worth proving (docs/product-spec.md, capture-stall section):
//
//   freeze    a borderless window covering its monitor that simply stops
//             repainting. Fullscreen-shaped, alive, visible, not minimized --
//             the one shape a stall is reported for.
//   minimise  the same window minimized. Deliberately silent: a minimized window
//             is supposed to stop producing frames, and warning about it would be
//             a false alarm about a state the user created.
//
// It prints its window handle and title on stdout before showing anything, so a
// caller can bind to exactly this window rather than guessing from a title
// match against whatever else the desktop has open.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>
#include <cstdlib>

namespace {

constexpr wchar_t kWindowTitlePrefix[] = L"ExoSnap stall probe";

// The title carries this process id.
//
// Two scenarios use this probe back to back and both bind their capture target by
// TITLE, so a shared title let the second one select the first one's window while
// it was still in the app's target list -- a window that no longer existed. The
// recording then failed in validation ("audio_target_process_id must be a non-zero
// PID"), which described the consequence and not the cause. A caller that started
// the probe knows its pid, so putting the pid in the title makes the binding
// unambiguous with no handshake.
std::wstring BuildWindowTitle() {
    std::wstring title(kWindowTitlePrefix);
    title += L" ";
    title += std::to_wstring(GetCurrentProcessId());
    return title;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        // A colour derived from the tick count: two captures of this window a
        // second apart differ, so "the capture is live" is observable in the
        // recording rather than assumed.
        const DWORD t = GetTickCount() / 200u;
        HBRUSH brush = CreateSolidBrush(RGB(static_cast<BYTE>(40 + (t % 200)), 60, 160));
        FillRect(dc, &ps.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    int stallAfter = 8;
    int totalSeconds = 60;
    bool minimiseMode = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--mode") == 0 && i + 1 < argc) {
            minimiseMode = (wcscmp(argv[i + 1], L"minimise") == 0);
        } else if ((wcscmp(argv[i], L"--stall-after") == 0 || wcscmp(argv[i], L"--minimise-after") == 0) &&
                   i + 1 < argc) {
            stallAfter = _wtoi(argv[i + 1]);
        } else if (wcscmp(argv[i], L"--seconds") == 0 && i + 1 < argc) {
            totalSeconds = _wtoi(argv[i + 1]);
        }
    }
    if (stallAfter < 1 || totalSeconds < stallAfter + 1 || totalSeconds > 600) {
        std::fprintf(stderr,
                     "usage: probe_stall_window [--mode freeze|minimise] [--stall-after N] [--seconds M]"
                     "  (1 <= N < M <= 600)\n");
        return 1;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ExoSnapStallProbe";
    // IDC_ARROW is a MAKEINTRESOURCE, which is narrow unless the whole target is
    // built UNICODE; the probe names the wide entry point explicitly instead of
    // depending on that.
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    RegisterClassExW(&wc);

    // Freeze mode has to be FULLSCREEN-SHAPED to reach the reported path: a
    // captioned or non-monitor-filling window that stops painting is
    // indistinguishable from an idle text editor, and the product stays silent
    // about it on purpose.
    const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor, &mi);
    const RECT screen = mi.rcMonitor;
    const std::wstring window_title = BuildWindowTitle();
    HWND hwnd = minimiseMode
                    ? CreateWindowExW(0, wc.lpszClassName, window_title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                      CW_USEDEFAULT, 900, 600, nullptr, nullptr, wc.hInstance, nullptr)
                    : CreateWindowExW(0, wc.lpszClassName, window_title.c_str(), WS_POPUP, screen.left, screen.top,
                                      screen.right - screen.left, screen.bottom - screen.top, nullptr, nullptr,
                                      wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        std::fprintf(stderr, "CreateWindowExW failed %lu\n", GetLastError());
        return 2;
    }

    // SHOWNOACTIVATE, not SHOW: the window has to be capturable, not focused.
    // Taking focus would move the operator's keyboard to a test window, and the
    // capture stack does not care which window is active.
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    std::printf("hwnd=%llu\ntitle=%ls\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)),
                window_title.c_str());
    std::fflush(stdout);

    const ULONGLONG start = GetTickCount64();
    const ULONGLONG stallAt = start + static_cast<ULONGLONG>(stallAfter) * 1000ull;
    const ULONGLONG exitAt = start + static_cast<ULONGLONG>(totalSeconds) * 1000ull;
    bool stalled = false;

    for (;;) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const ULONGLONG now = GetTickCount64();
        if (!stalled && now >= stallAt) {
            stalled = true;
            if (minimiseMode) {
                ShowWindow(hwnd, SW_MINIMIZE);
            }
            // Freeze mode needs no call at all: not invalidating is what stops the
            // presents, which is precisely the condition being tested.
            std::printf("%s\n", minimiseMode ? "minimised" : "frozen");
            std::fflush(stdout);
        }
        if (now >= exitAt) {
            break;
        }
        if (!stalled) {
            InvalidateRect(hwnd, nullptr, FALSE); // keep presenting until the stall begins
        }
        Sleep(50);
    }

    DestroyWindow(hwnd);
    std::printf("exited\n");
    return 0;
}
