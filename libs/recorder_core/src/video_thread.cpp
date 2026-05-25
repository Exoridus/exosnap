#include "video_thread.h"

#include "annexb_to_avcc.h"
#include "codec_private.h"
#include "nvenc_encoder.h"
#include "session_internal.h"

#include <recorder_core/packet_types.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <dwmapi.h>
#include <dxgi.h>

#include <cstdio>
#include <cstring>
#include <sstream>

// ============================================================================
// D3D11 threading contract
// ============================================================================
// ID3D11DeviceContext (m_d3dContext) and ID3D11VideoContext (m_videoContext) are
// used EXCLUSIVELY on this VideoThread.  No other thread in RecorderSession may
// call any method on these interfaces.  The shared ID3D11Device (m_d3dDevice)
// lifetime is owned by RecorderSession::Impl; VideoThread borrows the pointer.
// ============================================================================

namespace recorder_core {

namespace {

// Returns QPC-derived time in 100 ns units. freq must be the cached QPC frequency.
static uint64_t Qpc100ns(uint64_t freq) noexcept {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    const auto q = static_cast<uint64_t>(qpc.QuadPart);
    return (q / freq) * 10000000ULL + (q % freq) * 10000000ULL / freq;
}

const char* TargetKindName(CaptureTarget::Kind kind) noexcept {
    switch (kind) {
    case CaptureTarget::Kind::Monitor:
        return "Monitor";
    case CaptureTarget::Kind::Window:
        return "Window";
    default:
        return "Unknown";
    }
}

const char* BoolText(bool value) noexcept {
    return value ? "true" : "false";
}

int RectWidth(const RECT& r) noexcept {
    return r.right - r.left;
}

int RectHeight(const RECT& r) noexcept {
    return r.bottom - r.top;
}

} // namespace

VideoThread::VideoThread(SessionState& state) : m_state(state) {
}

VideoThread::~VideoThread() {
    if (m_thread.joinable())
        m_thread.detach();
}

void VideoThread::Start() {
    m_thread = std::thread([this] { Run(); });
}

bool VideoThread::Join(unsigned timeout_ms) {
    if (!m_thread.joinable())
        return true;
    // Windows thread join with timeout: use a timed wait on the native handle
    HANDLE h = m_thread.native_handle();
    DWORD r = WaitForSingleObject(h, static_cast<DWORD>(timeout_ms));
    if (r == WAIT_OBJECT_0) {
        m_thread.join();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

void VideoThread::Run() {
    // Cache QPC frequency once — it is constant for the process lifetime.
    LARGE_INTEGER qpcFreqRaw;
    QueryPerformanceFrequency(&qpcFreqRaw);
    const uint64_t qpcFreq = static_cast<uint64_t>(qpcFreqRaw.QuadPart);

    // --- COM init (apartment-threaded for WinRT) ---
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool com_inited = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!com_inited) {
        char buf[80];
        snprintf(buf, sizeof(buf), "CoInitializeEx failed 0x%08lX", static_cast<unsigned long>(hr));
        m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
        return;
    }

    // --- D3D11 video-capable device (exclusive to this thread's context) ---
    winrt::com_ptr<ID3D11Device> d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    winrt::com_ptr<ID3D11VideoDevice> videoDevice;
    winrt::com_ptr<ID3D11VideoContext> videoContext;

    {
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                               static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, d3dDevice.put(), nullptr,
                               d3dContext.put());

        if (FAILED(hr) || !d3dDevice) {
            char buf[80];
            snprintf(buf, sizeof(buf), "D3D11CreateDevice failed 0x%08lX", static_cast<unsigned long>(hr));
            m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }

        hr = d3dDevice->QueryInterface(IID_PPV_ARGS(videoDevice.put()));
        if (FAILED(hr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "QI ID3D11VideoDevice failed 0x%08lX", static_cast<unsigned long>(hr));
            m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }

        hr = d3dContext->QueryInterface(IID_PPV_ARGS(videoContext.put()));
        if (FAILED(hr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "QI ID3D11VideoContext failed 0x%08lX", static_cast<unsigned long>(hr));
            m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
    }

    const CaptureTarget& target = m_state.config.target;
    const HWND targetHwnd =
        (target.kind == CaptureTarget::Kind::Window) ? reinterpret_cast<HWND>(target.native_id) : nullptr;

    bool windowHandleValid = false;
    bool windowVisible = false;
    bool windowMinimized = false;
    bool windowCloaked = false;
    RECT windowRect{};
    RECT clientRect{};
    int windowWidth = 0;
    int windowHeight = 0;
    int clientWidth = 0;
    int clientHeight = 0;

    if (target.kind == CaptureTarget::Kind::Window) {
        windowHandleValid = (targetHwnd != nullptr && IsWindow(targetHwnd) != FALSE);
        if (!windowHandleValid) {
            std::ostringstream oss;
            oss << "Window target handle invalid before WGC init: native_id=0x" << std::hex << target.native_id
                << std::dec;
            m_state.RecordFailure(E_INVALIDARG, ErrorPhase::VideoCapture, oss.str());
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }

        windowVisible = IsWindowVisible(targetHwnd) != FALSE;
        windowMinimized = IsIconic(targetHwnd) != FALSE;
        if (GetWindowRect(targetHwnd, &windowRect)) {
            windowWidth = RectWidth(windowRect);
            windowHeight = RectHeight(windowRect);
        }
        if (GetClientRect(targetHwnd, &clientRect)) {
            clientWidth = RectWidth(clientRect);
            clientHeight = RectHeight(clientRect);
        }
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(targetHwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
            windowCloaked = (cloaked != 0);
        }
    }

    // --- WGC capture item ---
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    {
        try {
            auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                                         IGraphicsCaptureItemInterop>();

            if (target.kind == CaptureTarget::Kind::Monitor) {
                winrt::check_hresult(interop->CreateForMonitor(
                    reinterpret_cast<HMONITOR>(target.native_id),
                    winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
            } else {
                winrt::check_hresult(interop->CreateForWindow(
                    reinterpret_cast<HWND>(target.native_id),
                    winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
            }
        } catch (const winrt::hresult_error& e) {
            char buf[96];
            snprintf(buf, sizeof(buf), "CreateForMonitor/Window failed 0x%08X",
                     static_cast<unsigned int>(e.code().value));
            m_state.RecordFailure(static_cast<HRESULT>(e.code().value), ErrorPhase::VideoCapture, buf);
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
    }

    // Capture dimensions
    auto sz = item.Size();
    const int32_t sourceWidthSigned = sz.Width;
    const int32_t sourceHeightSigned = sz.Height;

    std::ostringstream diag;
    diag << "target.kind=" << TargetKindName(target.kind) << ", target.description=\"" << target.description
         << "\", target.native_id=0x" << std::hex << target.native_id << std::dec
         << ", capture.visibleContentSize=" << sourceWidthSigned << "x" << sourceHeightSigned
         << ", capture.dxgiFormat=DXGI_FORMAT_B8G8R8A8_UNORM"
         << ", videoCodec=" << (m_state.config.video_codec == VideoCodec::H264Nvenc ? "H264_NVENC" : "AV1_NVENC")
         << ", chroma=4:2:0, bitDepth=8, frameRate=" << m_state.config.frame_rate_num << "/"
         << m_state.config.frame_rate_den << ", nvencInputBufferFormat=NV_ENC_BUFFER_FORMAT_NV12";

    if (target.kind == CaptureTarget::Kind::Window) {
        diag << ", window.handleValid=" << BoolText(windowHandleValid) << ", window.visible=" << BoolText(windowVisible)
             << ", window.minimized=" << BoolText(windowMinimized) << ", window.cloaked=" << BoolText(windowCloaked)
             << ", window.windowRect=" << windowWidth << "x" << windowHeight << ", window.clientRect=" << clientWidth
             << "x" << clientHeight;
    }

    if (sourceWidthSigned <= 0 || sourceHeightSigned <= 0) {
        std::ostringstream err;
        err << "WGC source size invalid (<=0): " << sourceWidthSigned << "x" << sourceHeightSigned << "; preInit={"
            << diag.str() << "}";
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::VideoCapture, err.str());
        if (com_inited && hr != RPC_E_CHANGED_MODE)
            CoUninitialize();
        return;
    }

    uint32_t sourceWidth = static_cast<uint32_t>(sourceWidthSigned);
    uint32_t sourceHeight = static_cast<uint32_t>(sourceHeightSigned);

    // Determine crop region in monitor-local pixel coordinates.
    // CaptureRegion uses virtual-screen coordinates; subtract the monitor origin.
    bool hasCrop = false;
    int32_t cropX = 0, cropY = 0;
    int32_t cropW = static_cast<int32_t>(sourceWidth);
    int32_t cropH = static_cast<int32_t>(sourceHeight);

    if (m_state.config.crop_region.has_value() && target.kind == CaptureTarget::Kind::Monitor) {
        const auto& region = *m_state.config.crop_region;
        HMONITOR hmon = reinterpret_cast<HMONITOR>(target.native_id);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(hmon, &mi)) {
            cropX = region.x - static_cast<int32_t>(mi.rcMonitor.left);
            cropY = region.y - static_cast<int32_t>(mi.rcMonitor.top);
            cropW = region.width;
            cropH = region.height;
            // Clamp to monitor-local bounds (avoid std::min/max — Windows macros interfere).
            if (cropX < 0)
                cropX = 0;
            if (cropY < 0)
                cropY = 0;
            const int32_t maxW = static_cast<int32_t>(sourceWidth) - cropX;
            const int32_t maxH = static_cast<int32_t>(sourceHeight) - cropY;
            if (cropW > maxW)
                cropW = maxW;
            if (cropH > maxH)
                cropH = maxH;
            if (cropW >= CaptureRegion::kMinDimension && cropH >= CaptureRegion::kMinDimension) {
                hasCrop = true;
            }
        }
    }

    uint32_t encodeWidth = hasCrop ? (static_cast<uint32_t>(cropW) & ~1u) : (sourceWidth & ~1u);
    uint32_t encodeHeight = hasCrop ? (static_cast<uint32_t>(cropH) & ~1u) : (sourceHeight & ~1u);

    const bool dimsZero = (encodeWidth == 0 || encodeHeight == 0);
    const bool dimsEven = ((encodeWidth % 2u) == 0u) && ((encodeHeight % 2u) == 0u);
    diag << ", encodeSize=" << encodeWidth << "x" << encodeHeight << ", encode.dimsZero=" << BoolText(dimsZero)
         << ", encode.evenAligned=" << BoolText(dimsEven)
         << ", firstFrameTexture=unavailable_pre_init, firstFrameTextureFormat=unavailable_pre_init";

    if (encodeWidth < 2 || encodeHeight < 2) {
        std::ostringstream err;
        err << "source " << sourceWidth << "x" << sourceHeight << " rounds to " << encodeWidth << "x" << encodeHeight
            << " — too small for NV12; preInit={" << diag.str() << "}";
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::VideoCapture, err.str());
        if (com_inited && hr != RPC_E_CHANGED_MODE)
            CoUninitialize();
        return;
    }

    {
        std::lock_guard lk(m_state.stats_mutex);
        m_state.encode_width = encodeWidth;
        m_state.encode_height = encodeHeight;
    }

    // --- NVENC encoder ---
    NvencEncoder nvenc;
    {
        nvenc.SetCodec(m_state.config.video_codec);
        nvenc.SetQualityPreset(m_state.config.nvenc_quality_preset);

        std::string err;
        if (!nvenc.Open(d3dDevice.get(), err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::Prepare, "NVENC open: " + err);
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }

        const bool nvencQueryOk = (m_state.config.video_codec == VideoCodec::H264Nvenc)
                                      ? nvenc.QueryH264Nv12Support(err)
                                      : nvenc.QueryAv1Nv12Support(err);
        if (!nvencQueryOk) {
            const char* label = (m_state.config.video_codec == VideoCodec::H264Nvenc) ? "H264/NV12" : "AV1/NV12";
            m_state.RecordFailure(E_NOTIMPL, ErrorPhase::Prepare, std::string("NVENC ") + label + " support: " + err);
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }

        if (!nvenc.FetchPresetConfig(err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::Prepare, "NVENC preset config: " + err);
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
        if (!nvenc.InitEncoder(encodeWidth, encodeHeight, m_state.config.frame_rate_num, m_state.config.frame_rate_den,
                               err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoEncode,
                                  "NVENC init: " + err + "; preInit={" + diag.str() + "}");
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
        if (!nvenc.CreateBitstreamBuffer(err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoEncode, "NVENC bitstream buffer: " + err);
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
    }

    // --- NV12 texture ring + video processor ---
    static constexpr int32_t kSlotCount = 8;
    winrt::com_ptr<ID3D11Texture2D> nv12Textures[kSlotCount];
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> videoEnum;
    winrt::com_ptr<ID3D11VideoProcessor> videoProcessor;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> videoOutputViews[kSlotCount];

    {
        // Video processor enumerator (shared across all slots)
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc{};
        contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        // For crop: InputWidth/Height = full monitor source so the source rect is valid.
        contentDesc.InputWidth = hasCrop ? sourceWidth : encodeWidth;
        contentDesc.InputHeight = hasCrop ? sourceHeight : encodeHeight;
        contentDesc.OutputWidth = encodeWidth;
        contentDesc.OutputHeight = encodeHeight;
        contentDesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

        hr = videoDevice->CreateVideoProcessorEnumerator(&contentDesc, videoEnum.put());
        if (FAILED(hr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "CreateVideoProcessorEnumerator failed 0x%08lX", static_cast<unsigned long>(hr));
            m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
            if (com_inited)
                CoUninitialize();
            return;
        }

        hr = videoDevice->CreateVideoProcessor(videoEnum.get(), 0, videoProcessor.put());
        if (FAILED(hr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "CreateVideoProcessor failed 0x%08lX", static_cast<unsigned long>(hr));
            m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
            if (com_inited)
                CoUninitialize();
            return;
        }

        // Create NV12 textures and output views for each slot
        for (int32_t i = 0; i < kSlotCount; ++i) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = encodeWidth;
            desc.Height = encodeHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_NV12;
            desc.SampleDesc = {1, 0};
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;

            hr = d3dDevice->CreateTexture2D(&desc, nullptr, nv12Textures[i].put());
            if (FAILED(hr)) {
                char buf[80];
                snprintf(buf, sizeof(buf), "CreateTexture2D(NV12[%d]) failed 0x%08lX", static_cast<int>(i),
                         static_cast<unsigned long>(hr));
                m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
                if (com_inited)
                    CoUninitialize();
                return;
            }

            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc{};
            ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            ovDesc.Texture2D.MipSlice = 0;

            hr = videoDevice->CreateVideoProcessorOutputView(nv12Textures[i].get(), videoEnum.get(), &ovDesc,
                                                             videoOutputViews[i].put());
            if (FAILED(hr)) {
                char buf[80];
                snprintf(buf, sizeof(buf), "CreateVideoProcessorOutputView[%d] failed 0x%08lX", static_cast<int>(i),
                         static_cast<unsigned long>(hr));
                m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
                if (com_inited)
                    CoUninitialize();
                return;
            }

            std::string err;
            if (!nvenc.RegisterSlotTexture(i, nv12Textures[i].get(), err)) {
                m_state.RecordFailure(E_FAIL, ErrorPhase::Prepare, "NVENC register slot: " + err);
                if (com_inited)
                    CoUninitialize();
                return;
            }
        }
    }

    // Apply crop source/destination rects on the video processor.
    // These are stream state that persists until changed, so set once before the loop.
    if (hasCrop) {
        const RECT srcRect = {static_cast<LONG>(cropX), static_cast<LONG>(cropY),
                              static_cast<LONG>(cropX + static_cast<int32_t>(encodeWidth)),
                              static_cast<LONG>(cropY + static_cast<int32_t>(encodeHeight))};
        videoContext->VideoProcessorSetStreamSourceRect(videoProcessor.get(), 0, TRUE, &srcRect);
        const RECT dstRect = {0, 0, static_cast<LONG>(encodeWidth), static_cast<LONG>(encodeHeight)};
        videoContext->VideoProcessorSetStreamDestRect(videoProcessor.get(), 0, TRUE, &dstRect);
    }

    // --- WGC frame pool and session ---
    bool sourceLost = false;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession captureSession{nullptr};
    winrt::event_token closedToken{};

    try {
        winrt::com_ptr<IDXGIDevice> dxgiDev = d3dDevice.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> insp;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.get(), insp.put()));
        auto d3dWinRTDev = insp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        auto capSz = item.Size();
        framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
            d3dWinRTDev, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, capSz);

        captureSession = framePool.CreateCaptureSession(item);
        captureSession.IsCursorCaptureEnabled(m_state.config.capture_cursor);
        captureSession.StartCapture();

        closedToken = item.Closed([&sourceLost](const auto&, const auto&) { sourceLost = true; });
    } catch (const winrt::hresult_error& e) {
        char buf[96];
        snprintf(buf, sizeof(buf), "WGC frame pool init failed 0x%08X", static_cast<unsigned int>(e.code().value));
        m_state.RecordFailure(static_cast<HRESULT>(e.code().value), ErrorPhase::VideoCapture, buf);
        if (com_inited)
            CoUninitialize();
        return;
    }

    // --- Wait for first frame (5 s timeout) ---
    {
        LARGE_INTEGER freq{}, tStart{}, tNow{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&tStart);
        constexpr double kTimeoutSec = 5.0;
        bool gotFirst = false;

        while (!gotFirst && !m_state.stop_requested.load()) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            QueryPerformanceCounter(&tNow);
            double elapsed = static_cast<double>(tNow.QuadPart - tStart.QuadPart) / static_cast<double>(freq.QuadPart);
            if (elapsed > kTimeoutSec) {
                m_state.RecordFailure(HRESULT_FROM_WIN32(ERROR_TIMEOUT), ErrorPhase::VideoCapture,
                                      "WGC: timeout waiting for first frame (5 s)");
                if (captureSession != nullptr)
                    captureSession.Close();
                if (framePool != nullptr)
                    framePool.Close();
                if (com_inited)
                    CoUninitialize();
                return;
            }

            if (sourceLost) {
                m_state.RecordFailure(E_ABORT, ErrorPhase::VideoCapture, "WGC: source lost before first frame");
                if (captureSession != nullptr)
                    captureSession.Close();
                if (framePool != nullptr)
                    framePool.Close();
                if (com_inited)
                    CoUninitialize();
                return;
            }

            try {
                auto frame = framePool.TryGetNextFrame();
                if (frame != nullptr) {
                    gotFirst = true;
                } else {
                    Sleep(1);
                }
            } catch (...) {
                Sleep(1);
            }
        }
    }

    // --- Capture + encode loop ---
    bool av1CodecPrivateReady = false;
    bool h264CodecPrivateReady = false;
    uint64_t lastVideoPts = 0;
    uint64_t videoFramesCaptured = 0;
    uint64_t droppedFrames = 0;
    uint64_t slotStallCount = 0;

    // CFR timing constants (computed once; valid for both paths)
    const uint64_t frame_interval_100ns =
        (m_state.config.frame_rate_den > 0 && m_state.config.frame_rate_num > 0)
            ? (10000000ULL * m_state.config.frame_rate_den / m_state.config.frame_rate_num)
            : 166667ULL; // 60 fps fallback
    const uint64_t frame_interval_ns = frame_interval_100ns * 100ULL;
    // Maximum catch-up frames emitted per outer-loop iteration (1 second).
    // Prevents burst GPU workload after process suspension.
    const uint64_t kMaxCatchUpFrames = (m_state.config.frame_rate_den > 0 && m_state.config.frame_rate_num > 0)
                                           ? m_state.config.frame_rate_num / m_state.config.frame_rate_den
                                           : 60u;

    // Helper lambda: push an encoded packet to premux or mux queue.
    // Uses h264CodecPrivateReady and av1CodecPrivateReady by reference.
    auto routePacket = [&](EncodedVideoPacket pkt) -> bool {
        const size_t pkt_bytes_count = pkt.bytes.size();
        if (pkt.bytes.empty())
            return true;

        if (pkt.keyframe) {
            if (m_state.config.video_codec == VideoCodec::H264Nvenc && !h264CodecPrivateReady) {
                std::vector<uint8_t> spsPps;
                if (annexb::ExtractH264SpsAndPps(pkt.bytes.data(), pkt.bytes.size(), spsPps)) {
                    std::lock_guard lk(m_state.premux_mutex);
                    m_state.codec_private.h264_sps_pps = std::move(spsPps);
                    m_state.codec_private.h264_ready = true;
                    h264CodecPrivateReady = true;
                    m_state.premux_cv.notify_all();
                }
            } else if (m_state.config.video_codec != VideoCodec::H264Nvenc && !av1CodecPrivateReady) {
                char reason[256] = {};
                uint8_t cp[4] = {};
                if (codec_private::DeriveAv1CodecPrivate(pkt.bytes.data(), pkt.bytes.size(), cp, reason,
                                                         sizeof(reason))) {
                    std::lock_guard lk(m_state.premux_mutex);
                    std::memcpy(m_state.codec_private.av1_codec_private, cp, 4);
                    m_state.codec_private.av1_ready = true;
                    av1CodecPrivateReady = true;
                    m_state.premux_cv.notify_all();
                }
            }
        }

        {
            std::unique_lock lk(m_state.premux_mutex);
            bool bothReady = m_state.codec_private.VideoReady(m_state.config.video_codec) &&
                             m_state.codec_private.AudioAllReady(m_state.audio_track_count);
            if (!bothReady) {
                if (m_state.video_premux.size() >= SessionState::kVideoPremuxLimit) {
                    lk.unlock();
                    m_state.RecordFailure(E_OUTOFMEMORY, ErrorPhase::Mux,
                                          "Pre-mux video buffer limit (120 packets) exceeded "
                                          "before codec private data was ready");
                    return false;
                }
                m_state.video_premux.push_back(std::move(pkt));
            } else {
                lk.unlock();
                MuxItem mux_item;
                mux_item.payload = std::move(pkt);
                std::lock_guard mlk(m_state.mux_mutex);
                m_state.mux_queue.push_back(std::move(mux_item));
                m_state.mux_cv.notify_one();
            }
        }

        {
            std::lock_guard slk(m_state.stats_mutex);
            m_state.stats.video_frames_captured = videoFramesCaptured;
            m_state.stats.encoded_video_packets++;
            m_state.stats.video_bytes += pkt_bytes_count;
        }
        return true;
    };

    if (m_state.config.cfr) {
        // ====================================================================
        // CFR path: QPC-driven scheduler — duplicate/drop to hit constant rate
        // ====================================================================

        // Reference NV12 texture for frame duplication
        winrt::com_ptr<ID3D11Texture2D> refNv12;
        bool refNv12Valid = false;

        {
            D3D11_TEXTURE2D_DESC refDesc{};
            refDesc.Width = encodeWidth;
            refDesc.Height = encodeHeight;
            refDesc.MipLevels = 1;
            refDesc.ArraySize = 1;
            refDesc.Format = DXGI_FORMAT_NV12;
            refDesc.SampleDesc = {1, 0};
            refDesc.Usage = D3D11_USAGE_DEFAULT;
            refDesc.BindFlags = 0; // only used as CopyResource source/dest

            HRESULT refHr = d3dDevice->CreateTexture2D(&refDesc, nullptr, refNv12.put());
            if (FAILED(refHr)) {
                // Non-fatal: refNv12 stays null; duplication silently drops ticks
                // without a reference frame (until the first real frame arrives).
                refNv12 = nullptr;
            }
        }

        bool videoEpochSet = false;
        uint64_t epochQpc100ns = 0;
        uint64_t cfr_frame_idx = 0;
        uint64_t next_tick_100ns = 0; // relative to epoch

        bool cfr_was_paused = false;
        uint64_t cfr_pause_start_100ns = 0;

        winrt::com_ptr<ID3D11Texture2D> pendingWgcTex;

        while (!m_state.stop_requested.load()) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            if (sourceLost) {
                std::lock_guard lk(m_state.stats_mutex);
                m_state.stats.source_loss = true;
                m_state.stop_requested.store(true);
                break;
            }

            // Drain WGC frames — keep latest (always drain, even when paused)
            try {
                while (true) {
                    auto frame = framePool.TryGetNextFrame();
                    if (frame == nullptr)
                        break;
                    auto surface = frame.Surface();
                    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                    winrt::com_ptr<ID3D11Texture2D> tex;
                    if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put())))) {
                        if (pendingWgcTex != nullptr)
                            ++droppedFrames;
                        pendingWgcTex = tex;
                    }
                }
            } catch (...) {
            }

            // Pause: discard frames and track paused duration for epoch adjustment on resume
            const bool cfr_paused = m_state.pause_requested.load();
            if (cfr_paused) {
                if (!cfr_was_paused) {
                    cfr_was_paused = true;
                    cfr_pause_start_100ns = Qpc100ns(qpcFreq);
                }
                pendingWgcTex = nullptr;
                Sleep(1);
                continue;
            }
            if (cfr_was_paused) {
                epochQpc100ns += Qpc100ns(qpcFreq) - cfr_pause_start_100ns;
                cfr_was_paused = false;
            }

            // Set epoch on first WGC frame arrival
            if (!videoEpochSet && pendingWgcTex != nullptr) {
                epochQpc100ns = Qpc100ns(qpcFreq);
                videoEpochSet = true;
                next_tick_100ns = 0; // first tick at t=0 relative to epoch
                m_state.video_epoch_qpc_100ns.store(epochQpc100ns);
                lastVideoPts = 0;
            }

            if (!videoEpochSet) {
                Sleep(1);
                continue;
            }

            const uint64_t currentElapsed100ns = Qpc100ns(qpcFreq) - epochQpc100ns;
            bool anyWork = false;

            // Emit CFR frames while we're behind, capped at 1 second to avoid
            // burst workload after process suspension.
            uint64_t catchUpFrames = 0;
            while (currentElapsed100ns >= next_tick_100ns && catchUpFrames < kMaxCatchUpFrames) {
                const uint64_t pts_ns = cfr_frame_idx * frame_interval_ns;

                int32_t slot = nvenc.AcquireFreeSlot();
                if (slot < 0) {
                    ++slotStallCount;
                    break; // retry this tick next outer iteration once a slot is free
                }

                bool frameWritten = false;

                if (pendingWgcTex != nullptr) {
                    // Convert new BGRA WGC frame to NV12 via VideoProcessorBlt
                    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc{};
                    ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                    ivDesc.Texture2D.MipSlice = 0;
                    ivDesc.Texture2D.ArraySlice = 0;

                    winrt::com_ptr<ID3D11VideoProcessorInputView> inputView;
                    hr = videoDevice->CreateVideoProcessorInputView(pendingWgcTex.get(), videoEnum.get(), &ivDesc,
                                                                    inputView.put());

                    if (SUCCEEDED(hr) && inputView != nullptr) {
                        D3D11_VIDEO_PROCESSOR_STREAM stream{};
                        stream.Enable = TRUE;
                        stream.pInputSurface = inputView.get();

                        hr = videoContext->VideoProcessorBlt(videoProcessor.get(), videoOutputViews[slot].get(), 0, 1,
                                                             &stream);
                        inputView = nullptr;

                        if (SUCCEEDED(hr)) {
                            // Save NV12 as reference for future duplicate frames
                            if (refNv12 != nullptr) {
                                d3dContext->CopyResource(refNv12.get(), nv12Textures[slot].get());
                                refNv12Valid = true;
                            }
                            frameWritten = true;
                        }
                    }
                    pendingWgcTex = nullptr;
                } else if (refNv12Valid) {
                    // Duplicate: copy reference NV12 into this slot
                    d3dContext->CopyResource(nv12Textures[slot].get(), refNv12.get());
                    frameWritten = true;
                }

                if (!frameWritten) {
                    // VideoProcessorBlt failed or no reference yet — release slot and skip tick
                    nvenc.ReleaseSlot(slot);
                    ++droppedFrames;
                    cfr_frame_idx++;
                    next_tick_100ns += frame_interval_100ns;
                    anyWork = true;
                    continue;
                }

                EncodedVideoPacket pkt;
                std::string encErr;
                bool encOk = nvenc.EncodeFrame(slot, pts_ns, encodeWidth, encodeHeight, &pkt, encErr);

                if (!encOk) {
                    m_state.RecordFailure(E_FAIL, ErrorPhase::VideoEncode, "NVENC encode (CFR): " + encErr);
                    goto end_encode_loop;
                }

                ++videoFramesCaptured;
                lastVideoPts = pts_ns;

                if (!routePacket(std::move(pkt)))
                    goto end_encode_loop;

                cfr_frame_idx++;
                next_tick_100ns += frame_interval_100ns;
                anyWork = true;
                ++catchUpFrames;
            }

            if (!anyWork)
                Sleep(1);
        }
    } else {
        // ====================================================================
        // VFR path: WGC timestamps passed directly as PTS (explicit passthrough)
        // ====================================================================

        bool videoEpochSet = false;
        int64_t videoEpochTicks100ns = 0;

        bool vfr_was_paused = false;
        uint64_t vfr_pause_start_100ns = 0;

        while (!m_state.stop_requested.load()) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            if (sourceLost) {
                std::lock_guard lk(m_state.stats_mutex);
                m_state.stats.source_loss = true;
                m_state.stop_requested.store(true);
                break;
            }

            bool anyWork = false;

            // Drain WGC frames — keep latest (always drain, even when paused)
            winrt::com_ptr<ID3D11Texture2D> latestTex;
            int64_t latestFrameTicks100ns = 0;

            try {
                while (true) {
                    auto frame = framePool.TryGetNextFrame();
                    if (frame == nullptr)
                        break;

                    auto surface = frame.Surface();
                    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                    winrt::com_ptr<ID3D11Texture2D> tex;
                    if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put())))) {
                        if (latestTex != nullptr)
                            ++droppedFrames;
                        latestTex = tex;
                        latestFrameTicks100ns = frame.SystemRelativeTime().count();
                    }
                }
            } catch (...) {
            }

            // Pause: discard frames and track paused duration for epoch adjustment on resume
            const bool vfr_paused = m_state.pause_requested.load();
            if (vfr_paused) {
                if (!vfr_was_paused) {
                    vfr_was_paused = true;
                    vfr_pause_start_100ns = Qpc100ns(qpcFreq);
                }
                Sleep(1);
                continue;
            }
            if (vfr_was_paused) {
                videoEpochTicks100ns += static_cast<int64_t>(Qpc100ns(qpcFreq) - vfr_pause_start_100ns);
                vfr_was_paused = false;
            }

            if (latestTex != nullptr) {
                // Establish video epoch (also publish for MP4 A/V alignment)
                if (!videoEpochSet) {
                    videoEpochTicks100ns = latestFrameTicks100ns;
                    videoEpochSet = true;
                    // Snapshot QPC at first-frame arrival — must be on the same scale as
                    // session_start_qpc_100ns (both are QPC-derived 100ns-since-boot values).
                    // SystemRelativeTime is NOT QPC-comparable and must not be used here.
                    m_state.video_epoch_qpc_100ns.store(Qpc100ns(qpcFreq));
                }

                int64_t deltaTicks = latestFrameTicks100ns - videoEpochTicks100ns;
                if (deltaTicks < 0)
                    deltaTicks = 0;
                uint64_t framePts_ns = static_cast<uint64_t>(deltaTicks) * 100ULL;
                lastVideoPts = framePts_ns;

                // Acquire a free input slot
                int32_t slot = nvenc.AcquireFreeSlot();

                if (slot >= 0) {
                    // BGRA -> NV12 via VideoProcessorBlt into the selected slot's view
                    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc{};
                    ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                    ivDesc.Texture2D.MipSlice = 0;
                    ivDesc.Texture2D.ArraySlice = 0;

                    winrt::com_ptr<ID3D11VideoProcessorInputView> inputView;
                    hr = videoDevice->CreateVideoProcessorInputView(latestTex.get(), videoEnum.get(), &ivDesc,
                                                                    inputView.put());

                    if (SUCCEEDED(hr) && inputView != nullptr) {
                        D3D11_VIDEO_PROCESSOR_STREAM stream{};
                        stream.Enable = TRUE;
                        stream.pInputSurface = inputView.get();

                        hr = videoContext->VideoProcessorBlt(videoProcessor.get(), videoOutputViews[slot].get(), 0, 1,
                                                             &stream);

                        inputView = nullptr;
                        latestTex = nullptr;

                        if (SUCCEEDED(hr)) {
                            EncodedVideoPacket pkt;
                            std::string encErr;
                            bool encOk = nvenc.EncodeFrame(slot, framePts_ns, encodeWidth, encodeHeight, &pkt, encErr);

                            if (!encOk) {
                                m_state.RecordFailure(E_FAIL, ErrorPhase::VideoEncode, "NVENC encode: " + encErr);
                                break;
                            }

                            ++videoFramesCaptured;

                            if (!routePacket(std::move(pkt)))
                                goto end_encode_loop;
                        } else {
                            latestTex = nullptr;
                        }
                    } else {
                        latestTex = nullptr;
                    }
                } else {
                    // No free slot — drop the frame and skip
                    latestTex = nullptr;
                    ++slotStallCount;
                }

                anyWork = true;
            }

            if (!anyWork)
                Sleep(1);
        }
    }

