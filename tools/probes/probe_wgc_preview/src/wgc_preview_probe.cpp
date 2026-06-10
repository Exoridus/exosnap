#include "wgc_preview_probe.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <cstdio>
#include <cstdlib>

namespace wgc_preview {

namespace {

constexpr const wchar_t* kWindowClassName = L"WgcPreviewProbeWindow";
constexpr int kDefaultWindowW = 640;
constexpr int kDefaultWindowH = 480;

std::wstring KindToString(CaptureTarget::Kind kind) {
    return kind == CaptureTarget::Kind::Monitor ? L"monitor" : L"window";
}

} // namespace

std::vector<CaptureTarget> Probe::EnumerateTargets() {
    std::vector<CaptureTarget> targets;

    winrt::com_ptr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
            winrt::com_ptr<IDXGIOutput> output;
            for (UINT j = 0; adapter->EnumOutputs(j, output.put()) != DXGI_ERROR_NOT_FOUND; ++j) {
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                    MONITORINFOEXW mi{};
                    mi.cbSize = sizeof(mi);
                    bool hasName = false;
                    if (GetMonitorInfoW(desc.Monitor, &mi)) {
                        hasName = true;
                    }
                    CaptureTarget t;
                    t.kind = CaptureTarget::Kind::Monitor;
                    if (hasName) {
                        t.description = mi.szDevice;
                    } else {
                        t.description = std::to_wstring(targets.size());
                    }
                    t.hmonitor = desc.Monitor;
                    targets.push_back(std::move(t));
                }
                output = nullptr;
            }
            adapter = nullptr;
        }
    }

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto& vec = *reinterpret_cast<std::vector<CaptureTarget>*>(lParam);

            if (!IsWindowVisible(hwnd)) {
                return TRUE;
            }
            LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            if ((style & WS_CHILD) != 0) {
                return TRUE;
            }
            if (GetWindow(hwnd, GW_OWNER) != nullptr) {
                return TRUE;
            }

            wchar_t title[256] = {};
            if (GetWindowTextW(hwnd, title, 256) == 0) {
                return TRUE;
            }

            CaptureTarget t;
            t.kind = CaptureTarget::Kind::Window;
            t.description = title;
            t.hwnd = hwnd;
            vec.push_back(std::move(t));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&targets));

    return targets;
}

Probe::Probe(const CaptureTarget& target) : m_target(target) {
}

Probe::~Probe() {
    CleanupCapture();
    CleanupWindow();
}

bool Probe::InitD3D11() {
    fprintf(stdout, "[probe] creating D3D11 device...\n");
    fflush(stdout);

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, featureLevels,
                                   static_cast<UINT>(std::size(featureLevels)), D3D11_SDK_VERSION, m_d3dDevice.put(),
                                   nullptr, m_d3dContext.put());

    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FAIL: D3D11CreateDevice returned 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }
    fprintf(stdout, "[probe] D3D11 device created OK\n");
    fflush(stdout);
    return true;
}

bool Probe::InitCaptureItem() {
    auto kindWstr = KindToString(m_target.kind);
    fwprintf(stdout, L"[probe] creating capture item... kind=%s\n", kindWstr.c_str());
    fflush(stdout);

    try {
        auto interopFactory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                                            IGraphicsCaptureItemInterop>();

        if (m_target.kind == CaptureTarget::Kind::Monitor) {
            winrt::check_hresult(interopFactory->CreateForMonitor(
                m_target.hmonitor, winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                winrt::put_abi(m_item)));
        } else {
            winrt::check_hresult(interopFactory->CreateForWindow(
                m_target.hwnd, winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                winrt::put_abi(m_item)));
        }
    } catch (const winrt::hresult_error& e) {
        fwprintf(stderr, L"[probe] FAIL: create capture item -- %s (0x%08X)\n", e.message().c_str(),
                 static_cast<unsigned int>(e.code().value));
        fflush(stderr);
        return false;
    } catch (...) {
        fprintf(stderr, "[probe] FAIL: create capture item -- unknown exception\n");
        fflush(stderr);
        return false;
    }

    if (m_item == nullptr) {
        fprintf(stderr, "[probe] FAIL: capture item is null after creation\n");
        fflush(stderr);
        return false;
    }

    auto itemSize = m_item.Size();
    fprintf(stdout, "[probe] capture item created OK, source size=%u x %u\n", itemSize.Width, itemSize.Height);
    fflush(stdout);
    return true;
}

