#include "video_thread.h"

#include "annexb_to_avcc.h"
#include "annexb_to_hvcc.h"
#include "codec_private.h"
#include "dxgi_od_capture_src.h"
#include "gpu_compositor.h"
#include "nvenc_video_encoder.h"
#include "session_internal.h"

#include <recorder_core/logging/logging.h>
#include <recorder_core/packet_types.h>
#include <recorder_core/webcam_placement.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <dwmapi.h>
#include <dxgi.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <sstream>
#include <vector>

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

struct Win32CursorBitmap {
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    int hotspot_x = 0;
    int hotspot_y = 0;
};

bool CaptureWin32CursorBitmap(HCURSOR cursor, Win32CursorBitmap& out) {
    if (cursor == nullptr) {
        return false;
    }

    ICONINFO icon{};
    if (GetIconInfo(cursor, &icon) == FALSE) {
        return false;
    }

    auto cleanup = [&]() {
        if (icon.hbmColor != nullptr) {
            DeleteObject(icon.hbmColor);
        }
        if (icon.hbmMask != nullptr) {
            DeleteObject(icon.hbmMask);
        }
    };

    BITMAP bitmap{};
    int width = 0;
    int height = 0;
    if (icon.hbmColor != nullptr && GetObjectW(icon.hbmColor, sizeof(bitmap), &bitmap) != 0) {
        width = bitmap.bmWidth;
        height = bitmap.bmHeight;
    } else if (icon.hbmMask != nullptr && GetObjectW(icon.hbmMask, sizeof(bitmap), &bitmap) != 0) {
        width = bitmap.bmWidth;
        height = bitmap.bmHeight / 2;
    }

    if (width <= 0 || height <= 0 || width > 256 || height > 256) {
        cleanup();
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr) {
        cleanup();
        return false;
    }
    HBITMAP dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        if (dib != nullptr) {
            DeleteObject(dib);
        }
        DeleteDC(dc);
        cleanup();
        return false;
    }

    HGDIOBJ old = SelectObject(dc, dib);
    std::memset(bits, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    const BOOL drawn = DrawIconEx(dc, 0, 0, cursor, width, height, 0, nullptr, DI_NORMAL);
    if (old != nullptr) {
        SelectObject(dc, old);
    }

    if (drawn != FALSE) {
        out.width = width;
        out.height = height;
        out.hotspot_x = static_cast<int>(icon.xHotspot);
        out.hotspot_y = static_cast<int>(icon.yHotspot);
        out.bgra.assign(static_cast<const uint8_t*>(bits),
                        static_cast<const uint8_t*>(bits) + static_cast<size_t>(width) * height * 4u);
    }

    DeleteObject(dib);
    DeleteDC(dc);
    cleanup();
    return drawn != FALSE;
}

