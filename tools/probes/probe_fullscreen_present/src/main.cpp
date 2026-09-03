// The exclusive-fullscreen condition REL-CAP-FSE-001 needs, supplied without a
// person.
//
// Takes a display into REAL exclusive fullscreen (DXGI SetFullscreenState) for
// a bounded time, so the capture stack meets the presentation mode it must
// report honestly. A borderless window would not do: the whole point of the
// gate is the mode where the desktop compositor is out of the loop, and
// simulating it would test the simulation.
//
// It leaves the display as it found it: fullscreen state is dropped explicitly
// before the swap chain is released (DXGI requires this), and the process exits
// on its own deadline so an abandoned run cannot hold the screen.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <string>
#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

int Fail(const char* what, HRESULT hr) {
    std::fprintf(stderr, "%s failed 0x%08lX\n", what, static_cast<unsigned long>(hr));
    return 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    int seconds = 20;
    int displayIndex = 0; // DXGI output index on the first adapter; 0 is the primary here
    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--seconds") == 0) {
            seconds = _wtoi(argv[i + 1]);
        } else if (wcscmp(argv[i], L"--display") == 0) {
            displayIndex = _wtoi(argv[i + 1]);
        }
    }
    if (seconds < 1 || seconds > 600) {
        std::fprintf(stderr, "--seconds must be 1..600\n");
        return 1;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ExoSnapFseProbe";
    // IDC_ARROW is a MAKEINTRESOURCE, narrow unless the target is built UNICODE.
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    RegisterClassExW(&wc);

    // Resolve the target output first: the window has to be created ON the
    // display it will go exclusive on, or DXGI enters fullscreen on whichever
    // output the window happens to overlap most.
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return Fail("CreateDXGIFactory1", E_FAIL);
    }
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput* target = nullptr;
    DXGI_OUTPUT_DESC targetDesc{};
    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
        IDXGIOutput* output = nullptr;
        for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_OUTPUT_DESC d{};
            output->GetDesc(&d);
            if (static_cast<int>(i) == displayIndex) {
                target = output;
                targetDesc = d;
                break;
            }
            output->Release();
            output = nullptr;
        }
    }
    if (!target) {
        if (adapter) adapter->Release();
        factory->Release();
        std::fprintf(stderr, "display index %d has no DXGI output\n", displayIndex);
        return 1;
    }
    wprintf(L"target display %d: %s\n", displayIndex, targetDesc.DeviceName);

    const RECT r = targetDesc.DesktopCoordinates;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ExoSnap FSE probe", WS_OVERLAPPEDWINDOW,
                                r.left + 40, r.top + 40, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        return Fail("CreateWindowExW", HRESULT_FROM_WIN32(GetLastError()));
    }
    // Exclusive fullscreen is a foreground state: DXGI drops it the moment the
    // window is not the active one, and a swap chain that asks for it from a
    // background window is handed a composed present path instead. So this probe
    // -- unlike the stall probe, which must NOT take focus -- activates its window
    // on purpose. That is what the mode under test means.
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetActiveWindow(hwnd);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = 0; // let DXGI take the output's current mode
    scd.BufferDesc.Height = 0;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE; // enter fullscreen explicitly below, per DXGI guidance
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &scd, &swap, &device, nullptr, &context);
    if (FAILED(hr)) {
        return Fail("D3D11CreateDeviceAndSwapChain", hr);
    }

    hr = swap->SetFullscreenState(TRUE, target);
    if (FAILED(hr)) {
        swap->Release();
        context->Release();
        device->Release();
        return Fail("SetFullscreenState", hr);
    }
    // Asked back, not assumed: SetFullscreenState can succeed and still leave the
    // swap chain composed when the window is not the active one, which is exactly
    // the failure this probe was written with and had to be fixed for.
    BOOL confirmed = FALSE;
    swap->GetFullscreenState(&confirmed, nullptr);
    std::printf("exclusive fullscreen entered=%d\n", confirmed ? 1 : 0);
    std::fflush(stdout);
    if (!confirmed) {
        std::fprintf(stderr, "the swap chain did not stay exclusive\n");
    }

    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        device->CreateRenderTargetView(back, nullptr, &rtv);
        back->Release();
    }

    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000ull;
    bool quit = false;
    unsigned frame = 0;
    while (!quit && GetTickCount64() < deadline) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (rtv) {
            // A moving colour, so a capture of this run is visibly a live signal
            // rather than a still frame.
            const float phase = static_cast<float>(frame % 120) / 120.0f;
            const float clear[4] = {0.10f + 0.60f * phase, 0.05f, 0.35f, 1.0f};
            context->ClearRenderTargetView(rtv, clear);
        }
        swap->Present(1, 0);
        ++frame;
    }

    // Mandatory: a swap chain released while fullscreen leaves the display in a
    // state DXGI itself warns about.
    swap->SetFullscreenState(FALSE, nullptr);
    if (rtv) rtv->Release();
    swap->Release();
    context->Release();
    device->Release();
    target->Release();
    if (adapter) adapter->Release();
    factory->Release();
    DestroyWindow(hwnd);
    std::printf("exclusive fullscreen released after %u frames\n", frame);
    return 0;
}