bool Probe::InitFramePool(HWND hwnd) {
    fprintf(stdout, "[probe] creating frame pool (polling mode, no DispatcherQueue)...\n");
    fflush(stdout);

    try {
        winrt::com_ptr<IDXGIDevice> dxgiDevice = m_d3dDevice.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> d3dInspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), d3dInspectable.put()));
        auto d3dWinRTDevice = d3dInspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        auto size = m_item.Size();
        m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
            d3dWinRTDevice, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);

        fprintf(stdout, "[probe] frame pool created OK (buffer count=3)\n");
        fflush(stdout);

        m_session = m_framePool.CreateCaptureSession(m_item);
        m_session.StartCapture();
        fprintf(stdout, "[probe] capture session started OK\n");
        fflush(stdout);

        m_closedToken = m_item.Closed([this](const auto& /*sender*/, const auto& /*args*/) {
            fprintf(stdout, "[probe] capture source closed (GraphicsCaptureItem::Closed event)\n");
            fflush(stdout);
            m_sourceLost = true;
        });

        SetWindowTextW(hwnd, (L"WGC Preview - " + m_target.description).c_str());
    } catch (const winrt::hresult_error& e) {
        fwprintf(stderr, L"[probe] FAIL: InitFramePool -- %s (0x%08X)\n", e.message().c_str(),
                 static_cast<unsigned int>(e.code().value));
        fflush(stderr);
        return false;
    } catch (...) {
        fprintf(stderr, "[probe] FAIL: InitFramePool -- unknown exception\n");
        fflush(stderr);
        return false;
    }
    return true;
}

bool Probe::InitPreviewWindow() {
    fprintf(stdout, "[probe] creating preview window...\n");
    fflush(stdout);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;

    if (RegisterClassExW(&wc) == 0) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            fprintf(stderr, "[probe] FAIL: RegisterClassExW failed, error=%lu\n", static_cast<unsigned long>(err));
            fflush(stderr);
            return false;
        }
    }

    m_hwnd =
        CreateWindowExW(0, kWindowClassName, L"WGC Preview Probe", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        kDefaultWindowW, kDefaultWindowH, nullptr, nullptr, GetModuleHandleW(nullptr), this);

    if (m_hwnd == nullptr) {
        fprintf(stderr, "[probe] FAIL: CreateWindowExW failed, error=%lu\n",
                static_cast<unsigned long>(GetLastError()));
        fflush(stderr);
        return false;
    }

    fprintf(stdout, "[probe] window created OK (hwnd=0x%p)\n", m_hwnd);
    fflush(stdout);

    {
        winrt::com_ptr<IDXGIDevice> dxgiDevice = m_d3dDevice.as<IDXGIDevice>();
        winrt::com_ptr<IDXGIAdapter> dxgiAdapter;
        dxgiDevice->GetAdapter(dxgiAdapter.put());
        winrt::com_ptr<IDXGIFactory2> dxgiFactory;
        dxgiAdapter->GetParent(IID_PPV_ARGS(dxgiFactory.put()));

        RECT clientRect{};
        GetClientRect(m_hwnd, &clientRect);

        DXGI_SWAP_CHAIN_DESC1 scDesc{};
        scDesc.Width = static_cast<UINT>(clientRect.right - clientRect.left);
        scDesc.Height = static_cast<UINT>(clientRect.bottom - clientRect.top);
        scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scDesc.Stereo = FALSE;
        scDesc.SampleDesc.Count = 1;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = 2;
        scDesc.Scaling = DXGI_SCALING_STRETCH;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        HRESULT hr = dxgiFactory->CreateSwapChainForHwnd(m_d3dDevice.get(), m_hwnd, &scDesc, nullptr, nullptr,
                                                         m_swapChain.put());
        if (FAILED(hr)) {
            fprintf(stderr, "[probe] FAIL: CreateSwapChainForHwnd returned 0x%08lX\n", static_cast<unsigned long>(hr));
            fflush(stderr);
            return false;
        }
    }

    fprintf(stdout, "[probe] swap chain created OK\n");
    fflush(stdout);

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    fprintf(stdout, "[probe] preview window shown\n");
    fflush(stdout);

    return true;
}