end_encode_loop:
    // --- Stop capture ---
    try {
        if (captureSession != nullptr) {
            captureSession.Close();
            captureSession = nullptr;
        }
        if (framePool != nullptr) {
            framePool.Close();
            framePool = nullptr;
        }
        try {
            item.Closed(closedToken);
        } catch (...) {
        }
    } catch (...) {
    }

    // --- Flush NVENC EOS ---
    {
        std::vector<EncodedVideoPacket> drainPkts;
        std::string flushErr;
        // flushErr is not escalated — a partial drain is acceptable; any encoded
        // output already in the mux queue is preserved regardless of flush outcome.
        nvenc.Flush(drainPkts, flushErr);

        for (auto& pkt : drainPkts) {
            if (pkt.bytes.empty())
                continue;

            const size_t drain_bytes = pkt.bytes.size();

            if (pkt.keyframe) {
                if (m_state.config.video_codec == VideoCodec::H264Nvenc && !h264CodecPrivateReady) {
                    std::vector<uint8_t> spsPps;
                    if (annexb::ExtractH264SpsAndPps(pkt.bytes.data(), pkt.bytes.size(), spsPps)) {
                        std::lock_guard lk(m_state.premux_mutex);
                        m_state.codec_private.h264_sps_pps = std::move(spsPps);
                        m_state.codec_private.h264_ready = true;
                        h264CodecPrivateReady = true;
                        m_state.premux_cv.notify_all();
                    }
                } else if (m_state.config.video_codec != VideoCodec::H264Nvenc && !av1CodecPrivateReady) {
                    char reason[256] = {};
                    uint8_t cp[4] = {};
                    if (codec_private::DeriveAv1CodecPrivate(pkt.bytes.data(), pkt.bytes.size(), cp, reason,
                                                             sizeof(reason))) {
                        std::lock_guard lk(m_state.premux_mutex);
                        std::memcpy(m_state.codec_private.av1_codec_private, cp, 4);
                        m_state.codec_private.av1_ready = true;
                        av1CodecPrivateReady = true;
                        m_state.premux_cv.notify_all();
                    }
                }
            }

            {
                std::unique_lock lk(m_state.premux_mutex);
                bool bothReady = m_state.codec_private.VideoReady(m_state.config.video_codec) &&
                                 m_state.codec_private.AudioAllReady(m_state.audio_track_count);
                if (!bothReady) {
                    if (m_state.video_premux.size() < SessionState::kVideoPremuxLimit) {
                        m_state.video_premux.push_back(std::move(pkt));
                    }
                } else {
                    lk.unlock();
                    MuxItem mi;
                    mi.payload = std::move(pkt);
                    std::lock_guard mlk(m_state.mux_mutex);
                    m_state.mux_queue.push_back(std::move(mi));
                    m_state.mux_cv.notify_one();
                }
            }

            {
                std::lock_guard slk(m_state.stats_mutex);
                m_state.stats.encoded_video_packets++;
                m_state.stats.video_bytes += drain_bytes;
            }
        }
    }

    // --- Unregister NVENC resources + destroy ---
    nvenc.UnregisterAllSlots();
    nvenc.Destroy();

    // --- Update final stats ---
    {
        std::lock_guard lk(m_state.stats_mutex);
        m_state.stats.video_frames_captured = videoFramesCaptured;
        m_state.stats.dropped_or_skipped_video_frames = droppedFrames + slotStallCount;
        m_state.stats.video_duration_ns = lastVideoPts;
    }

    // --- Push video EOS sentinel ---
    {
        MuxItem eos;
        eos.payload = VideoEosSentinel{};
        std::lock_guard lk(m_state.mux_mutex);
        m_state.mux_queue.push_back(std::move(eos));
        m_state.mux_cv.notify_one();
    }

    // Cleanup slot views and textures
    for (int32_t i = 0; i < kSlotCount; ++i) {
        videoOutputViews[i] = nullptr;
        nv12Textures[i] = nullptr;
    }
    videoProcessor = nullptr;
    videoEnum = nullptr;
    videoContext = nullptr;
    videoDevice = nullptr;
    d3dContext = nullptr;
    d3dDevice = nullptr;

    if (com_inited && hr != RPC_E_CHANGED_MODE)
        CoUninitialize();
}

} // namespace recorder_core