int ScaleCoordinateToSource(LONG screen_delta, int source_pixels, int bounds_pixels) noexcept {
    if (bounds_pixels <= 0 || source_pixels <= 0) {
        return static_cast<int>(screen_delta);
    }
    const int64_t numerator = static_cast<int64_t>(screen_delta) * source_pixels;
    const int64_t rounded = numerator >= 0 ? numerator + bounds_pixels / 2 : numerator - bounds_pixels / 2;
    return static_cast<int>(rounded / bounds_pixels);
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

    const CaptureTarget& target = m_state.config.target;
    const HWND targetHwnd =
        (target.kind == CaptureTarget::Kind::Window) ? reinterpret_cast<HWND>(target.native_id) : nullptr;
    const bool useOdCapture = (target.kind == CaptureTarget::Kind::Monitor);

    {
        const char* backend = useOdCapture ? "dxgi_od" : "wgc";
        logging::LogField fields[] = {
            {"backend", backend}, {"target_kind", TargetKindName(target.kind)}, {"target_desc", target.description}};
        logging::log(logging::LogLevel::Info, "video_thread", "capture session starting",
                     std::span<const logging::LogField>(fields, std::size(fields)));
    }

    // For Monitor targets, find the adapter owning the HMONITOR so DXGI OD works
    // on multi-GPU systems. Fall back to default adapter on failure.
    winrt::com_ptr<IDXGIAdapter1> monitorAdapter;
    if (useOdCapture) {
        std::string adapterErr;
        FindAdapterForMonitor(reinterpret_cast<HMONITOR>(target.native_id), monitorAdapter.put(), adapterErr);
    }

    // --- D3D11 video-capable device (exclusive to this thread's context) ---
    winrt::com_ptr<ID3D11Device> d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    winrt::com_ptr<ID3D11VideoDevice> videoDevice;
    winrt::com_ptr<ID3D11VideoContext> videoContext;
    // Optional newer interface: VideoProcessorSet{Stream,Output}ColorSpace1 takes
    // explicit DXGI_COLOR_SPACE_TYPE enums, which drivers honour for the YUV
    // quantization range. The legacy D3D11_VIDEO_PROCESSOR_COLOR_SPACE.Nominal_Range
    // is widely ignored on output (NVIDIA included), so the studio-range request was
    // silently dropped and full-range YUV was tagged as limited (washed-out / dark).
    winrt::com_ptr<ID3D11VideoContext1> videoContext1;

    {
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

        if (monitorAdapter) {
            // Adapter-matched device: required for DuplicateOutput on multi-GPU systems.
            hr = D3D11CreateDevice(monitorAdapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, d3dDevice.put(), nullptr,
                                   d3dContext.put());
        } else {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, d3dDevice.put(), nullptr,
                                   d3dContext.put());
        }

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
        // Optional — present on all Windows 10+; failure is non-fatal (legacy path).
        videoContext->QueryInterface(IID_PPV_ARGS(videoContext1.put()));
    }

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

    // --- Capture backend init ---
    // Monitor  → DXGI Output Duplication (no VRR interference, no capture indicator)
    // Window   → WGC GraphicsCaptureSession (only option for window/app capture)

    DxgiOdCaptureSrc odSrc;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};

    if (useOdCapture) {
        std::string odErr;
        if (!odSrc.Open(d3dDevice.get(), reinterpret_cast<HMONITOR>(target.native_id), odErr)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoCapture, "DXGI OD open: " + odErr);
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
    } else {
        try {
            auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                                         IGraphicsCaptureItemInterop>();
            winrt::check_hresult(interop->CreateForWindow(
                reinterpret_cast<HWND>(target.native_id),
                winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
        } catch (const winrt::hresult_error& e) {
            char buf[96];
            snprintf(buf, sizeof(buf), "WGC CreateForWindow failed 0x%08X", static_cast<unsigned int>(e.code().value));
            m_state.RecordFailure(static_cast<HRESULT>(e.code().value), ErrorPhase::VideoCapture, buf);
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
    }

    // Capture dimensions
    const int32_t sourceWidthSigned =
        useOdCapture ? static_cast<int32_t>(odSrc.Width()) : static_cast<int32_t>(item.Size().Width);
    const int32_t sourceHeightSigned =
        useOdCapture ? static_cast<int32_t>(odSrc.Height()) : static_cast<int32_t>(item.Size().Height);

    std::ostringstream diag;
    diag << "target.kind=" << TargetKindName(target.kind) << ", target.description=\"" << target.description
         << "\", target.native_id=0x" << std::hex << target.native_id << std::dec
         << ", capture.visibleContentSize=" << sourceWidthSigned << "x" << sourceHeightSigned
         << ", capture.dxgiFormat=DXGI_FORMAT_B8G8R8A8_UNORM"
         << ", videoCodec="
         << (m_state.config.video_codec == VideoCodec::H264Nvenc
                 ? "H264_NVENC"
                 : (m_state.config.video_codec == VideoCodec::HevcNvenc ? "HEVC_NVENC" : "AV1_NVENC"))
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
        err << "capture source size invalid (<=0): " << sourceWidthSigned << "x" << sourceHeightSigned << "; preInit={"
            << diag.str() << "}";
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::VideoCapture, err.str());
        if (com_inited && hr != RPC_E_CHANGED_MODE)
            CoUninitialize();
        return;
    }

    uint32_t sourceWidth = static_cast<uint32_t>(sourceWidthSigned);
    uint32_t sourceHeight = static_cast<uint32_t>(sourceHeightSigned);

    RECT wgcCursorBounds = windowRect;
    if (target.kind == CaptureTarget::Kind::Window) {
        RECT extendedFrameBounds{};
        if (SUCCEEDED(DwmGetWindowAttribute(targetHwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &extendedFrameBounds,
                                            sizeof(extendedFrameBounds))) &&
            RectWidth(extendedFrameBounds) > 0 && RectHeight(extendedFrameBounds) > 0) {
            wgcCursorBounds = extendedFrameBounds;
        }
    }

    // Determine crop region in monitor-local pixel coordinates.
    // CaptureRegion uses virtual-screen coordinates; subtract the monitor origin.
    bool hasCrop = false;
    const bool cropRequested = m_state.config.crop_region.has_value() && target.kind == CaptureTarget::Kind::Monitor;
    int32_t cropX = 0, cropY = 0;
    int32_t cropW = static_cast<int32_t>(sourceWidth);
    int32_t cropH = static_cast<int32_t>(sourceHeight);

    if (cropRequested) {
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

    if (cropRequested && !hasCrop) {
        std::ostringstream err;
        err << "requested Region is outside the selected monitor or too small after clamping; preInit={" << diag.str()
            << "}";
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::VideoCapture, err.str());
        if (com_inited && hr != RPC_E_CHANGED_MODE)
            CoUninitialize();
        return;
    }

    const uint32_t sourceContentWidth = hasCrop ? static_cast<uint32_t>(cropW) : sourceWidth;
    const uint32_t sourceContentHeight = hasCrop ? static_cast<uint32_t>(cropH) : sourceHeight;
    const bool fixedOutputRequested = m_state.config.output_width != 0 || m_state.config.output_height != 0;
    uint32_t encodeWidth =
        fixedOutputRequested ? m_state.config.output_width : AlignOutputDimensionEven(sourceContentWidth);
    uint32_t encodeHeight =
        fixedOutputRequested ? m_state.config.output_height : AlignOutputDimensionEven(sourceContentHeight);

    const bool dimsZero = (encodeWidth == 0 || encodeHeight == 0);
    const bool dimsEven = ((encodeWidth % 2u) == 0u) && ((encodeHeight % 2u) == 0u);
    diag << ", sourceContentSize=" << sourceContentWidth << "x" << sourceContentHeight
         << ", requestedOutputSize=" << m_state.config.output_width << "x" << m_state.config.output_height
         << ", encodeSize=" << encodeWidth << "x" << encodeHeight << ", encode.dimsZero=" << BoolText(dimsZero)
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

    const std::optional<OutputGeometry> outputGeometry =
        ResolveOutputGeometry({sourceContentWidth, sourceContentHeight}, {encodeWidth, encodeHeight});
    if (!outputGeometry.has_value()) {
        std::ostringstream err;
        err << "output geometry invalid for source " << sourceContentWidth << "x" << sourceContentHeight
            << " and output " << encodeWidth << "x" << encodeHeight << "; preInit={" << diag.str() << "}";
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::VideoCapture, err.str());
        if (com_inited && hr != RPC_E_CHANGED_MODE)
            CoUninitialize();
        return;
    }
    const ContentRect contentRect = outputGeometry->content;

    {
        std::lock_guard lk(m_state.stats_mutex);
        m_state.encode_width = encodeWidth;
        m_state.encode_height = encodeHeight;
        m_state.stats.source_size = {sourceContentWidth, sourceContentHeight};
        m_state.stats.output_size = {encodeWidth, encodeHeight};
        m_state.stats.content_rect = contentRect;
        m_state.stats.frame_rate_num = m_state.config.frame_rate_num;
        m_state.stats.frame_rate_den = m_state.config.frame_rate_den;
        m_state.stats.cfr = m_state.config.cfr;
        m_state.stats.container = m_state.config.container;
        m_state.stats.video_codec = m_state.config.video_codec;
        m_state.stats.audio_codec = m_state.config.audio_codec;
    }

    // --- NVENC encoder ---
    NvencVideoEncoder nvenc;
    {
        nvenc.SetCodec(m_state.config.video_codec);
        nvenc.SetBitDepth(m_state.config.bit_depth);
        nvenc.SetQualityPreset(m_state.config.nvenc_quality_preset);
        nvenc.SetRateControl(m_state.config.nvenc_rate_control, m_state.config.nvenc_bitrate_kbps);

        std::string err;
        if (!nvenc.Open(d3dDevice.get(), err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::Prepare, "NVENC open: " + err);
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
        if (!nvenc.Configure(encodeWidth, encodeHeight, m_state.config.frame_rate_num, m_state.config.frame_rate_den,
                             err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoEncode,
                                  "NVENC configure: " + err + "; preInit={" + diag.str() + "}");
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
    }

    // --- NV12 / P010 texture ring + video processor ---
    // For 10-bit recording (HEVC Main10 / AV1 10-bit, ADR 0032 SDR BT.709) the
    // VideoProcessor converts BGRA → P010 instead of NV12, and the encode ring +
    // reference texture use DXGI_FORMAT_P010. The output color space stays studio
    // BT.709 (no HDR/BT.2020 here — that is a later slice).
    const bool tenBit = (m_state.config.bit_depth == BitDepth::Bit10);
    const DXGI_FORMAT encodeFormat = tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    static constexpr int32_t kSlotCount = 8;
    winrt::com_ptr<ID3D11Texture2D> nv12Textures[kSlotCount];
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> videoEnum;
    winrt::com_ptr<ID3D11VideoProcessor> videoProcessor;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> videoOutputViews[kSlotCount];

    {
        // Video processor enumerator (shared across all slots)
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc{};
        contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        contentDesc.InputWidth = sourceWidth;
        contentDesc.InputHeight = sourceHeight;
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
            desc.Format = encodeFormat; // NV12 (8-bit) or P010 (10-bit)
            desc.SampleDesc = {1, 0};
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;

            hr = d3dDevice->CreateTexture2D(&desc, nullptr, nv12Textures[i].put());
            if (FAILED(hr)) {
                char buf[96];
                snprintf(buf, sizeof(buf), "CreateTexture2D(%s[%d]) failed 0x%08lX", tenBit ? "P010" : "NV12",
                         static_cast<int>(i), static_cast<unsigned long>(hr));
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

    // Apply source crop, contain-fit destination, and black letterbox bars once.
    // VideoProcessorBlt handles BGRA->NV12 conversion and GPU scaling.
    {
        D3D11_VIDEO_COLOR background{};
        background.RGBA.A = 1.0f;
        videoContext->VideoProcessorSetOutputBackgroundColor(videoProcessor.get(), FALSE, &background);

        // Make the RGB->NV12 conversion deterministic (ADR 0032). Without an
        // explicit color space the driver picks an implementation-defined
        // matrix/range (BT.601 vs BT.709, full vs studio), so the same desktop
        // could encode to subtly different colors on different GPUs and the
        // container carried no color tags at all. Pin the input to full-range
        // RGB (the desktop composite) and the NV12 output to studio-range
        // BT.709 — the SDR HD standard the Matroska Colour element is tagged
        // with (see color_metadata.h / RecorderConfig::color). Input and output
        // ranges therefore agree, so there is no black-level mismatch.
        if (videoContext1) {
            // Preferred path: explicit DXGI colour spaces. Input is the desktop's
            // full-range BT.709 RGB; output is studio (limited 16-235) BT.709 YUV.
            // Drivers honour the quantization range here, so the encoded NV12/P010
            // is genuinely limited-range and matches the limited tag the container
            // carries (ADR 0032) — no full-vs-limited mismatch, no crushed shadows.
            videoContext1->VideoProcessorSetStreamColorSpace1(videoProcessor.get(), 0,
                                                              DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            videoContext1->VideoProcessorSetOutputColorSpace1(videoProcessor.get(),
                                                              DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        } else {
            // Legacy fallback (pre-Win10 / no ID3D11VideoContext1). The output
            // Nominal_Range is widely ignored here, so the result may be full-range.
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace{};
            inputColorSpace.Usage = 0;     // 0 = playback (full-precision conversion)
            inputColorSpace.RGB_Range = 0; // 0 = full-range RGB (0-255)
            inputColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
            videoContext->VideoProcessorSetStreamColorSpace(videoProcessor.get(), 0, &inputColorSpace);

            D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace{};
            outputColorSpace.Usage = 0;
            outputColorSpace.YCbCr_Matrix = 1;                                           // 1 = BT.709
            outputColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235; // studio/limited
            videoContext->VideoProcessorSetOutputColorSpace(videoProcessor.get(), &outputColorSpace);
        }

        const RECT targetRect = {0, 0, static_cast<LONG>(encodeWidth), static_cast<LONG>(encodeHeight)};
        videoContext->VideoProcessorSetOutputTargetRect(videoProcessor.get(), TRUE, &targetRect);

        const RECT srcRect = {static_cast<LONG>(hasCrop ? cropX : 0), static_cast<LONG>(hasCrop ? cropY : 0),
                              static_cast<LONG>((hasCrop ? cropX : 0) + static_cast<int32_t>(sourceContentWidth)),
                              static_cast<LONG>((hasCrop ? cropY : 0) + static_cast<int32_t>(sourceContentHeight))};
        videoContext->VideoProcessorSetStreamSourceRect(videoProcessor.get(), 0, TRUE, &srcRect);

        const RECT dstRect = {static_cast<LONG>(contentRect.x), static_cast<LONG>(contentRect.y),
                              static_cast<LONG>(contentRect.x + contentRect.width),
                              static_cast<LONG>(contentRect.y + contentRect.height)};
        videoContext->VideoProcessorSetStreamDestRect(videoProcessor.get(), 0, TRUE, &dstRect);
    }

    // --- DXGI OD captured frame texture + cursor resources ---
    // odCapturedTex: persistent BGRA texture we CopyResource into after each DXGI OD acquire.
    // DXGI OD textures are owned by the duplication interface and must be released before the
    // next AcquireNextFrame, so we always copy to this texture first.
    winrt::com_ptr<ID3D11Texture2D> odCapturedTex;
    bool odCapturedTexValid = false;
    bool odCursorShapeValid = false;
    bool odCursorVisible = false;
    int32_t odCursorPosX = 0;
    int32_t odCursorPosY = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO odCursorShapeInfo{};
    std::vector<uint8_t> odCursorBitmap;
    std::vector<uint8_t> odCursorUploadBgra;
    HCURSOR wgcCursorHandle = nullptr;
    Win32CursorBitmap wgcCursorBitmap;
    std::vector<uint8_t> wgcCursorUploadBgra;

    if (useOdCapture) {
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = sourceWidth;
            desc.Height = sourceHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc = {1, 0};
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;

            HRESULT odHr = d3dDevice->CreateTexture2D(&desc, nullptr, odCapturedTex.put());
            if (FAILED(odHr)) {
                char buf[80];
                snprintf(buf, sizeof(buf), "CreateTexture2D(odCapturedTex) failed 0x%08lX",
                         static_cast<unsigned long>(odHr));
                m_state.RecordFailure(odHr, ErrorPhase::Prepare, buf);
                if (com_inited)
                    CoUninitialize();
                return;
            }
        }
    }

    // --- GPU compositing resources ---
    // OD and WGC compositing operate in source coordinates. VideoProcessorBlt then
    // applies crop, contain-fit scaling, letterbox background, and BGRA->NV12.
    const bool webcamProviderAvailable = (m_state.config.webcam.frame_provider != nullptr);
    const bool needsGpuCompositor = webcamProviderAvailable || m_state.config.capture_cursor;
    const uint32_t compositorWidth = sourceWidth;
    const uint32_t compositorHeight = sourceHeight;

    GpuCompositor gpuCompositor;
    bool gpuCompositorReady = false;
    if (needsGpuCompositor) {
        std::string compErr;
        if (!gpuCompositor.Init(d3dDevice.get(), d3dContext.get(), compositorWidth, compositorHeight, compErr)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::Prepare, "GPU compositor init: " + compErr);
            if (com_inited)
                CoUninitialize();
            return;
        }
        gpuCompositorReady = true;
    }

    std::vector<uint8_t> camBgra;
    auto webcamRectFor = [&](const WebcamOverlayLive& overlay) {
        WebcamPlacement placement;
        placement.x = overlay.overlay_x_norm;
        placement.y = overlay.overlay_y_norm;
        placement.w = overlay.overlay_w_norm;
        placement.h = overlay.overlay_h_norm;
        placement.mirror = overlay.mirror;

        const int contentX = (useOdCapture && hasCrop) ? cropX : 0;
        const int contentY = (useOdCapture && hasCrop) ? cropY : 0;
        return MapWebcamPlacementToContent(placement, contentX, contentY, static_cast<int>(sourceContentWidth),
                                           static_cast<int>(sourceContentHeight));
    };

    auto drawWebcamGpu = [&](const WebcamOverlayLive& overlay) -> bool {
        if (!overlay.enabled || m_state.config.webcam.frame_provider == nullptr) {
            return true;
        }

        int camW = 0;
        int camH = 0;
        if (!m_state.config.webcam.frame_provider->TryGetFrame(camW, camH, camBgra)) {
            return true;
        }
        const size_t required = (camW > 0 && camH > 0) ? static_cast<size_t>(camW) * camH * 4 : 0;
        if (camW <= 0 || camH <= 0 || camBgra.size() < required) {
            return true;
        }

        const WebcamPixelRect rect = webcamRectFor(overlay);
        if (rect.w < 2 || rect.h < 2) {
            return true;
        }

        GpuCompositor::ChromaKeyParams chroma;
        chroma.enabled = overlay.chroma_key_enabled;
        chroma.r = overlay.chroma_r;
        chroma.g = overlay.chroma_g;
        chroma.b = overlay.chroma_b;
        chroma.tolerance = overlay.chroma_tolerance;
        chroma.softness = overlay.chroma_softness;
        chroma.spill_reduction = overlay.chroma_spill_reduction;

        std::string compErr;
        if (!gpuCompositor.DrawWebcam(camBgra.data(), camW, camH, rect, overlay.mirror, chroma, compErr)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoCapture, "GPU webcam composite: " + compErr);
            return false;
        }
        return true;
    };

    auto drawCursorGpu = [&]() -> bool {
        if (!useOdCapture || !m_state.config.capture_cursor || !odCursorVisible || !odCursorShapeValid) {
            return true;
        }
        if (odCursorShapeInfo.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR &&
            odCursorShapeInfo.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
            return true;
        }
        if (odCursorBitmap.empty()) {
            return true;
        }

        int32_t cx = odCursorPosX;
        int32_t cy = odCursorPosY;
        int32_t cw = static_cast<int32_t>(odCursorShapeInfo.Width);
        int32_t ch = static_cast<int32_t>(odCursorShapeInfo.Height);

        int32_t bitmapOffX = 0;
        int32_t bitmapOffY = 0;
        if (cx < 0) {
            bitmapOffX = -cx;
            cw += cx;
            cx = 0;
        }
        if (cy < 0) {
            bitmapOffY = -cy;
            ch += cy;
            cy = 0;
        }

        const int32_t targetW = static_cast<int32_t>(compositorWidth);
        const int32_t targetH = static_cast<int32_t>(compositorHeight);
        const int32_t maxW = targetW - cx;
        const int32_t maxH = targetH - cy;
        if (cw > maxW)
            cw = maxW;
        if (ch > maxH)
            ch = maxH;
        if (cw <= 0 || ch <= 0 || cw > 256 || ch > 256)
            return true;

        const uint32_t pitch =
            odCursorShapeInfo.Pitch != 0 ? odCursorShapeInfo.Pitch : static_cast<uint32_t>(odCursorShapeInfo.Width * 4);
        const size_t minBytes = static_cast<size_t>(odCursorShapeInfo.Height - 1) * pitch +
                                static_cast<size_t>(odCursorShapeInfo.Width) * 4;
        if (odCursorBitmap.size() < minBytes) {
            return true;
        }

        odCursorUploadBgra.resize(static_cast<size_t>(cw) * ch * 4);
        for (int32_t row = 0; row < ch; ++row) {
            const size_t srcOff = static_cast<size_t>(bitmapOffY + row) * pitch + static_cast<size_t>(bitmapOffX) * 4;
            const uint8_t* srcRow = odCursorBitmap.data() + srcOff;
            uint8_t* dstRow = odCursorUploadBgra.data() + static_cast<size_t>(row) * cw * 4;
            std::memcpy(dstRow, srcRow, static_cast<size_t>(cw) * 4);
        }

        WebcamPixelRect rect;
        rect.x = cx;
        rect.y = cy;
        rect.w = cw;
        rect.h = ch;

        std::string compErr;
        if (!gpuCompositor.DrawCursor(odCursorUploadBgra.data(), cw, ch, rect, compErr)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoCapture, "GPU cursor composite: " + compErr);
            return false;
        }
        return true;
    };

    auto drawWin32CursorGpu = [&]() -> bool {
        if (useOdCapture || !m_state.config.capture_cursor) {
            return true;
        }

        CURSORINFO cursorInfo{};
        cursorInfo.cbSize = sizeof(cursorInfo);
        if (GetCursorInfo(&cursorInfo) == FALSE || (cursorInfo.flags & CURSOR_SHOWING) == 0 ||
            cursorInfo.hCursor == nullptr) {
            return true;
        }

        if (cursorInfo.hCursor != wgcCursorHandle || wgcCursorBitmap.bgra.empty()) {
            Win32CursorBitmap next;
            if (!CaptureWin32CursorBitmap(cursorInfo.hCursor, next)) {
                return true;
            }
            wgcCursorHandle = cursorInfo.hCursor;
            wgcCursorBitmap = std::move(next);
        }

        const int boundsW = RectWidth(wgcCursorBounds);
        const int boundsH = RectHeight(wgcCursorBounds);
        if (boundsW <= 0 || boundsH <= 0) {
            return true;
        }

        int32_t cx = ScaleCoordinateToSource(cursorInfo.ptScreenPos.x - wgcCursorBounds.left,
                                             static_cast<int>(sourceWidth), boundsW) -
                     wgcCursorBitmap.hotspot_x;
        int32_t cy = ScaleCoordinateToSource(cursorInfo.ptScreenPos.y - wgcCursorBounds.top,
                                             static_cast<int>(sourceHeight), boundsH) -
                     wgcCursorBitmap.hotspot_y;
        int32_t cw = wgcCursorBitmap.width;
        int32_t ch = wgcCursorBitmap.height;

        int32_t bitmapOffX = 0;
        int32_t bitmapOffY = 0;
        if (cx < 0) {
            bitmapOffX = -cx;
            cw += cx;
            cx = 0;
        }
        if (cy < 0) {
            bitmapOffY = -cy;
            ch += cy;
            cy = 0;
        }

        const int32_t targetW = static_cast<int32_t>(sourceWidth);
        const int32_t targetH = static_cast<int32_t>(sourceHeight);
        const int32_t maxW = targetW - cx;
        const int32_t maxH = targetH - cy;
        if (cw > maxW)
            cw = maxW;
        if (ch > maxH)
            ch = maxH;
        if (cw <= 0 || ch <= 0 || cw > 256 || ch > 256) {
            return true;
        }

        wgcCursorUploadBgra.resize(static_cast<size_t>(cw) * ch * 4);
        for (int32_t row = 0; row < ch; ++row) {
            const size_t srcOff = (static_cast<size_t>(bitmapOffY + row) * wgcCursorBitmap.width + bitmapOffX) * 4u;
            const uint8_t* srcRow = wgcCursorBitmap.bgra.data() + srcOff;
            uint8_t* dstRow = wgcCursorUploadBgra.data() + static_cast<size_t>(row) * cw * 4u;
            std::memcpy(dstRow, srcRow, static_cast<size_t>(cw) * 4u);
        }

        WebcamPixelRect rect;
        rect.x = cx;
        rect.y = cy;
        rect.w = cw;
        rect.h = ch;

        std::string compErr;
        if (!gpuCompositor.DrawCursor(wgcCursorUploadBgra.data(), cw, ch, rect, compErr)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoCapture, "GPU WGC cursor composite: " + compErr);
            return false;
        }
        return true;
    };

    auto compositeFrameGpu = [&](ID3D11Texture2D* source, const WebcamOverlayLive& overlay) -> ID3D11Texture2D* {
        if (!gpuCompositorReady || source == nullptr) {
            return source;
        }

        const bool webcamActive = overlay.enabled && webcamProviderAvailable;
        const bool cursorActive =
            m_state.config.capture_cursor && (!useOdCapture || (odCursorVisible && odCursorShapeValid));
        if (!webcamActive && !cursorActive) {
            return source;
        }

        std::string compErr;
        if (!gpuCompositor.BeginFrame(source, compErr)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoCapture, "GPU compositor begin: " + compErr);
            return nullptr;
        }
        if (!drawWebcamGpu(overlay)) {
            return nullptr;
        }
        if (!drawCursorGpu()) {
            return nullptr;
        }
        if (!drawWin32CursorGpu()) {
            return nullptr;
        }
        return gpuCompositor.Result();
    };

    // --- WGC frame pool and session (Window-only path) ---
    bool sourceLost = false;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession captureSession{nullptr};
    winrt::event_token closedToken{};

    if (!useOdCapture) {
        try {
            winrt::com_ptr<IDXGIDevice> dxgiDev = d3dDevice.as<IDXGIDevice>();
            winrt::com_ptr<IInspectable> insp;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.get(), insp.put()));
            auto d3dWinRTDev = insp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

            auto capSz = item.Size();
            framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
                d3dWinRTDev, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, capSz);

            captureSession = framePool.CreateCaptureSession(item);
            captureSession.IsBorderRequired(false);
            // WGC's built-in cursor would be baked into the source frame before
            // webcam composition. Draw it manually through the GPU compositor so
            // cursor z-order matches DXGI Output Duplication.
            captureSession.IsCursorCaptureEnabled(false);
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
    } // end if (!useOdCapture) — WGC session init

    // --- Wait for first frame (5 s timeout) ---
    {
        LARGE_INTEGER freq{}, tStart{}, tNow{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&tStart);
        constexpr double kTimeoutSec = 5.0;
        bool gotFirst = false;

        while (!gotFirst && !m_state.stop_requested.load()) {
            if (!useOdCapture) {
                MSG msg{};
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }

            QueryPerformanceCounter(&tNow);
            double elapsed = static_cast<double>(tNow.QuadPart - tStart.QuadPart) / static_cast<double>(freq.QuadPart);
            if (elapsed > kTimeoutSec) {
                const char* which = useOdCapture ? "DXGI OD" : "WGC";
                char buf[80];
                snprintf(buf, sizeof(buf), "%s: timeout waiting for first frame (5 s)", which);
                m_state.RecordFailure(HRESULT_FROM_WIN32(ERROR_TIMEOUT), ErrorPhase::VideoCapture, buf);
                if (!useOdCapture) {
                    if (captureSession != nullptr)
                        captureSession.Close();
                    if (framePool != nullptr)
                        framePool.Close();
                }
                if (com_inited)
                    CoUninitialize();
                return;
            }

            if (!useOdCapture && sourceLost) {
                m_state.RecordFailure(E_ABORT, ErrorPhase::VideoCapture, "WGC: source lost before first frame");
                if (captureSession != nullptr)
                    captureSession.Close();
                if (framePool != nullptr)
                    framePool.Close();
                if (com_inited)
                    CoUninitialize();
                return;
            }

            if (useOdCapture) {
                // DXGI OD: try a 16ms blocking acquire (one-frame wait interval)
                ID3D11Texture2D* rawTex = nullptr;
                DXGI_OUTDUPL_FRAME_INFO info{};
                HRESULT odHr = S_OK;
                if (odSrc.TryAcquireFrame(16, &rawTex, &info, &odHr)) {
                    d3dContext->CopyResource(odCapturedTex.get(), rawTex);
                    rawTex->Release();
                    if (info.PointerShapeBufferSize > 0 && m_state.config.capture_cursor) {
                        if (odSrc.GetFramePointerShape(&odCursorShapeInfo, odCursorBitmap))
                            odCursorShapeValid = true;
                    }
                    if (info.LastMouseUpdateTime.QuadPart != 0) {
                        odCursorVisible = info.PointerPosition.Visible != FALSE;
                        odCursorPosX = info.PointerPosition.Position.x;
                        odCursorPosY = info.PointerPosition.Position.y;
                    }
                    odSrc.ReleaseFrame();
                    odCapturedTexValid = true;
                    gotFirst = true;
                } else if (odHr == DXGI_ERROR_ACCESS_LOST) {
                    m_state.RecordFailure(odHr, ErrorPhase::VideoCapture, "DXGI OD: access lost before first frame");
                    if (com_inited)
                        CoUninitialize();
                    return;
                }
                // DXGI_ERROR_WAIT_TIMEOUT: no frame yet, loop again
            } else {
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
    }

    // --- Capture + encode loop ---
    bool av1CodecPrivateReady = false;
    bool h264CodecPrivateReady = false;
    bool hevcCodecPrivateReady = false;
    uint64_t lastVideoPts = 0;
    uint64_t videoFramesCaptured = 0;
    uint64_t droppedFrames = 0;
    uint64_t duplicatedFrames = 0;
    uint64_t slotStallCount = 0;

    // --- Split-recording boundary coordination (SPLIT-RECORDING-R1) ---
    // VideoThread owns the segment boundary because it owns both the media
    // timeline (encoded-frame session PTS) and the forced-IDR arming point.
    //
    // current_segment_index: 0-based index of the segment currently being filled.
    // segment_start_session_pts_ns: session PTS at which the current segment began
    //   (0 for the first segment); used to keep the auto interval per-segment so a
    //   manual split resets the auto timer.
    // next_auto_threshold_ns: session PTS at which the next AUTOMATIC split fires;
    //   UINT64_MAX disables (mode Off). Recomputed after every boundary.
    // split_last_seq: last split_request_seq value VideoThread has acted on.
    // split_armed: a boundary has been decided and a forced IDR requested; the next
    //   routed keyframe is the first frame of the new segment.
    // split_armed_trigger: trigger that armed the pending boundary (for logging).
    uint32_t current_segment_index = 0;
    uint64_t segment_start_session_pts_ns = 0;
    uint64_t split_last_seq = m_state.split_request_seq.load();
    bool split_armed = false;
    SplitTriggerSource split_armed_trigger = SplitTriggerSource::ManualButton;

    const bool split_auto_enabled = (m_state.config.split.duration_ms > 0);
    const uint64_t split_auto_interval_ns = split_auto_enabled ? (m_state.config.split.duration_ms * 1000000ULL) : 0ULL;
    uint64_t next_auto_threshold_ns =
        split_auto_enabled ? split_auto_interval_ns : (~0ULL); // first auto split one interval in

    // Decide whether a split boundary should be armed for the frame about to be
    // encoded at session PTS `pts_ns`. Arms the encoder forced-IDR exactly once
    // per boundary. Manual (seq bump) and automatic (media-time threshold) share
    // this single arming point; manual implicitly resets the auto interval because
    // the threshold is recomputed off the new segment start when the boundary
    // actually lands (in routePacket).
    auto maybeArmSplit = [&](uint64_t pts_ns) {
        if (split_armed)
            return; // a boundary is already pending; coalesce further requests
        const uint64_t seq = m_state.split_request_seq.load();
        bool manual = (seq != split_last_seq);
        bool automatic = (pts_ns >= next_auto_threshold_ns);
        if (!manual && !automatic)
            return;
        split_last_seq = seq; // consume manual requests up to here (coalesced)
        split_armed = true;
        split_armed_trigger = manual ? static_cast<SplitTriggerSource>(m_state.split_last_trigger.load())
                                     : SplitTriggerSource::AutomaticDuration;
        nvenc.RequestKeyframe();
        m_state.diagnostics.OnForcedKeyframe();
        m_state.diagnostics.SetSplitPending(true);
        logging::LogField fields[] = {{"segment_index", std::to_string(current_segment_index + 1u)},
                                      {"trigger", manual ? "manual" : "automatic"},
                                      {"session_pts_ms", std::to_string(pts_ns / 1000000ULL)}};
        logging::log(logging::LogLevel::Info, "video_thread", "split boundary armed (forced keyframe requested)",
                     std::span<const logging::LogField>(fields, std::size(fields)));
    };

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
    // Uses h264CodecPrivateReady, hevcCodecPrivateReady, and av1CodecPrivateReady by reference.
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
            } else if (m_state.config.video_codec == VideoCodec::HevcNvenc && !hevcCodecPrivateReady) {
                std::vector<uint8_t> vpsSpsPps;
                if (annexb::ExtractHevcVpsSpsPps(pkt.bytes.data(), pkt.bytes.size(), vpsSpsPps)) {
                    std::lock_guard lk(m_state.premux_mutex);
                    m_state.codec_private.hevc_vps_sps_pps = std::move(vpsSpsPps);
                    m_state.codec_private.hevc_ready = true;
                    hevcCodecPrivateReady = true;
                    m_state.premux_cv.notify_all();
                } else {
                    logging::log(logging::LogLevel::Warn, "video_thread",
                                 "HEVC VPS/SPS/PPS extraction failed on keyframe");
                }
            } else if (m_state.config.video_codec == VideoCodec::Av1Nvenc && !av1CodecPrivateReady) {
                char reason[256] = {};
                uint8_t cp[4] = {};
                if (codec_private::DeriveAv1CodecPrivate(pkt.bytes.data(), pkt.bytes.size(), cp, reason,
                                                         sizeof(reason))) {
                    std::lock_guard lk(m_state.premux_mutex);
                    std::memcpy(m_state.codec_private.av1_codec_private, cp, 4);
                    m_state.codec_private.av1_ready = true;
                    av1CodecPrivateReady = true;
                    m_state.premux_cv.notify_all();
                } else {
                    logging::LogField fields[] = {{"reason", reason}};
                    logging::log(logging::LogLevel::Warn, "video_thread",
                                 "AV1 codec private derivation failed on keyframe",
                                 std::span<const logging::LogField>(fields, std::size(fields)));
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
                // Segment boundary: if a split is armed and THIS packet is the
                // forced keyframe, emit a SplitSentinel into the mux queue
                // immediately before the keyframe so the mux thread finalizes the
                // current container and opens the next one, with this keyframe as
                // the new segment's first (self-contained) frame. The new segment
                // epoch is this packet's session PTS, so the auto interval resets
                // off the actual boundary (manual splits therefore push the next
                // auto split out by a full interval).
                std::lock_guard mlk(m_state.mux_mutex);
                if (split_armed && pkt.keyframe) {
                    ++current_segment_index;
                    segment_start_session_pts_ns = pkt.pts_ns;
                    if (split_auto_enabled) {
                        next_auto_threshold_ns = segment_start_session_pts_ns + split_auto_interval_ns;
                    }
                    MuxItem split_item;
                    split_item.payload = SplitSentinel{current_segment_index, split_armed_trigger};
                    m_state.mux_queue.push_back(std::move(split_item));
                    split_armed = false;
                }
                MuxItem mux_item;
                mux_item.payload = std::move(pkt);
                m_state.mux_queue.push_back(std::move(mux_item));
                m_state.mux_cv.notify_one();
            }
        }

        {
            std::lock_guard slk(m_state.stats_mutex);
            m_state.stats.video_frames_captured = videoFramesCaptured;
            m_state.stats.duplicated_video_frames = duplicatedFrames;
            m_state.stats.dropped_or_skipped_video_frames = droppedFrames + slotStallCount;
            m_state.stats.encoded_video_packets++;
            m_state.stats.video_bytes += pkt_bytes_count;
        }
        return true;
    };

    // --- Frame snapshot (CaptureFrame) ---
    // Lazily created staging texture (USAGE_STAGING + CPU_ACCESS_READ) for NV12→BGRA readback.
    // Lives until the encode loop exits; reused across multiple snapshot requests.
    winrt::com_ptr<ID3D11Texture2D> snapshotStagingTex;
    // Callback type alias — must precede the lambda that uses it.
    using SnapshotCallback = std::function<void(bool, uint32_t, uint32_t, std::vector<uint8_t>, std::string)>;

    // Perform a one-shot NV12→BGRA readback for the current slot if a snapshot is pending.
    // Called only on real frames (not duplicates) to ensure non-stale data.
    // NOTE: The Map(D3D11_MAP_READ) call below provides the minimal synchronization point;
    //       it stalls the thread until the GPU completes the CopyResource, typically <1 ms.
    auto performSnapshotIfRequested = [&](int32_t slot_idx) {
        if (!m_state.snapshot_requested.load())
            return;

        // 10-bit (P010) snapshot is not implemented: the readback below assumes an
        // 8-bit NV12 layout. Fail the request cleanly rather than produce garbage.
        // The encode path is unaffected; only CaptureFrame is unavailable for 10-bit.
        if (tenBit) {
            SnapshotCallback pending_cb;
            {
                std::lock_guard lk(m_state.snapshot_callback_mutex);
                pending_cb = std::move(m_state.snapshot_callback);
                m_state.snapshot_callback = nullptr;
                m_state.snapshot_requested.store(false);
            }
            if (pending_cb)
                pending_cb(false, 0, 0, {}, "CaptureFrame is not supported for 10-bit recording");
            return;
        }

        // Lazily allocate the staging texture on first use.
        if (!snapshotStagingTex) {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = encodeWidth;
            sd.Height = encodeHeight;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = DXGI_FORMAT_NV12;
            sd.SampleDesc = {1, 0};
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.BindFlags = 0;
            HRESULT shr = d3dDevice->CreateTexture2D(&sd, nullptr, snapshotStagingTex.put());
            if (FAILED(shr)) {
                char errbuf[80];
                snprintf(errbuf, sizeof(errbuf), "snapshot staging tex failed 0x%08lX",
                         static_cast<unsigned long>(shr));
                SnapshotCallback pending_cb;
                {
                    std::lock_guard lk(m_state.snapshot_callback_mutex);
                    pending_cb = std::move(m_state.snapshot_callback);
                    m_state.snapshot_callback = nullptr;
                    m_state.snapshot_requested.store(false);
                }
                if (pending_cb)
                    pending_cb(false, 0, 0, {}, errbuf);
                return;
            }
        }

        // Copy the final NV12 encode-ready frame to the staging texture.
        d3dContext->CopyResource(snapshotStagingTex.get(), nv12Textures[slot_idx].get());

        // Map for CPU read (synchronization point — stalls until GPU copy completes).
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT mhr = d3dContext->Map(snapshotStagingTex.get(), 0, D3D11_MAP_READ, 0, &mapped);

        SnapshotCallback pending_cb;
        {
            std::lock_guard lk(m_state.snapshot_callback_mutex);
            pending_cb = std::move(m_state.snapshot_callback);
            m_state.snapshot_callback = nullptr;
            m_state.snapshot_requested.store(false);
        }

        if (FAILED(mhr)) {
            char errbuf[80];
            snprintf(errbuf, sizeof(errbuf), "snapshot map failed 0x%08lX", static_cast<unsigned long>(mhr));
            if (pending_cb)
                pending_cb(false, 0, 0, {}, errbuf);
            return;
        }

        // NV12 layout:
        //   Plane Y:  rows 0 .. height-1, each row = RowPitch bytes, 1 byte per pixel
        //   Plane UV: rows height .. height+height/2-1, interleaved U V, same RowPitch
        const auto* y_plane = static_cast<const uint8_t*>(mapped.pData);
        const auto* uv_plane = y_plane + static_cast<size_t>(mapped.RowPitch) * encodeHeight;
        const uint32_t y_pitch = mapped.RowPitch;
        const uint32_t uv_pitch = mapped.RowPitch;

        std::vector<uint8_t> bgra;
        bgra.resize(static_cast<size_t>(encodeWidth) * encodeHeight * 4u);

        // BT.601 limited-range (studio swing) YUV → full-range RGB → BGRA
        for (uint32_t row = 0; row < encodeHeight; ++row) {
            const uint8_t* y_row = y_plane + static_cast<size_t>(row) * y_pitch;
            const uint8_t* uv_row = uv_plane + static_cast<size_t>(row / 2u) * uv_pitch;
            uint8_t* out_row = bgra.data() + static_cast<size_t>(row) * encodeWidth * 4u;
            for (uint32_t col = 0; col < encodeWidth; ++col) {
                const int y_val = static_cast<int>(y_row[col]) - 16;
                const int u_val = static_cast<int>(uv_row[col & ~1u]) - 128;
                const int v_val = static_cast<int>(uv_row[(col & ~1u) + 1u]) - 128;
                auto clamp255 = [](int v) -> uint8_t { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
                out_row[col * 4u + 0u] = clamp255((298 * y_val + 516 * u_val + 128) >> 8);               // B
                out_row[col * 4u + 1u] = clamp255((298 * y_val - 100 * u_val - 208 * v_val + 128) >> 8); // G
                out_row[col * 4u + 2u] = clamp255((298 * y_val + 409 * v_val + 128) >> 8);               // R
                out_row[col * 4u + 3u] = 255u;                                                           // A
            }
        }

        d3dContext->Unmap(snapshotStagingTex.get(), 0);

        if (pending_cb)
            pending_cb(true, encodeWidth, encodeHeight, std::move(bgra), {});
    };

    if (m_state.config.cfr) {
        // ====================================================================
        // CFR path: QPC-driven scheduler — duplicate/drop to hit constant rate
        // ====================================================================

        // Reference encode texture for frame duplication (NV12 8-bit / P010 10-bit)
        winrt::com_ptr<ID3D11Texture2D> refNv12;
        bool refNv12Valid = false;

        {
            D3D11_TEXTURE2D_DESC refDesc{};
            refDesc.Width = encodeWidth;
            refDesc.Height = encodeHeight;
            refDesc.MipLevels = 1;
            refDesc.ArraySize = 1;
            refDesc.Format = encodeFormat;
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
            if (!useOdCapture) {
                MSG msg{};
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }

            if (sourceLost) {
                std::lock_guard lk(m_state.stats_mutex);
                m_state.stats.source_loss = true;
                m_state.stop_requested.store(true);
                break;
            }

            if (useOdCapture) {
                // DXGI OD: drain all available frames, copy each to odCapturedTex, keep newest.
                while (true) {
                    ID3D11Texture2D* rawTex = nullptr;
                    DXGI_OUTDUPL_FRAME_INFO info{};
                    HRESULT odHr = S_OK;
                    if (!odSrc.TryAcquireFrame(0, &rawTex, &info, &odHr)) {
                        if (odHr == DXGI_ERROR_ACCESS_LOST)
                            sourceLost = true;
                        break;
                    }
                    d3dContext->CopyResource(odCapturedTex.get(), rawTex);
                    rawTex->Release();
                    if (info.PointerShapeBufferSize > 0 && m_state.config.capture_cursor) {
                        if (odSrc.GetFramePointerShape(&odCursorShapeInfo, odCursorBitmap))
                            odCursorShapeValid = true;
                    }
                    if (info.LastMouseUpdateTime.QuadPart != 0) {
                        odCursorVisible = info.PointerPosition.Visible != FALSE;
                        odCursorPosX = info.PointerPosition.Position.x;
                        odCursorPosY = info.PointerPosition.Position.y;
                    }
                    odSrc.ReleaseFrame();
                    // Only count capture/coalesce while actively recording — frames the
                    // backend produces during pause are intentionally discarded, not drops.
                    const bool diag_recording = !m_state.pause_requested.load();
                    if (diag_recording)
                        m_state.diagnostics.OnFrameCaptured();
                    if (odCapturedTexValid) {
                        ++droppedFrames;
                        if (diag_recording)
                            m_state.diagnostics.OnFrameDroppedCoalesced();
                    }
                    odCapturedTexValid = true;
                }
            } else {
                // WGC: drain frame pool — keep latest (always drain, even when paused)
                try {
                    while (true) {
                        auto frame = framePool.TryGetNextFrame();
                        if (frame == nullptr)
                            break;
                        auto surface = frame.Surface();
                        auto access =
                            surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                        winrt::com_ptr<ID3D11Texture2D> tex;
                        if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put())))) {
                            D3D11_TEXTURE2D_DESC frameDesc{};
                            tex->GetDesc(&frameDesc);
                            if (frameDesc.Width != sourceWidth || frameDesc.Height != sourceHeight) {
                                std::ostringstream err;
                                err << "capture source size changed during session from " << sourceWidth << "x"
                                    << sourceHeight << " to " << frameDesc.Width << "x" << frameDesc.Height
                                    << "; restart recording to reconfigure encoder";
                                m_state.RecordFailure(E_INVALIDARG, ErrorPhase::VideoCapture, err.str());
                                sourceLost = true;
                                break;
                            }
                            const bool diag_recording = !m_state.pause_requested.load();
                            if (diag_recording)
                                m_state.diagnostics.OnFrameCaptured();
                            if (pendingWgcTex != nullptr) {
                                ++droppedFrames;
                                if (diag_recording)
                                    m_state.diagnostics.OnFrameDroppedCoalesced();
                            }
                            pendingWgcTex = tex;
                        }
                    }
                } catch (...) {
                }
            }

            // Pause: discard frames and track paused duration for epoch adjustment on resume
            const bool cfr_paused = m_state.pause_requested.load();
            if (cfr_paused) {
                if (!cfr_was_paused) {
                    cfr_was_paused = true;
                    cfr_pause_start_100ns = Qpc100ns(qpcFreq);
                }
                pendingWgcTex = nullptr;
                odCapturedTexValid = false;
                Sleep(1);
                continue;
            }
            if (cfr_was_paused) {
                epochQpc100ns += Qpc100ns(qpcFreq) - cfr_pause_start_100ns;
                cfr_was_paused = false;
            }

            // Set epoch on first frame arrival (OD or WGC)
            const bool hasNewFrame = useOdCapture ? odCapturedTexValid : (pendingWgcTex != nullptr);
            if (!videoEpochSet && hasNewFrame) {
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
                    m_state.diagnostics.OnFrameDroppedBackpressure();
                    break; // retry this tick next outer iteration once a slot is free
                }

                bool frameWritten = false;

                // Determine source texture for this tick
                ID3D11Texture2D* rawSourceTex =
                    useOdCapture ? (odCapturedTexValid ? odCapturedTex.get() : nullptr) : pendingWgcTex.get();

                if (rawSourceTex != nullptr) {
                    const WebcamOverlayLive overlay = m_state.SnapshotWebcamOverlay();
                    const auto comp_t0 = std::chrono::steady_clock::now();
                    ID3D11Texture2D* vpInput = compositeFrameGpu(rawSourceTex, overlay);
                    const auto comp_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnCompositorSubmit(
                        comp_t1, std::chrono::duration<double, std::milli>(comp_t1 - comp_t0).count(),
                        needsGpuCompositor);
                    if (vpInput == nullptr) {
                        nvenc.ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    if (useOdCapture) {
                        odCapturedTexValid = false;
                    } else {
                        pendingWgcTex = nullptr;
                    }

                    // Convert BGRA frame to NV12 via VideoProcessorBlt
                    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc{};
                    ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                    ivDesc.Texture2D.MipSlice = 0;
                    ivDesc.Texture2D.ArraySlice = 0;

                    winrt::com_ptr<ID3D11VideoProcessorInputView> inputView;
                    hr = videoDevice->CreateVideoProcessorInputView(vpInput, videoEnum.get(), &ivDesc, inputView.put());

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
                            // Capture frame snapshot on real (non-duplicate) frames.
                            performSnapshotIfRequested(slot);
                            frameWritten = true;
                        }
                    }
                } else if (refNv12Valid) {
                    // Duplicate: copy reference NV12 into this slot
                    d3dContext->CopyResource(nv12Textures[slot].get(), refNv12.get());
                    frameWritten = true;
                    ++duplicatedFrames;
                }

                if (!frameWritten) {
                    // VideoProcessorBlt failed or no reference yet — release slot and skip tick
                    nvenc.ReleaseSlot(slot);
                    ++droppedFrames;
                    m_state.diagnostics.OnFrameDroppedCfr();
                    cfr_frame_idx++;
                    next_tick_100ns += frame_interval_100ns;
                    anyWork = true;
                    continue;
                }

                // Arm a split boundary (manual or automatic) for this submission
                // so its forced IDR opens the next segment. Done before encode.
                maybeArmSplit(pts_ns);

                EncodedVideoPacket pkt;
                std::string encErr;
                m_state.diagnostics.OnEncodeSubmitted();
                const auto enc_t0 = std::chrono::steady_clock::now();
                bool encOk = nvenc.EncodeFrame(slot, pts_ns, encodeWidth, encodeHeight, pkt, encErr);
                const auto enc_t1 = std::chrono::steady_clock::now();
                m_state.diagnostics.OnEncodeLatency(enc_t1,
                                                    std::chrono::duration<double, std::milli>(enc_t1 - enc_t0).count());

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
            if (!useOdCapture) {
                MSG msg{};
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }

            if (sourceLost) {
                std::lock_guard lk(m_state.stats_mutex);
                m_state.stats.source_loss = true;
                m_state.stop_requested.store(true);
                break;
            }

            bool anyWork = false;

            winrt::com_ptr<ID3D11Texture2D> latestTex;
            int64_t latestFrameTicks100ns = 0;

            if (useOdCapture) {
                // DXGI OD: drain available frames, copy to odCapturedTex, keep newest
                while (true) {
                    ID3D11Texture2D* rawTex = nullptr;
                    DXGI_OUTDUPL_FRAME_INFO info{};
                    HRESULT odHr = S_OK;
                    if (!odSrc.TryAcquireFrame(0, &rawTex, &info, &odHr)) {
                        if (odHr == DXGI_ERROR_ACCESS_LOST)
                            sourceLost = true;
                        break;
                    }
                    d3dContext->CopyResource(odCapturedTex.get(), rawTex);
                    rawTex->Release();
                    if (info.PointerShapeBufferSize > 0 && m_state.config.capture_cursor) {
                        if (odSrc.GetFramePointerShape(&odCursorShapeInfo, odCursorBitmap))
                            odCursorShapeValid = true;
                    }
                    if (info.LastMouseUpdateTime.QuadPart != 0) {
                        odCursorVisible = info.PointerPosition.Visible != FALSE;
                        odCursorPosX = info.PointerPosition.Position.x;
                        odCursorPosY = info.PointerPosition.Position.y;
                    }
                    // Convert DXGI LastPresentTime (QPC ticks) to 100ns units
                    if (info.LastPresentTime.QuadPart != 0) {
                        const auto lpt = static_cast<uint64_t>(info.LastPresentTime.QuadPart);
                        latestFrameTicks100ns =
                            static_cast<int64_t>(lpt / qpcFreq * 10000000ULL + lpt % qpcFreq * 10000000ULL / qpcFreq);
                    }
                    odSrc.ReleaseFrame();
                    // Only count capture/coalesce while actively recording — frames the
                    // backend produces during pause are intentionally discarded, not drops.
                    const bool diag_recording = !m_state.pause_requested.load();
                    if (diag_recording)
                        m_state.diagnostics.OnFrameCaptured();
                    if (odCapturedTexValid) {
                        ++droppedFrames;
                        if (diag_recording)
                            m_state.diagnostics.OnFrameDroppedCoalesced();
                    }
                    odCapturedTexValid = true;
                }
                if (odCapturedTexValid) {
                    latestTex = odCapturedTex; // borrow — not released in loop
                    if (latestFrameTicks100ns == 0)
                        latestFrameTicks100ns = static_cast<int64_t>(Qpc100ns(qpcFreq));
                }
            } else {
                // WGC: drain frame pool — keep latest (always drain, even when paused)
                try {
                    while (true) {
                        auto frame = framePool.TryGetNextFrame();
                        if (frame == nullptr)
                            break;

                        auto surface = frame.Surface();
                        auto access =
                            surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                        winrt::com_ptr<ID3D11Texture2D> tex;
                        if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put())))) {
                            D3D11_TEXTURE2D_DESC frameDesc{};
                            tex->GetDesc(&frameDesc);
                            if (frameDesc.Width != sourceWidth || frameDesc.Height != sourceHeight) {
                                std::ostringstream err;
                                err << "capture source size changed during session from " << sourceWidth << "x"
                                    << sourceHeight << " to " << frameDesc.Width << "x" << frameDesc.Height
                                    << "; restart recording to reconfigure encoder";
                                m_state.RecordFailure(E_INVALIDARG, ErrorPhase::VideoCapture, err.str());
                                sourceLost = true;
                                break;
                            }
                            const bool diag_recording = !m_state.pause_requested.load();
                            if (diag_recording)
                                m_state.diagnostics.OnFrameCaptured();
                            if (latestTex != nullptr) {
                                ++droppedFrames;
                                if (diag_recording)
                                    m_state.diagnostics.OnFrameDroppedCoalesced();
                            }
                            latestTex = tex;
                            latestFrameTicks100ns = frame.SystemRelativeTime().count();
                        }
                    }
                } catch (...) {
                }
            }

            // Pause: discard frames and track paused duration for epoch adjustment on resume
            const bool vfr_paused = m_state.pause_requested.load();
            if (vfr_paused) {
                if (!vfr_was_paused) {
                    vfr_was_paused = true;
                    vfr_pause_start_100ns = Qpc100ns(qpcFreq);
                }
                odCapturedTexValid = false;
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
                    m_state.video_epoch_qpc_100ns.store(Qpc100ns(qpcFreq));
                }

                int64_t deltaTicks = latestFrameTicks100ns - videoEpochTicks100ns;
                if (deltaTicks < 0)
                    deltaTicks = 0;
                uint64_t framePts_ns = static_cast<uint64_t>(deltaTicks) * 100ULL;
                if (videoFramesCaptured > 0 && framePts_ns <= lastVideoPts) {
                    framePts_ns = lastVideoPts + 1;
                }
                lastVideoPts = framePts_ns;

                // Acquire a free input slot
                int32_t slot = nvenc.AcquireFreeSlot();

                if (slot >= 0) {
                    const WebcamOverlayLive overlay = m_state.SnapshotWebcamOverlay();
                    const auto comp_t0 = std::chrono::steady_clock::now();
                    ID3D11Texture2D* vpInput = compositeFrameGpu(latestTex.get(), overlay);
                    const auto comp_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnCompositorSubmit(
                        comp_t1, std::chrono::duration<double, std::milli>(comp_t1 - comp_t0).count(),
                        needsGpuCompositor);
                    if (vpInput == nullptr) {
                        nvenc.ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    if (useOdCapture) {
                        odCapturedTexValid = false;
                    }

                    // BGRA -> NV12 via VideoProcessorBlt into the selected slot's view
                    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc{};
                    ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                    ivDesc.Texture2D.MipSlice = 0;
                    ivDesc.Texture2D.ArraySlice = 0;

                    winrt::com_ptr<ID3D11VideoProcessorInputView> inputView;
                    hr = videoDevice->CreateVideoProcessorInputView(vpInput, videoEnum.get(), &ivDesc, inputView.put());

                    if (SUCCEEDED(hr) && inputView != nullptr) {
                        D3D11_VIDEO_PROCESSOR_STREAM stream{};
                        stream.Enable = TRUE;
                        stream.pInputSurface = inputView.get();

                        hr = videoContext->VideoProcessorBlt(videoProcessor.get(), videoOutputViews[slot].get(), 0, 1,
                                                             &stream);

                        inputView = nullptr;
                        latestTex = nullptr;

                        if (SUCCEEDED(hr)) {
                            // Capture frame snapshot on real frames (VFR path).
                            performSnapshotIfRequested(slot);

                            // Arm a split boundary for this submission (see CFR path).
                            maybeArmSplit(framePts_ns);

                            EncodedVideoPacket pkt;
                            std::string encErr;
                            m_state.diagnostics.OnEncodeSubmitted();
                            const auto enc_t0 = std::chrono::steady_clock::now();
                            bool encOk = nvenc.EncodeFrame(slot, framePts_ns, encodeWidth, encodeHeight, pkt, encErr);
                            const auto enc_t1 = std::chrono::steady_clock::now();
                            m_state.diagnostics.OnEncodeLatency(
                                enc_t1, std::chrono::duration<double, std::milli>(enc_t1 - enc_t0).count());

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
                    m_state.diagnostics.OnFrameDroppedBackpressure();
                }

                anyWork = true;
            }

            if (!anyWork)
                Sleep(1);
        }
    }

end_encode_loop:
    // --- Stop capture ---
    if (useOdCapture) {
        odSrc.Close();
    } else {
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
                } else if (m_state.config.video_codec == VideoCodec::HevcNvenc && !hevcCodecPrivateReady) {
                    std::vector<uint8_t> vpsSpsPps;
                    if (annexb::ExtractHevcVpsSpsPps(pkt.bytes.data(), pkt.bytes.size(), vpsSpsPps)) {
                        std::lock_guard lk(m_state.premux_mutex);
                        m_state.codec_private.hevc_vps_sps_pps = std::move(vpsSpsPps);
                        m_state.codec_private.hevc_ready = true;
                        hevcCodecPrivateReady = true;
                        m_state.premux_cv.notify_all();
                    }
                } else if (m_state.config.video_codec == VideoCodec::Av1Nvenc && !av1CodecPrivateReady) {
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
    nvenc.Destroy();

    // --- Update final stats ---
    {
        std::lock_guard lk(m_state.stats_mutex);
        m_state.stats.video_frames_captured = videoFramesCaptured;
        m_state.stats.duplicated_video_frames = duplicatedFrames;
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