void Probe::PollAndProcessFrames() {
    if (m_framePool == nullptr) {
        return;
    }

    try {
        while (true) {
            auto frame = m_framePool.TryGetNextFrame();
            if (frame == nullptr) {
                break;
            }

            std::lock_guard lock(m_frameMutex);

            auto surface = frame.Surface();
            auto dxgiAccess = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

            winrt::com_ptr<ID3D11Texture2D> frameTexture;
            HRESULT hr = dxgiAccess->GetInterface(IID_PPV_ARGS(frameTexture.put()));
            if (FAILED(hr)) {
                fprintf(stderr, "[probe] WARN: GetInterface for frame texture failed 0x%08lX\n",
                        static_cast<unsigned long>(hr));
                fflush(stderr);
                continue;
            }

            D3D11_TEXTURE2D_DESC desc{};
            frameTexture->GetDesc(&desc);

            bool sizeChanged = (desc.Width != m_frameWidth || desc.Height != m_frameHeight);
            m_frameWidth = desc.Width;
            m_frameHeight = desc.Height;

            if (sizeChanged || m_latestFrame == nullptr) {
                D3D11_TEXTURE2D_DESC copyDesc = desc;
                copyDesc.MiscFlags = 0;
                copyDesc.CPUAccessFlags = 0;
                copyDesc.Usage = D3D11_USAGE_DEFAULT;

                HRESULT createHr = m_d3dDevice->CreateTexture2D(&copyDesc, nullptr, m_latestFrame.put());
                if (FAILED(createHr)) {
                    fprintf(stderr, "[probe] WARN: CreateTexture2D for frame copy failed 0x%08lX\n",
                            static_cast<unsigned long>(createHr));
                    fflush(stderr);
                    continue;
                }
            }

            m_d3dContext->CopySubresourceRegion(m_latestFrame.get(), 0, 0, 0, 0, frameTexture.get(), 0, nullptr);
            m_totalFrameCount++;

            if (!m_hadFirstFrame) {
                m_hadFirstFrame = true;
                fwprintf(stdout, L"[probe] first frame received, size=%ux%u\n", desc.Width, desc.Height);
                fflush(stdout);
            }
        }
    } catch (const winrt::hresult_error& e) {
        fwprintf(stderr, L"[probe] WARN: PollAndProcessFrames -- %s (0x%08X)\n", e.message().c_str(),
                 static_cast<unsigned int>(e.code().value));
        fflush(stderr);
    } catch (...) {
        fprintf(stderr, "[probe] WARN: PollAndProcessFrames -- unknown exception\n");
        fflush(stderr);
    }
}

void Probe::RenderLatestFrame() {
    if (m_swapChain == nullptr) {
        return;
    }

    std::lock_guard lock(m_frameMutex);

    winrt::com_ptr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()));
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] WARN: GetBuffer failed 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return;
    }

    if (m_latestFrame == nullptr) {
        float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        winrt::com_ptr<ID3D11RenderTargetView> rtv;
        m_d3dDevice->CreateRenderTargetView(backBuffer.get(), nullptr, rtv.put());
        m_d3dContext->OMSetRenderTargets(1, rtv.put(), nullptr);
        m_d3dContext->ClearRenderTargetView(rtv.get(), clearColor);
        m_swapChain->Present(1, 0);
        return;
    }

    D3D11_TEXTURE2D_DESC frameDesc{};
    m_latestFrame->GetDesc(&frameDesc);

    if (frameDesc.Width == 0 || frameDesc.Height == 0) {
        m_swapChain->Present(1, 0);
        return;
    }

    D3D11_TEXTURE2D_DESC bbDesc{};
    backBuffer->GetDesc(&bbDesc);

    if (bbDesc.Width != frameDesc.Width || bbDesc.Height != frameDesc.Height) {
        m_d3dContext->OMSetRenderTargets(0, nullptr, nullptr);
        m_d3dContext->Flush();
        backBuffer = nullptr;
        HRESULT resizeHr =
            m_swapChain->ResizeBuffers(2, frameDesc.Width, frameDesc.Height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (FAILED(resizeHr)) {
            fprintf(stderr, "[probe] WARN: ResizeBuffers failed 0x%08lX\n", static_cast<unsigned long>(resizeHr));
            fflush(stderr);
        }
        return;
    }

    m_d3dContext->CopySubresourceRegion(backBuffer.get(), 0, 0, 0, 0, m_latestFrame.get(), 0, nullptr);

    HRESULT presentHr = m_swapChain->Present(1, 0);
    if (FAILED(presentHr)) {
        fprintf(stderr, "[probe] WARN: Present failed 0x%08lX\n", static_cast<unsigned long>(presentHr));
        fflush(stderr);
    }
}

void Probe::PrintMetrics() {
    auto now = std::chrono::steady_clock::now();
    double intervalSec = std::chrono::duration<double>(now - m_lastMetricTime).count();
    if (intervalSec <= 0.0) {
        return;
    }

    uint64_t framesInInterval = m_totalFrameCount - m_lastMetricFrameCount;
    double sourceFps = static_cast<double>(framesInInterval) / intervalSec;

    std::lock_guard lock(m_frameMutex);
    auto w = KindToString(m_target.kind);
    fwprintf(stdout, L"[metrics] target=%s size=%ux%u fps=%.1f total=%llu dropped=n/a\n", w.c_str(), m_frameWidth,
             m_frameHeight, sourceFps, static_cast<unsigned long long>(m_totalFrameCount));

    m_lastMetricTime = now;
    m_lastMetricFrameCount = m_totalFrameCount;
}

void Probe::PrintSummary() {
    auto endTime = std::chrono::steady_clock::now();
    double durationSec = std::chrono::duration<double>(endTime - m_startTime).count();

    double avgFps = 0.0;
    if (durationSec > 0.0) {
        avgFps = static_cast<double>(m_totalFrameCount) / durationSec;
    }

    auto w = KindToString(m_target.kind);
    fwprintf(stdout, L"\n=== SUMMARY ===\n");
    fwprintf(stdout, L"  target kind        : %s\n", w.c_str());
    fwprintf(stdout, L"  duration (s)       : %.1f\n", durationSec);
    fwprintf(stdout, L"  total frames       : %llu\n", static_cast<unsigned long long>(m_totalFrameCount));
    fwprintf(stdout, L"  avg fps            : %.1f\n", avgFps);
    fwprintf(stdout, L"  source loss        : %s\n", m_sourceLost ? L"yes" : L"no");
    fwprintf(stdout, L"=================\n");
    fflush(stdout);
}

bool Probe::Start() {
    fprintf(stdout, "[probe] === starting probe ===\n");

    auto kindWstr = KindToString(m_target.kind);
    fwprintf(stdout, L"[probe] target: %s -- %s\n", kindWstr.c_str(), m_target.description.c_str());
    fflush(stdout);

    if (!InitD3D11()) {
        fprintf(stderr, "[probe] ABORT: D3D11 init failed\n");
        fflush(stderr);
        return false;
    }
    if (!InitPreviewWindow()) {
        fprintf(stderr, "[probe] ABORT: preview window init failed\n");
        fflush(stderr);
        return false;
    }
    if (!InitCaptureItem()) {
        fprintf(stderr, "[probe] ABORT: capture item init failed\n");
        fflush(stderr);
        return false;
    }
    if (!InitFramePool(m_hwnd)) {
        fprintf(stderr, "[probe] ABORT: frame pool init failed\n");
        fflush(stderr);
        return false;
    }

    m_running = true;
    m_startTime = std::chrono::steady_clock::now();
    m_lastMetricTime = m_startTime;

    fprintf(stdout, "[probe] all init steps passed -- entering render loop\n");
    fflush(stdout);
    return true;
}

void Probe::Run() {
    MSG msg{};
    auto nextMetricTime = m_startTime + std::chrono::seconds(1);

    while (m_running && !m_sourceLost) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                m_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!m_running) {
            fprintf(stdout, "[probe] window closed, stopping...\n");
            fflush(stdout);
            break;
        }

        if (m_target.kind == CaptureTarget::Kind::Window && !IsWindow(m_target.hwnd)) {
            fprintf(stdout, "[probe] source window destroyed (IsWindow check)\n");
            fflush(stdout);
            m_sourceLost = true;
            break;
        }

        PollAndProcessFrames();

        auto now = std::chrono::steady_clock::now();
        if (now >= nextMetricTime) {
            PrintMetrics();
            nextMetricTime = now + std::chrono::seconds(1);
        }

        RenderLatestFrame();

        Sleep(1);
    }

    if (m_sourceLost) {
        fprintf(stdout, "[probe] capture source lost, stopping...\n");
        fflush(stdout);
    }

    CleanupCapture();

    if (m_hwnd != nullptr) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    {
        MSG leftover{};
        while (PeekMessageW(&leftover, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&leftover);
            DispatchMessageW(&leftover);
        }
    }

    PrintSummary();
}

LRESULT CALLBACK Probe::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* pcs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pcs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* probe = reinterpret_cast<Probe*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (probe != nullptr) {
        return probe->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Probe::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_DESTROY:
        m_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (m_swapChain != nullptr && wParam != SIZE_MINIMIZED) {
            RECT clientRect{};
            GetClientRect(hwnd, &clientRect);
            UINT w = static_cast<UINT>(clientRect.right - clientRect.left);
            UINT h = static_cast<UINT>(clientRect.bottom - clientRect.top);
            if (w > 0 && h > 0) {
                std::lock_guard lock(m_frameMutex);
                m_d3dContext->OMSetRenderTargets(0, nullptr, nullptr);
                m_swapChain->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
            }
        }
        return 0;
    case WM_CLOSE:
        m_running = false;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void Probe::CleanupCapture() {
    if (m_session == nullptr && m_framePool == nullptr && m_item == nullptr) {
        return;
    }

    fprintf(stdout, "[probe] cleaning up capture...\n");
    fflush(stdout);

    if (m_session != nullptr) {
        m_session.Close();
        m_session = nullptr;
        fprintf(stdout, "[probe] capture session closed\n");
        fflush(stdout);
    }
    if (m_framePool != nullptr) {
        m_framePool.Close();
        m_framePool = nullptr;
        fprintf(stdout, "[probe] frame pool closed\n");
        fflush(stdout);
    }
    if (m_item != nullptr) {
        m_item.Closed(m_closedToken);
        m_closedToken = {};
        m_item = nullptr;
    }
    m_swapChain = nullptr;
    m_d3dContext = nullptr;
    m_d3dDevice = nullptr;
    m_latestFrame = nullptr;
}

void Probe::CleanupWindow() {
    if (m_hwnd != nullptr) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

} // namespace wgc_preview
