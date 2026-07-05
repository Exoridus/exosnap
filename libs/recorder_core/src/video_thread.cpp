#include "video_thread.h"

#include "annexb_to_avcc.h"
#include "annexb_to_hvcc.h"
#include "codec_private.h"
#include "dxgi_od_capture_src.h"
#include "gpu_compositor.h"
#include "gpu_hdr_pq.h"
#include "gpu_hdr_tonemap.h"
#include "hdr_preview.h"
#include "hdr_tonemap.h"

#include "nvenc_video_encoder.h"
#include "preview_publish_gate.h"
#include "preview_staging_ring.h"
#include "session_internal.h"
#include "yuv_to_bgra.h"
#include <recorder_core/hdr_native.h>

#include <recorder_core/frame_pacing.h>
#include <recorder_core/logging/logging.h>
#include <recorder_core/packet_types.h>
#include <recorder_core/sdr_white_level.h>
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
#include <span>
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

// Stable name for the process's effective DPI-awareness context. Per-monitor
// awareness is what keeps capture geometry at native pixels; if it were lost
// (e.g. a manifest/regression dropping to System-aware or Unaware) the OS would
// hand back virtualized/legacy-scaled surfaces. Logged once at capture start so
// such a regression is a visible log fact rather than mysterious scaling.
const char* DpiAwarenessName() noexcept {
    const DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
    if (AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return "PerMonitorV2";
    if (AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
        return "PerMonitor";
    if (AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))
        return "System";
    if (AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED))
        return "UnawareGdiScaled";
    if (AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_UNAWARE))
        return "Unaware";
    return "Unknown";
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
        logging::LogField fields[] = {{"backend", backend},
                                      {"target_kind", TargetKindName(target.kind)},
                                      {"target_desc", target.description},
                                      {"dpi_awareness", DpiAwarenessName()}};
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

    // WGC (window) path: HDR facts of the window's hosting monitor, resolved once
    // at session start via MonitorFromWindow. A mid-session move to another monitor
    // keeps this initial decision (documented limitation). Unused on the OD path,
    // which reads its facts from odSrc.DisplayFacts() instead.
    HdrDisplayFacts wgcHdrFacts;
    // WGC frame-pool format + capture mode. Defaults keep the historic BGRA8 / SDR
    // window path; recomputed from wgcHdrFacts below when the target is a window.
    WgcCapturePlan wgcPlan;

    // scRGB->SDR tone-map knee, in reference-white multiples. Only an actively-
    // HDR display's reported peak is trusted; otherwise the documented fallback
    // is used (an SDR-mode display still reports inflated EDID luminance caps).
    float hdrPeakScale = HdrPeakScale(false, 0.0f);
    // Native HDR10 output is expected when HDR10 handling is selected, the
    // captured display is HDR-active, and the codec can encode HDR10. The
    // session colour metadata (BT.2020/PQ + mastering) and 10-bit pinning are
    // assembled by the caller before the session starts; this flag only drives
    // the capture-conversion path and is kept consistent by the same rule.
    bool expectNativeHdr = false;
    if (useOdCapture) {
        std::string odErr;
        if (!odSrc.Open(d3dDevice.get(), reinterpret_cast<HMONITOR>(target.native_id), odErr)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoCapture, "DXGI OD open: " + odErr);
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
        hdrPeakScale = HdrPeakScale(odSrc.HdrActive(), odSrc.MaxLuminanceNits());
        expectNativeHdr =
            IsHdr10NativeEffective(m_state.config.hdr_mode, odSrc.HdrActive(), m_state.config.video_codec);
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

        // Resolve the hosting monitor's HDR facts once, then decide the frame-pool
        // format + capture mode the same way the OD path does for a monitor. On an
        // HDR desktop this negotiates an FP16 pool so the real scRGB signal reaches
        // the shared tone-map / native-HDR10 machinery instead of DWM's SDR
        // tone-map; on an SDR desktop the plan stays BGRA8 / SDR (unchanged).
        const HMONITOR wgcMonitor =
            MonitorFromWindow(reinterpret_cast<HWND>(target.native_id), MONITOR_DEFAULTTONEAREST);
        QueryDisplayHdrFacts(wgcMonitor, wgcHdrFacts);
        wgcPlan = ResolveWgcCapturePlan(wgcHdrFacts.hdr_active, m_state.config.hdr_mode,
                                        CodecSupportsHdr10Native(m_state.config.video_codec));
        hdrPeakScale = HdrPeakScale(wgcHdrFacts.hdr_active, wgcHdrFacts.max_luminance_nits);
        expectNativeHdr =
            IsHdr10NativeEffective(m_state.config.hdr_mode, wgcHdrFacts.hdr_active, m_state.config.video_codec);
    }

    // Capture dimensions
    const int32_t sourceWidthSigned =
        useOdCapture ? static_cast<int32_t>(odSrc.Width()) : static_cast<int32_t>(item.Size().Width);
    const int32_t sourceHeightSigned =
        useOdCapture ? static_cast<int32_t>(odSrc.Height()) : static_cast<int32_t>(item.Size().Height);

    // OD: the desktop's declared mode format (frames may still arrive in a
    // different format — negotiated from the first acquired frame, see below).
    // WGC: the frame pool is explicitly created as BGRA8.
    char formatNameBuf[32];
    const char* preInitFormatName = useOdCapture
                                        ? OdCaptureFormatName(odSrc.Format(), formatNameBuf, sizeof(formatNameBuf))
                                        : "B8G8R8A8_UNORM (8-bit SDR)";

    std::ostringstream diag;
    diag << "target.kind=" << TargetKindName(target.kind) << ", target.description=\"" << target.description
         << "\", target.native_id=0x" << std::hex << target.native_id << std::dec
         << ", capture.visibleContentSize=" << sourceWidthSigned << "x" << sourceHeightSigned
         << ", capture.modeDescFormat=" << preInitFormatName << ", videoCodec="
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
        nvenc.SetPreset(m_state.config.nvenc_preset);
        // Color signaling (fix for color-range-signaling bug): the encoded
        // bitstream itself must carry the same color description as the
        // VideoProcessor conversion below and the Matroska Colour element
        // (mux_thread.cpp), otherwise players/ffprobe that read color info from
        // the bitstream (all of them for AV1 — verified; container tags are
        // ignored) see an untagged/wrong-range stream regardless of correct
        // container tagging.
        nvenc.SetColor(m_state.config.color);

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
    // VideoProcessor converts RGB → P010 instead of NV12, and the encode ring +
    // reference texture use DXGI_FORMAT_P010. The output color space stays studio
    // BT.709 (no HDR/BT.2020 here — that is a later slice).
    const bool tenBit = (m_state.config.bit_depth == BitDepth::Bit10);
    const DXGI_FORMAT encodeFormat = tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

    // HDR->SDR tone-map intermediate depth follows the encode target: a 10-bit
    // encode (P010) tone-maps into R10G10B10A2 so the extra depth survives the
    // RGB->P010 conversion; 8-bit encodes keep BGRA8, byte-identical to before.
    // This is the pure choice; it may fall back to BGRA8 at runtime if the device
    // rejects R10G10B10A2 as a VideoProcessor input (OD format negotiation below)
    // or as a render target (tone-map texture creation below).
    DXGI_FORMAT toneMapIntermediateFormat = ToneMapIntermediateFormat(tenBit);

    // Native HDR10 (PQ/BT.2020) requires a 10-bit P010 encode target. The caller
    // assembles BT.2020/PQ colour metadata and pins 10-bit together (see the HDR10
    // metadata assembly). If a future caller engages the native HDR path without
    // pinning 10-bit, the PQ converter would bind an 8-bit NV12 render target and
    // fail cryptically at RTV creation — catch the inconsistency up front instead.
    if (NativeHdr10BitDepthViolation(expectNativeHdr, m_state.config.bit_depth)) {
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::Prepare,
                              "native HDR10 output requires 10-bit (P010) but the session bit depth is 8-bit; "
                              "pin 10-bit bit depth for HDR10 capture");
        if (com_inited)
            CoUninitialize();
        return;
    }

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
    // VideoProcessorBlt handles RGB->NV12/P010 conversion and GPU scaling.
    {
        D3D11_VIDEO_COLOR background{};
        background.RGBA.A = 1.0f;
        videoContext->VideoProcessorSetOutputBackgroundColor(videoProcessor.get(), FALSE, &background);

        // Make the RGB->NV12/P010 conversion deterministic (ADR 0032). Without an
        // explicit color space the driver picks an implementation-defined
        // matrix/range (BT.601 vs BT.709, full vs studio), so the same desktop
        // could encode to subtly different colors on different GPUs and the
        // container carried no color tags at all. Pin the input to full-range
        // RGB (the desktop composite) and the YUV output to BT.709 with the
        // user-selected quantization range — Limited (studio 16-235, broadcast
        // standard, the default as of fix/color-range-signaling — common
        // consumer players ignore the range flag and always expand limited to
        // full, so Full-range recordings look permanently crushed there) or
        // Full (0-255, native screen precision, opt-in). The output range here
        // MUST match the range the container/bitstream is tagged with (see
        // color_metadata.h / RecorderConfig::color), so the same config value
        // drives both; otherwise there is a black-level mismatch. `fullRange`
        // is read from the live config, not hardcoded, so it always follows
        // the current default/selection automatically.
        const bool fullRange = m_state.config.color.range != ColorRange::Limited;
        if (videoContext1) {
            // Preferred path: explicit DXGI colour spaces. Input is the desktop's
            // full-range BT.709 RGB; output is BT.709 YUV in the selected range.
            // Drivers honour the quantization range here, so the encoded NV12/P010
            // genuinely carries the chosen range and matches the container tag
            // (ADR 0032) — no full-vs-limited mismatch, no crushed/washed levels.
            videoContext1->VideoProcessorSetStreamColorSpace1(videoProcessor.get(), 0,
                                                              DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            videoContext1->VideoProcessorSetOutputColorSpace1(videoProcessor.get(),
                                                              fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                                                                        : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        } else {
            // Legacy fallback (pre-Win10 / no ID3D11VideoContext1). The output
            // Nominal_Range is widely ignored here, so the result may not honour
            // the requested range — best effort only.
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace{};
            inputColorSpace.Usage = 0;     // 0 = playback (full-precision conversion)
            inputColorSpace.RGB_Range = 0; // 0 = full-range RGB (0-255)
            inputColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
            videoContext->VideoProcessorSetStreamColorSpace(videoProcessor.get(), 0, &inputColorSpace);

            D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace{};
            outputColorSpace.Usage = 0;
            outputColorSpace.YCbCr_Matrix = 1; // 1 = BT.709
            outputColorSpace.Nominal_Range =
                fullRange ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
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
    // odCapturedTex: persistent texture we CopyResource into after each DXGI OD acquire.
    // DXGI OD textures are owned by the duplication interface and must be released before the
    // next AcquireNextFrame, so we always copy to this texture first.
    //
    // Format negotiation: the desktop framebuffer is BGRA8 on an 8-bit SDR
    // desktop but R10G10B10A2 on a 10 bpc SDR desktop (e.g. NVIDIA "Output
    // color depth: 10 bpc"). DXGI_OUTDUPL_DESC.ModeDesc.Format is NOT reliable
    // for this decision (measured: FP16 ModeDesc with BGRA8 frames on an
    // Advanced-Color desktop), so odCapturedTex is created lazily from the
    // FIRST acquired frame's actual texture desc. CopyResource silently does
    // nothing on format mismatch, which previously starved the encoder and
    // surfaced minutes later as an opaque mux timeout — an unsupported format
    // is now an explicit ErrorPhase::VideoCapture failure instead.
    winrt::com_ptr<ID3D11Texture2D> odCapturedTex;
    DXGI_FORMAT odFrameFormat = DXGI_FORMAT_UNKNOWN; // set when odCapturedTex is created
    bool odCapturedTexValid = false;

    // HDR (scRGB FP16) capture: odCapturedTex/ring textures hold the raw FP16
    // frames, and a per-emitted-frame tone-map pass converts them into hdrSdrTex
    // (an SDR BGRA8 surface) that then follows the normal SDR VideoProcessor
    // route. hdrToneMapActive is decided during first-frame negotiation.
    bool hdrToneMapActive = false;
    HdrToneMapper hdrToneMapper;
    winrt::com_ptr<ID3D11Texture2D> hdrSdrTex;

    // Native HDR10 (PQ/BT.2020 -> P010) capture: the captured HDR surface is
    // converted straight into the P010 encode slot, bypassing the SDR compositor
    // and the VideoProcessor (which cannot convert HDR colour spaces). Decided at
    // first-frame negotiation.
    bool hdrNativeActive = false;
    bool hdrPqInputIsPq = false; // true when the source is an already-PQ HDR10 R10G10B10A2 desktop
    DXGI_FORMAT hdrPqSrcFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    HdrPqConverter hdrPqConverter;

    // WGC (window) path decides its HDR handling up front from wgcPlan (the frame
    // pool format is *requested*, not negotiated from frames). A WGC FP16 pool is
    // always scRGB linear — never an already-PQ desktop — so hdrPqInputIsPq stays
    // false and hdrPqSrcFormat stays FP16 (the defaults above). The OD path leaves
    // these untouched here and sets them during first-frame negotiation instead.
    if (!useOdCapture) {
        hdrToneMapActive = (wgcPlan.mode == OdCaptureMode::HdrToneMap);
        hdrNativeActive = (wgcPlan.mode == OdCaptureMode::HdrNative);
    }

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

    // Validate an acquired OD frame; on the FIRST frame, negotiate the session
    // capture format and create odCapturedTex to match it. Cheap on the
    // steady-state path (one CPU-side GetDesc + compare per acquire).
    //
    //   Ok    — frame matches the negotiated format; safe to CopyResource.
    //   Skip  — foreign-format frame interleaved into the stream (measured on
    //           a 10 bpc SDR desktop: the legacy duplication delivers a BGRA8
    //           stream with occasional FP16 frames). The caller releases the
    //           frame and continues; the negotiated stream keeps feeding the
    //           encoder and CFR duplication bridges the gap.
    //   Fatal — first frame in an unsupported format, or a size change. An
    //           explicit ErrorPhase::VideoCapture failure has been recorded
    //           (previously this starved the encoder silently and surfaced as
    //           an opaque "codec private data" mux error at stop).
    enum class OdFrameCheck { Ok, Skip, Fatal };
    bool odForeignFrameLogged = false;
    // On an HDR-active display, OD hands out BGRA8 SDR compatibility ("seed")
    // frames interleaved with the real scRGB FP16 desktop frames — reliably so
    // right after DuplicateOutput1. When HDR handling is on, the session must
    // negotiate its format from a real HDR frame, not lock onto an SDR seed, or
    // an HDR desktop would silently record as DWM-tone-mapped SDR (and native
    // HDR10 could never engage). Skip a bounded number of leading SDR frames
    // while waiting for the HDR format; if none arrives, fall through to normal
    // negotiation (SDR for tone-map; the native guard then reports honestly).
    int odHdrSeedSkips = 0;
    bool odHdrSeedLogged = false;
    constexpr int kMaxHdrSeedSkips = 240;
    auto checkOdFrame = [&](ID3D11Texture2D* rawTex) -> OdFrameCheck {
        D3D11_TEXTURE2D_DESC rawDesc{};
        rawTex->GetDesc(&rawDesc);

        if (odCapturedTex == nullptr && odSrc.HdrActive() && m_state.config.hdr_mode != HdrMode::Off &&
            rawDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM && odHdrSeedSkips < kMaxHdrSeedSkips) {
            ++odHdrSeedSkips;
            if (!odHdrSeedLogged) {
                odHdrSeedLogged = true;
                logging::log(logging::LogLevel::Info, "video_thread",
                             "HDR display delivered an SDR seed frame; waiting for the scRGB HDR format", {});
            }
            return OdFrameCheck::Skip;
        }

        if (odCapturedTex != nullptr) {
            if (rawDesc.Format == odFrameFormat && rawDesc.Width == sourceWidth && rawDesc.Height == sourceHeight) {
                return OdFrameCheck::Ok;
            }
            if (rawDesc.Width != sourceWidth || rawDesc.Height != sourceHeight) {
                std::ostringstream err;
                err << "DXGI OD: capture frame size changed during session to " << rawDesc.Width << "x"
                    << rawDesc.Height << " (expected " << sourceWidth << "x" << sourceHeight
                    << "); restart recording to reconfigure";
                m_state.RecordFailure(E_INVALIDARG, ErrorPhase::VideoCapture, err.str());
                return OdFrameCheck::Fatal;
            }
            if (!odForeignFrameLogged) {
                odForeignFrameLogged = true;
                char fmtBuf[32];
                char negBuf[32];
                const logging::LogField fields[] = {
                    {"frameFormat", OdCaptureFormatName(rawDesc.Format, fmtBuf, sizeof(fmtBuf))},
                    {"negotiatedFormat", OdCaptureFormatName(odFrameFormat, negBuf, sizeof(negBuf))}};
                logging::log(logging::LogLevel::Warn, "video_thread",
                             "DXGI OD delivered a frame in a foreign format; skipping such frames this session",
                             std::span<const logging::LogField>(fields, std::size(fields)));
            }
            // Frames discarded during pause are intentional, not drops (same
            // convention as the drain loops' diag_recording gating).
            if (!m_state.pause_requested.load()) {
                m_state.diagnostics.OnFrameDroppedCoalesced();
            }
            return OdFrameCheck::Skip;
        }

        OdCaptureMode capMode = OdCaptureMode::Sdr;
        const bool hdr10OutputSupported = CodecSupportsHdr10Native(m_state.config.video_codec);
        if (!ResolveOdCaptureMode(rawDesc.Format, m_state.config.hdr_mode, odSrc.HdrActive(), hdr10OutputSupported,
                                  capMode)) {
            char fmtBuf[32];
            std::ostringstream err;
            err << "DXGI OD: unsupported desktop capture format "
                << OdCaptureFormatName(rawDesc.Format, fmtBuf, sizeof(fmtBuf));
            if (rawDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                // FP16 arrives only on an HDR desktop; the sole way to reach here
                // is HdrMode::Off, where HDR handling is disabled by request.
                err << "; the capture display is in HDR mode but HDR handling is off. "
                    << "Enable HDR tone-mapping, or switch the display to SDR. preInit={" << diag.str() << "}";
            } else {
                err << "; supported: B8G8R8A8_UNORM (8-bit SDR), R10G10B10A2_UNORM (10 bpc SDR), "
                    << "R16G16B16A16_FLOAT (HDR, tone-mapped or native HDR10). preInit={" << diag.str() << "}";
            }
            m_state.RecordFailure(static_cast<int32_t>(DXGI_ERROR_UNSUPPORTED), ErrorPhase::VideoCapture, err.str());
            return OdFrameCheck::Fatal;
        }
        const bool toneMap = (capMode == OdCaptureMode::HdrToneMap);
        const bool nativeHdr = (capMode == OdCaptureMode::HdrNative);
        // The caller committed BT.2020/PQ colour metadata + 10-bit for a native
        // session; if the display instead delivered a surface that resolves to
        // something else (e.g. an SDR compatibility surface because Advanced-Color
        // duplication was unavailable), the tags would not match the pixels. Fail
        // explicitly rather than encode a mislabelled stream.
        if (expectNativeHdr && !nativeHdr) {
            char fmtBuf[32];
            std::ostringstream err;
            err << "DXGI OD: native HDR10 output was configured but the display delivered a "
                << OdCaptureFormatName(rawDesc.Format, fmtBuf, sizeof(fmtBuf))
                << " surface that cannot carry it (Advanced-Color duplication unavailable). "
                << "preInit={" << diag.str() << "}";
            m_state.RecordFailure(static_cast<int32_t>(DXGI_ERROR_UNSUPPORTED), ErrorPhase::VideoCapture, err.str());
            return OdFrameCheck::Fatal;
        }
        // scRGB FP16 tone-map and native HDR10 both sample the capture texture as
        // a shader resource; only the plain SDR path feeds the VideoProcessor
        // directly from a render-target texture.
        const bool needsSrvSource = toneMap || nativeHdr;
        if (rawDesc.Width != sourceWidth || rawDesc.Height != sourceHeight) {
            std::ostringstream err;
            err << "DXGI OD: capture frame size " << rawDesc.Width << "x" << rawDesc.Height
                << " does not match duplicated output " << sourceWidth << "x" << sourceHeight << "; preInit={"
                << diag.str() << "}";
            m_state.RecordFailure(E_INVALIDARG, ErrorPhase::VideoCapture, err.str());
            return OdFrameCheck::Fatal;
        }

        // The format fed to the VideoProcessor must be a supported VP INPUT on
        // this driver, or every CreateVideoProcessorInputView would fail per tick
        // without a recorded failure — silent starvation one stage later. For a
        // tone-mapped HDR desktop the VP input is the tone-map output (BGRA8, or
        // R10G10B10A2 for a 10-bit encode), not the FP16 capture format (the D3D11
        // VP cannot convert scRGB HDR at all). The native HDR10 path never touches
        // the VP, so it skips this check.
        if (!nativeHdr) {
            auto vpInputSupported = [&](DXGI_FORMAT fmt, HRESULT& hr, UINT& support) {
                support = 0;
                hr = videoEnum->CheckVideoProcessorFormat(fmt, &support);
                return SUCCEEDED(hr) && (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) != 0;
            };
            DXGI_FORMAT vpInputFormat = toneMap ? toneMapIntermediateFormat : rawDesc.Format;
            HRESULT supHr = S_OK;
            UINT formatSupport = 0;
            // Graceful fallback: a driver that rejects the 10-bit R10G10B10A2
            // tone-map intermediate as a VP input keeps recording at BGRA8 (the
            // historic default, universally supported) rather than failing.
            if (toneMap && vpInputFormat == DXGI_FORMAT_R10G10B10A2_UNORM &&
                !vpInputSupported(vpInputFormat, supHr, formatSupport)) {
                logging::log(logging::LogLevel::Warn, "video_thread",
                             "10-bit tone-map intermediate (R10G10B10A2) is not a supported VideoProcessor input on "
                             "this driver; falling back to BGRA8 (8-bit tone-map precision)",
                             {});
                toneMapIntermediateFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
                vpInputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            }
            if (!vpInputSupported(vpInputFormat, supHr, formatSupport)) {
                char fmtBuf[32];
                std::ostringstream err;
                err << "DXGI OD: capture format " << OdCaptureFormatName(vpInputFormat, fmtBuf, sizeof(fmtBuf))
                    << " is not a supported VideoProcessor input on this driver (CheckVideoProcessorFormat hr=0x"
                    << std::hex << static_cast<unsigned long>(supHr) << ", support=0x" << formatSupport << std::dec
                    << "); preInit={" << diag.str() << "}";
                m_state.RecordFailure(static_cast<int32_t>(DXGI_ERROR_UNSUPPORTED), ErrorPhase::VideoCapture,
                                      err.str());
                return OdFrameCheck::Fatal;
            }
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = sourceWidth;
        desc.Height = sourceHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = rawDesc.Format; // negotiated from the actual frame, not ModeDesc
        desc.SampleDesc = {1, 0};
        desc.Usage = D3D11_USAGE_DEFAULT;
        // SDR: the VP reads odCapturedTex directly (kept as a render target as
        // before). Tone-map and native HDR10: the shader pass samples
        // odCapturedTex/ring as a shader resource, so bind for SRV instead.
        desc.BindFlags = needsSrvSource ? D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_RENDER_TARGET;

        HRESULT odHr = d3dDevice->CreateTexture2D(&desc, nullptr, odCapturedTex.put());
        if (FAILED(odHr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "CreateTexture2D(odCapturedTex) failed 0x%08lX",
                     static_cast<unsigned long>(odHr));
            m_state.RecordFailure(odHr, ErrorPhase::Prepare, buf);
            return OdFrameCheck::Fatal;
        }
        odFrameFormat = rawDesc.Format;
        hdrToneMapActive = toneMap;
        hdrNativeActive = nativeHdr;
        hdrPqInputIsPq = (rawDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
        hdrPqSrcFormat = rawDesc.Format;

        char fmtBuf[32];
        char modeBuf[32];
        const logging::LogField fields[] = {
            {"frameFormat", OdCaptureFormatName(odFrameFormat, fmtBuf, sizeof(fmtBuf))},
            {"modeDescFormat", OdCaptureFormatName(odSrc.Format(), modeBuf, sizeof(modeBuf))},
            {"handling",
             nativeHdr ? "native HDR10 PQ/BT.2020 -> P010" : (toneMap ? "HDR scRGB -> SDR BT.709 tone-map" : "SDR")}};
        logging::log(logging::LogLevel::Info, "video_thread", "DXGI OD capture format negotiated",
                     std::span<const logging::LogField>(fields, std::size(fields)));
        return OdFrameCheck::Ok;
    };

    // --- GPU compositing resources ---
    // OD and WGC compositing operate in source coordinates. VideoProcessorBlt then
    // applies crop, contain-fit scaling, letterbox background, and RGB->NV12/P010.
    // For OD the compositor's render target must match the negotiated capture
    // format, so Init is deferred until after the first frame (see below).
    const bool webcamProviderAvailable = (m_state.config.webcam.frame_provider != nullptr);
    const bool needsGpuCompositor = webcamProviderAvailable || m_state.config.capture_cursor;
    const uint32_t compositorWidth = sourceWidth;
    const uint32_t compositorHeight = sourceHeight;

    GpuCompositor gpuCompositor;
    bool gpuCompositorReady = false;

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
        if (!gpuCompositor.DrawWebcam(camBgra.data(), camW, camH, rect, overlay.mirror, chroma, compErr,
                                      overlay.opacity)) {
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

    // scRGB FP16 -> SDR BT.709 for an HDR desktop. Runs once per emitted frame,
    // replacing the FP16 capture texture with an SDR BGRA8 surface before
    // compositing/VideoProcessor. A no-op (returns source unchanged) on SDR
    // desktops. Returns nullptr and records the failure if the pass fails.
    auto toneMapIfHdr = [&](ID3D11Texture2D* source) -> ID3D11Texture2D* {
        if (!hdrToneMapActive || source == nullptr) {
            return source;
        }
        std::string tmErr;
        if (!hdrToneMapper.Convert(source, hdrSdrTex.get(), tmErr)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoCapture, "HDR tone-map: " + tmErr);
            return nullptr;
        }
        return hdrSdrTex.get();
    };

    // Native HDR10: convert the captured HDR surface straight into the P010
    // encode slot texture (colour + crop/scale/letterbox), replacing the
    // tone-map + compositor + VideoProcessor route. Records the failure and
    // returns false on error.
    auto encodeNativeHdrSlot = [&](ID3D11Texture2D* source, int32_t slot) -> bool {
        std::string pqErr;
        if (!hdrPqConverter.Convert(source, nv12Textures[slot].get(), pqErr)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoEncode, "HDR10 native convert: " + pqErr);
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
            // BGRA8 for the SDR window path (unchanged); scRGB FP16 when the hosting
            // display is HDR-active and HDR handling is on (wgcPlan). FP16 delivers
            // the real HDR signal instead of DWM's silent SDR tone-map.
            const auto wgcPoolFormat =
                (wgcPlan.frame_pool_format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                    ? winrt::Windows::Graphics::DirectX::DirectXPixelFormat::R16G16B16A16Float
                    : winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;
            framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(d3dWinRTDev,
                                                                                              wgcPoolFormat, 3, capSz);

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

    // First WGC frame captured by the wait loop below. WGC (like OD) only
    // delivers further frames when the source repaints, so the first frame
    // must be kept and seeded into the encode loop — discarding it starved a
    // fully static window into the opaque "codec private data" mux error
    // (same failure family as the OD phase-correct seed, measured live).
    winrt::com_ptr<ID3D11Texture2D> seedWgcTex;

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
                    // Negotiate the capture format from the first real frame
                    // (creates odCapturedTex). Unsupported format => explicit
                    // failure here instead of a silent mux timeout later.
                    const OdFrameCheck check = checkOdFrame(rawTex);
                    if (check == OdFrameCheck::Fatal) {
                        rawTex->Release();
                        odSrc.ReleaseFrame();
                        if (com_inited)
                            CoUninitialize();
                        return;
                    }
                    if (check == OdFrameCheck::Skip) {
                        rawTex->Release();
                        odSrc.ReleaseFrame();
                        continue;
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
                        // Keep the texture as the encode-loop seed (see
                        // seedWgcTex above). Same borrow pattern as the drain
                        // loops: the texture outlives the frame object. Only a
                        // frame matching the configured capture size may seed
                        // the encoder (mirrors the drain loop's size check);
                        // on mismatch the drain loop reports the honest
                        // size-changed failure on the next frame.
                        auto surface = frame.Surface();
                        auto access =
                            surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                        winrt::com_ptr<ID3D11Texture2D> tex;
                        if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put())))) {
                            D3D11_TEXTURE2D_DESC frameDesc{};
                            tex->GetDesc(&frameDesc);
                            if (frameDesc.Width == sourceWidth && frameDesc.Height == sourceHeight) {
                                seedWgcTex = tex;
                            }
                        }
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

    // --- HDR tone-map init (deferred until the capture format is negotiated) ---
    // On an HDR desktop the negotiated frames are scRGB FP16; convert each
    // emitted frame to an SDR surface (hdrSdrTex) that the compositor and
    // VideoProcessor then treat as an ordinary SDR desktop. The surface is
    // R10G10B10A2 for a 10-bit encode (preserving tone-map precision into P010)
    // or BGRA8 for an 8-bit encode / after a device-capability fallback.
    if (hdrToneMapActive) {
        D3D11_TEXTURE2D_DESC sdrDesc{};
        sdrDesc.Width = sourceWidth;
        sdrDesc.Height = sourceHeight;
        sdrDesc.MipLevels = 1;
        sdrDesc.ArraySize = 1;
        sdrDesc.Format = toneMapIntermediateFormat;
        sdrDesc.SampleDesc = {1, 0};
        sdrDesc.Usage = D3D11_USAGE_DEFAULT;
        sdrDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT sdrHr = d3dDevice->CreateTexture2D(&sdrDesc, nullptr, hdrSdrTex.put());
        // Graceful fallback: a device that rejects R10G10B10A2 as a render target
        // reverts to BGRA8 rather than failing the recording. BGRA8 VP-input support
        // is assumed here (universal in practice), not re-checked: negotiation only
        // probed BGRA8 when R10G10B10A2 was rejected as VP input.
        if (FAILED(sdrHr) && toneMapIntermediateFormat == DXGI_FORMAT_R10G10B10A2_UNORM) {
            logging::log(logging::LogLevel::Warn, "video_thread",
                         "10-bit tone-map intermediate (R10G10B10A2) could not be created as a render target on this "
                         "device; falling back to BGRA8 (8-bit tone-map precision)",
                         {});
            toneMapIntermediateFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            sdrDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            hdrSdrTex = nullptr;
            sdrHr = d3dDevice->CreateTexture2D(&sdrDesc, nullptr, hdrSdrTex.put());
        }
        std::string tmErr;
        if (FAILED(sdrHr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "CreateTexture2D(hdrSdrTex) failed 0x%08lX", static_cast<unsigned long>(sdrHr));
            m_state.RecordFailure(sdrHr, ErrorPhase::Prepare, buf);
            if (com_inited)
                CoUninitialize();
            return;
        }
        if (!hdrToneMapper.Init(d3dDevice.get(), d3dContext.get(), sourceWidth, sourceHeight, hdrPeakScale, tmErr)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::Prepare, "HDR tone-map init: " + tmErr);
            if (com_inited)
                CoUninitialize();
            return;
        }
    }

    // --- Native HDR10 converter init (deferred until the capture format is
    // negotiated) --- The captured HDR surface is converted directly to a
    // PQ/BT.2020 P010 encode frame (colour + crop / contain-fit scale /
    // letterbox), bypassing the SDR compositor and VideoProcessor.
    if (hdrNativeActive) {
        HdrPqConverter::Geometry geom;
        geom.src_crop_x = hasCrop ? static_cast<uint32_t>(cropX) : 0u;
        geom.src_crop_y = hasCrop ? static_cast<uint32_t>(cropY) : 0u;
        geom.src_crop_w = sourceContentWidth;
        geom.src_crop_h = sourceContentHeight;
        geom.src_width = sourceWidth;
        geom.src_height = sourceHeight;
        geom.content_x = static_cast<uint32_t>(contentRect.x);
        geom.content_y = static_cast<uint32_t>(contentRect.y);
        geom.content_w = static_cast<uint32_t>(contentRect.width);
        geom.content_h = static_cast<uint32_t>(contentRect.height);
        geom.encode_width = encodeWidth;
        geom.encode_height = encodeHeight;
        std::string pqErr;
        if (!hdrPqConverter.Init(d3dDevice.get(), d3dContext.get(), geom, hdrPqInputIsPq, hdrPqSrcFormat, pqErr)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::Prepare, "HDR10 native converter init: " + pqErr);
            if (com_inited)
                CoUninitialize();
            return;
        }
        // Webcam PiP + cursor are composited in linear scRGB FP16 before the PQ
        // conversion (see the compositor init below), so soft chroma-keyed edges
        // blend correctly. The already-PQ R10G10B10A2 desktop variant is the
        // exception — its surface is non-linear, so linear compositing does not
        // apply and overlays are omitted for that (rare) sub-path.
        if (hdrPqInputIsPq && needsGpuCompositor) {
            logging::log(logging::LogLevel::Warn, "video_thread",
                         "native HDR10 from an already-PQ 10-bit desktop: webcam PiP and cursor overlay are omitted "
                         "(the surface is non-linear, so linear-light compositing does not apply)",
                         {});
            // Also surface this calmly through live diagnostics (not a fault).
            std::lock_guard lk(m_state.stats_mutex);
            m_state.stats.webcam_overlay_omitted = true;
        }
    }

    // --- GPU compositor init (deferred until the capture format is known) ---
    // WGC frames use the requested frame-pool format (BGRA8 on SDR, scRGB FP16 when
    // wgcPlan chose HDR); OD frames use the format negotiated from the first
    // acquired frame above. If stop was requested before the first OD frame arrived
    // the format is unknown — skip init; the encode loop below will not run.
    // The already-PQ R10G10B10A2 native sub-path composites nothing (non-linear
    // surface); every other path gets a compositor matched to its working format.
    const bool nativeOverlaysUnsupported = hdrNativeActive && hdrPqInputIsPq;
    if (needsGpuCompositor && !nativeOverlaysUnsupported && (!useOdCapture || odCapturedTex != nullptr)) {
        // Native HDR10 (FP16): overlays are composited in linear scRGB FP16 before
        // the PQ conversion. Tone-mapped HDR: composited on the SDR tone-map
        // surface (R10G10B10A2 for a 10-bit encode, else BGRA8). The compositor
        // background CopyResource requires this to match hdrSdrTex exactly.
        // Plain SDR: the negotiated capture format.
        const DXGI_FORMAT compositorFormat =
            hdrNativeActive ? DXGI_FORMAT_R16G16B16A16_FLOAT
                            : (hdrToneMapActive ? toneMapIntermediateFormat
                                                : (useOdCapture ? odFrameFormat : DXGI_FORMAT_B8G8R8A8_UNORM));
        // Only the native-HDR FP16 path uses the SDR white level (linear-light
        // overlay compositing); every other path keeps the 203-nit default,
        // which Init ignores for non-FP16 render formats anyway. The facts come
        // from the capture path's own source: odSrc for a monitor, the window's
        // hosting-monitor facts for WGC.
        const float overlayRefWhiteNits =
            hdrNativeActive ? EffectiveOverlayReferenceWhiteNits(
                                  (useOdCapture ? odSrc.DisplayFacts() : wgcHdrFacts).sdr_white_level_nits)
                            : kDefaultSdrWhiteLevelNits;
        std::string compErr;
        if (!gpuCompositor.Init(d3dDevice.get(), d3dContext.get(), compositorWidth, compositorHeight, compErr,
                                compositorFormat, overlayRefWhiteNits)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::Prepare, "GPU compositor init: " + compErr);
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
        gpuCompositorReady = true;
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

    // Native HDR10 monitoring decode (PQ/BT.2020 P010 -> tone-mapped SDR BGRA),
    // shared by the snapshot readback and the live preview tap. Its transfer
    // tables depend only on the session display peak, so it is built once on
    // first use and reused for every frame. Never engaged on SDR sessions.
    std::optional<P010PqMonitorConverter> hdrMonitorConverter;
    auto hdrMonitorConvert = [&](const PlanarYuv420Frame& yuvSrc, uint8_t* out_bgra, uint32_t out_stride_bytes) {
        if (!hdrMonitorConverter)
            hdrMonitorConverter.emplace(hdrPeakScale);
        hdrMonitorConverter->Convert(yuvSrc, out_bgra, out_stride_bytes);
    };

    // Perform a one-shot NV12→BGRA readback for the current slot if a snapshot is pending.
    // Called only on real frames (not duplicates) to ensure non-stale data.
    // NOTE: The Map(D3D11_MAP_READ) call below provides the minimal synchronization point;
    //       it stalls the thread until the GPU completes the CopyResource, typically <1 ms.
    auto performSnapshotIfRequested = [&](int32_t slot_idx) {
        if (!m_state.snapshot_requested.load())
            return;

        // Lazily allocate the staging texture on first use. Matches encodeFormat
        // (NV12 for 8-bit, P010 for 10-bit) so both bit depths can snapshot.
        if (!snapshotStagingTex) {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = encodeWidth;
            sd.Height = encodeHeight;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = encodeFormat;
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

        // Copy the final NV12/P010 encode-ready frame to the staging texture.
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

        // NV12/P010 layout:
        //   Plane Y:  rows 0 .. height-1, each row = RowPitch bytes
        //   Plane UV: rows height .. height+height/2-1, interleaved U V, same RowPitch
        const auto* y_plane = static_cast<const uint8_t*>(mapped.pData);
        const auto* uv_plane = y_plane + static_cast<size_t>(mapped.RowPitch) * encodeHeight;

        std::vector<uint8_t> bgra;
        bgra.resize(static_cast<size_t>(encodeWidth) * encodeHeight * 4u);

        // Convert using the color space the session actually configured
        // (see color_metadata.h / RecorderConfig::color) rather than a
        // hard-coded assumption — the encoder always writes BT.709-tagged
        // output with the user-selected range, so the readback must match.
        PlanarYuv420Frame yuvSrc;
        yuvSrc.y_plane = y_plane;
        yuvSrc.y_stride_bytes = mapped.RowPitch;
        yuvSrc.uv_plane = uv_plane;
        yuvSrc.uv_stride_bytes = mapped.RowPitch;
        yuvSrc.width = encodeWidth;
        yuvSrc.height = encodeHeight;
        yuvSrc.bits_per_sample = tenBit ? 10u : 8u;

        if (hdrNativeActive) {
            // Native HDR10: the P010 holds PQ/BT.2020, not SDR BT.709. Decode and
            // tone-map to SDR for on-screen monitoring (approximate; see
            // hdr_preview.h) at the session display peak.
            hdrMonitorConvert(yuvSrc, bgra.data(), encodeWidth * 4u);
        } else {
            YuvToBgraParams colorParams;
            colorParams.matrix = m_state.config.color.matrix;
            colorParams.range = m_state.config.color.range;
            ConvertYuv420ToBgra(yuvSrc, colorParams, bgra.data(), encodeWidth * 4u);
        }

        d3dContext->Unmap(snapshotStagingTex.get(), 0);

        if (pending_cb)
            pending_cb(true, encodeWidth, encodeHeight, std::move(bgra), {});
    };

    // --- Live WYSIWYG preview tap (Strand 3 slice 1) ---
    // Throttled to ~30 Hz and zero-cost when no callback is registered (a
    // single std::function bool-check before any GPU work). The staging ring
    // (see preview_staging_ring.h) lets Map() read back a copy submitted on
    // the PREVIOUS publish tick, so it resolves immediately instead of
    // stalling on the newest, possibly still in-flight copy; the published
    // frame therefore carries the ring's per-slot timestamp (one tick old),
    // never the current tick's PTS.
    //
    // The CPU YUV->BGRA conversion runs inline on VideoThread rather than on
    // a separate worker thread. Deliberate, pragmatic choice: the conversion
    // is integer fixed-point (see yuv_to_bgra.cpp) and the destination
    // buffer is reused across ticks, so at the ~30 Hz throttle rate this is
    // a small, bounded, measured per-tick cost (see the slice benchmark).
    // Handing it to a worker thread would remove that cost from VideoThread
    // entirely, but adds cross-thread lifetime/synchronization for the
    // source buffer that this throttled cadence does not need.
    PreviewStagingRing previewStagingRing;
    bool previewStagingReady = false;
    int previewInitFailures = 0;
    constexpr int kPreviewMaxInitFailures = 3; // give up for the session after this many
    bool previewCallbackFaulted = false;       // one-shot log guard for throwing callbacks
    PreviewPublishGate previewGate(kPreviewMinIntervalNs);
    PreviewFrame previewFrame; // persistent: bgra buffer reused across ticks

    auto publishPreviewIfDue = [&](int32_t slot_idx, uint64_t pts_ns) {
        if (!m_state.preview_frame_callback)
            return; // unregistered -- zero cost beyond this check
        if (previewInitFailures >= kPreviewMaxInitFailures)
            return; // staging init failed repeatedly -- preview disabled for this session

        if (!previewGate.ShouldPublish(pts_ns))
            return;

        if (!previewStagingReady) {
            D3D11_TEXTURE2D_DESC srcDesc{};
            nv12Textures[slot_idx]->GetDesc(&srcDesc);
            previewStagingReady = previewStagingRing.Initialize(d3dDevice.get(), srcDesc);
            if (!previewStagingReady) {
                ++previewInitFailures;
                if (previewInitFailures == kPreviewMaxInitFailures) {
                    logging::log(logging::LogLevel::Warn, "video_thread",
                                 "preview staging ring init failed repeatedly; preview disabled for this session", {});
                }
                return;
            }
        }

        previewStagingRing.Submit(d3dContext.get(), nv12Textures[slot_idx].get(), pts_ns);

        const uint8_t* mapped_data = nullptr;
        uint32_t mapped_pitch = 0;
        uint64_t mapped_pts_ns = 0;
        if (!previewStagingRing.TryReadReady(d3dContext.get(), &mapped_data, &mapped_pitch, &mapped_pts_ns))
            return; // ring not primed / copy still in flight -- next tick will have data

        // RAII: the slot is unmapped even if buffer sizing or the app
        // callback throws (otherwise the preview would be dead for the rest
        // of the session, or the exception would escape VideoThread::Run and
        // std::terminate).
        PreviewRingReadGuard readGuard(previewStagingRing, d3dContext.get());

        try {
            previewFrame.width = encodeWidth;
            previewFrame.height = encodeHeight;
            previewFrame.stride_bytes = encodeWidth * 4u;
            previewFrame.timestamp_ns = mapped_pts_ns; // one-tick-old frame => its own PTS
            previewFrame.bgra.resize(static_cast<size_t>(previewFrame.stride_bytes) * encodeHeight);

            PlanarYuv420Frame yuvSrc;
            yuvSrc.y_plane = mapped_data;
            yuvSrc.y_stride_bytes = mapped_pitch;
            yuvSrc.uv_plane = mapped_data + static_cast<size_t>(mapped_pitch) * encodeHeight;
            yuvSrc.uv_stride_bytes = mapped_pitch;
            yuvSrc.width = encodeWidth;
            yuvSrc.height = encodeHeight;
            yuvSrc.bits_per_sample = tenBit ? 10u : 8u;

            if (hdrNativeActive) {
                // Native HDR10: the P010 holds PQ/BT.2020, not SDR BT.709. Decode
                // and tone-map to SDR for the live preview (approximate; see
                // hdr_preview.h) at the session display peak.
                hdrMonitorConvert(yuvSrc, previewFrame.bgra.data(), previewFrame.stride_bytes);
            } else {
                YuvToBgraParams colorParams;
                colorParams.matrix = m_state.config.color.matrix;
                colorParams.range = m_state.config.color.range;
                ConvertYuv420ToBgra(yuvSrc, colorParams, previewFrame.bgra.data(), previewFrame.stride_bytes);
            }

            m_state.preview_frame_callback(previewFrame);
        } catch (...) {
            // The callback contract says it must not throw (see
            // recorder_session.h); a bad_alloc from resize lands here too.
            // Drop this frame; recording continues unaffected.
            if (!previewCallbackFaulted) {
                previewCallbackFaulted = true;
                logging::log(logging::LogLevel::Warn, "video_thread",
                             "preview publish threw (callback or allocation); frame dropped", {});
            }
        }
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

        // Seed the first WGC frame from the wait loop so a static window still
        // encodes from t=0 (WGC only delivers frames on repaint).
        winrt::com_ptr<ID3D11Texture2D> pendingWgcTex = std::move(seedWgcTex);

        // Present-cadence tap state (DXGI OD only): previous frame's LastPresentTime (QPC).
        uint64_t cfrLastPresentQpc = 0;

        // --- Phase-correct CFR pacing (DXGI-OD + Smooth mode only, ADR 0035) ---
        // Ring of captured frames (negotiated capture format) keyed by their source present-QPC. Each CFR
        // slot then encodes the frame whose present time is nearest the slot's ideal
        // present time, instead of newest-at-tick. WGC capture and Newest mode keep
        // the single-texture (odCapturedTex / pendingWgcTex) path verbatim.
        struct CaptureRingEntry {
            winrt::com_ptr<ID3D11Texture2D> tex;
            uint64_t presentQpc = 0; // raw QPC of source present; 0 == empty / consumed slot
        };
        std::vector<CaptureRingEntry> captureRing;
        size_t ringHead = 0;                // next physical slot to overwrite (round-robin)
        uint64_t lastEmittedPresentQpc = 0; // present-QPC of the last frame handed to the encoder
        bool phaseRingHasFrame = false;     // true once any live entry exists since last reset
        // odCapturedTex is null only if stop was requested before the first OD
        // frame arrived (lazy format negotiation) — no ring needed then.
        bool usePhaseCorrect =
            useOdCapture && odCapturedTex != nullptr && m_state.config.cfr_pacing_mode == FramePacingMode::Smooth;
        if (usePhaseCorrect) {
            const uint32_t outFps = (m_state.config.frame_rate_den > 0)
                                        ? (m_state.config.frame_rate_num / m_state.config.frame_rate_den)
                                        : 0u;
            // Use the actual refresh rate from the OD descriptor (available after odSrc.Open()).
            // Non-fatal: if 0, ComputePacingRingSize falls back to a safe ring of 8.
            const size_t ringN = ComputePacingRingSize(odSrc.RefreshRateHz(), outFps);
            D3D11_TEXTURE2D_DESC ringDesc{};
            odCapturedTex->GetDesc(&ringDesc); // mirror the OD capture texture exactly
            captureRing.resize(ringN);
            for (auto& entry : captureRing) {
                HRESULT ringHr = d3dDevice->CreateTexture2D(&ringDesc, nullptr, entry.tex.put());
                if (FAILED(ringHr)) {
                    // Non-fatal: disable phase-correct for the session and fall back to
                    // the single-texture newest-at-tick path. Recording must never fail
                    // because the pacing ring could not be allocated.
                    captureRing.clear();
                    usePhaseCorrect = false;
                    break;
                }
            }
        }

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
                // DXGI OD: drain all available frames. Newest-at-tick copies into
                // odCapturedTex; phase-correct copies into the present-QPC ring.
                const auto acq_t0 = std::chrono::steady_clock::now();
                while (true) {
                    ID3D11Texture2D* rawTex = nullptr;
                    DXGI_OUTDUPL_FRAME_INFO info{};
                    HRESULT odHr = S_OK;
                    if (!odSrc.TryAcquireFrame(0, &rawTex, &info, &odHr)) {
                        if (odHr == DXGI_ERROR_ACCESS_LOST)
                            sourceLost = true;
                        break;
                    }
                    // Format guard: skip foreign-format frames; fatal on size
                    // change (explicit failure, not a silent CopyResource no-op).
                    {
                        const OdFrameCheck check = checkOdFrame(rawTex);
                        if (check != OdFrameCheck::Ok) {
                            rawTex->Release();
                            odSrc.ReleaseFrame();
                            if (check == OdFrameCheck::Fatal)
                                break; // RecordFailure has set stop_requested
                            continue;  // Skip: try the next frame
                        }
                    }
                    // Only count capture/coalesce while actively recording — frames the
                    // backend produces during pause are intentionally discarded, not drops.
                    const bool diag_recording = !m_state.pause_requested.load();
                    if (usePhaseCorrect) {
                        // Round-robin write into the ring keyed by source present-QPC.
                        CaptureRingEntry& entry = captureRing[ringHead];
                        // Evicting a fresh, never-emitted frame is a genuine drop.
                        if (entry.presentQpc != 0 && entry.presentQpc > lastEmittedPresentQpc) {
                            ++droppedFrames;
                            if (diag_recording)
                                m_state.diagnostics.OnFrameDroppedCoalesced();
                        }
                        d3dContext->CopyResource(entry.tex.get(), rawTex);
                        uint64_t entryPresentQpc = static_cast<uint64_t>(info.LastPresentTime.QuadPart);
                        if (entryPresentQpc == 0) {
                            // No source present timestamp this acquire; stamp with the
                            // current QPC (same clock domain) so the frame stays
                            // selectable and monotonic and is never lost.
                            LARGE_INTEGER nowQpc;
                            QueryPerformanceCounter(&nowQpc);
                            entryPresentQpc = static_cast<uint64_t>(nowQpc.QuadPart);
                        }
                        entry.presentQpc = entryPresentQpc;
                        ringHead = (ringHead + 1) % captureRing.size();
                        phaseRingHasFrame = true;
                    } else {
                        d3dContext->CopyResource(odCapturedTex.get(), rawTex);
                    }
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
                    if (diag_recording)
                        m_state.diagnostics.OnFrameCaptured();
                    // Present-cadence tap (VRR/CFR judder correlation). DXGI OD exposes the
                    // source's last present timestamp (QPC) plus the coalesced-update count.
                    // O(1): derive the inter-present delta and hand raw numbers to the
                    // aggregator. Skip null/non-advancing presents — never fabricate a value.
                    if (diag_recording && info.LastPresentTime.QuadPart != 0 && qpcFreq != 0) {
                        const auto presentQpc = static_cast<uint64_t>(info.LastPresentTime.QuadPart);
                        if (cfrLastPresentQpc != 0 && presentQpc > cfrLastPresentQpc) {
                            const double intervalMs = static_cast<double>(presentQpc - cfrLastPresentQpc) * 1000.0 /
                                                      static_cast<double>(qpcFreq);
                            m_state.diagnostics.OnSourcePresentInterval(std::chrono::steady_clock::now(), intervalMs,
                                                                        info.AccumulatedFrames);
                        }
                        cfrLastPresentQpc = presentQpc;
                    }
                    if (!usePhaseCorrect) {
                        if (odCapturedTexValid) {
                            ++droppedFrames;
                            if (diag_recording)
                                m_state.diagnostics.OnFrameDroppedCoalesced();
                        }
                        odCapturedTexValid = true;
                    }
                }
                if (!m_state.pause_requested.load()) {
                    const auto acq_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnAcquireLatency(
                        acq_t1, std::chrono::duration<double, std::milli>(acq_t1 - acq_t0).count());
                }
            } else {
                // WGC: drain frame pool — keep latest (always drain, even when paused)
                const auto acq_t0 = std::chrono::steady_clock::now();
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
                if (!m_state.pause_requested.load()) {
                    const auto acq_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnAcquireLatency(
                        acq_t1, std::chrono::duration<double, std::milli>(acq_t1 - acq_t0).count());
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
                if (usePhaseCorrect) {
                    // Discard frames captured during pause; the epoch shifts on resume.
                    for (auto& entry : captureRing)
                        entry.presentQpc = 0;
                    ringHead = 0;
                    phaseRingHasFrame = false;
                }
                Sleep(1);
                continue;
            }
            if (cfr_was_paused) {
                epochQpc100ns += Qpc100ns(qpcFreq) - cfr_pause_start_100ns;
                cfr_was_paused = false;
            }

            // Set epoch on first frame arrival (OD or WGC). In phase-correct
            // mode the first frame lives in odCapturedTex (seeded by the
            // wait-for-first-frame loop), NOT in the present-QPC ring — it must
            // still open the epoch, otherwise a monitor that stays static after
            // the first frame never encodes anything and the session dies
            // minutes later as an opaque "codec private data" mux error
            // (measured live on a 10 bpc SDR desktop, fix/od-10bit-desktop).
            const bool odHasFrame = usePhaseCorrect ? (phaseRingHasFrame || odCapturedTexValid) : odCapturedTexValid;
            const bool hasNewFrame = useOdCapture ? odHasFrame : (pendingWgcTex != nullptr);
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
                ID3D11Texture2D* rawSourceTex = nullptr;
                if (usePhaseCorrect) {
                    // Linearise the round-robin ring to ascending (oldest -> newest)
                    // present order; ringHead points at the oldest physical slot.
                    std::vector<uint64_t> presentQpcsAscending;
                    std::vector<size_t> liveIndexToRingSlot;
                    const size_t ringN = captureRing.size();
                    presentQpcsAscending.reserve(ringN);
                    liveIndexToRingSlot.reserve(ringN);
                    for (size_t i = 0; i < ringN; ++i) {
                        const size_t phys = (ringHead + i) % ringN;
                        if (captureRing[phys].presentQpc != 0) {
                            presentQpcsAscending.push_back(captureRing[phys].presentQpc);
                            liveIndexToRingSlot.push_back(phys);
                        }
                    }
                    // slotQpc: this tick's ideal present time in the raw-QPC present-time
                    // domain (same clock as info.LastPresentTime). The epoch is tracked
                    // in 100ns; convert epoch + relative offset back to QPC ticks.
                    const uint64_t slotRel100ns = cfr_frame_idx * frame_interval_100ns;
                    const uint64_t epochRawQpc =
                        (epochQpc100ns / 10000000ULL) * qpcFreq + (epochQpc100ns % 10000000ULL) * qpcFreq / 10000000ULL;
                    const uint64_t slotRelRawQpc =
                        (slotRel100ns / 10000000ULL) * qpcFreq + (slotRel100ns % 10000000ULL) * qpcFreq / 10000000ULL;
                    const uint64_t slotQpc = epochRawQpc + slotRelRawQpc;

                    const PacingDecision dec = SelectFrameForSlot(presentQpcsAscending, slotQpc, lastEmittedPresentQpc,
                                                                  FramePacingMode::Smooth);
                    // Fresh entries older than the chosen one were skipped: real drops.
                    droppedFrames += dec.newly_dropped;
                    for (uint32_t d = 0; d < dec.newly_dropped; ++d)
                        m_state.diagnostics.OnFrameDroppedCoalesced();
                    if (dec.emit) {
                        rawSourceTex = captureRing[liveIndexToRingSlot[dec.index]].tex.get();
                        lastEmittedPresentQpc = presentQpcsAscending[dec.index];
                        // Consume the emitted entry and every skipped/older one so they
                        // are not re-selected or counted again as eviction drops.
                        for (auto& entry : captureRing) {
                            if (entry.presentQpc != 0 && entry.presentQpc <= lastEmittedPresentQpc)
                                entry.presentQpc = 0;
                        }
                    } else {
                        // No fresh frame near this slot -> existing duplicate / CFR-skip path.
                        rawSourceTex = nullptr;
                    }
                    // Seed: until a reference NV12 exists (session start) the
                    // ring may be empty while odCapturedTex already holds the
                    // first frame from the wait loop — use it so a static
                    // desktop encodes real content from t=0 instead of
                    // silently dropping every tick.
                    if (rawSourceTex == nullptr && !refNv12Valid && odCapturedTexValid) {
                        rawSourceTex = odCapturedTex.get();
                    }
                } else {
                    rawSourceTex =
                        useOdCapture ? (odCapturedTexValid ? odCapturedTex.get() : nullptr) : pendingWgcTex.get();
                }

                if (rawSourceTex != nullptr && hdrNativeActive) {
                    // Native HDR10: composite webcam/cursor in linear scRGB FP16,
                    // then convert straight into the P010 slot (colour + geometry).
                    const WebcamOverlayLive overlay = m_state.SnapshotWebcamOverlay();
                    const auto comp_t0 = std::chrono::steady_clock::now();
                    ID3D11Texture2D* nativeSrc = compositeFrameGpu(rawSourceTex, overlay);
                    const auto comp_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnCompositorSubmit(
                        comp_t1, std::chrono::duration<double, std::milli>(comp_t1 - comp_t0).count(),
                        needsGpuCompositor);
                    if (nativeSrc == nullptr) {
                        nvenc.ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    if (useOdCapture) {
                        odCapturedTexValid = false;
                    } else {
                        pendingWgcTex = nullptr;
                    }
                    const auto conv_t0 = std::chrono::steady_clock::now();
                    if (!encodeNativeHdrSlot(nativeSrc, slot)) {
                        nvenc.ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    const auto conv_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnVpbltSubmit(
                        conv_t1, std::chrono::duration<double, std::milli>(conv_t1 - conv_t0).count());
                    if (refNv12 != nullptr) {
                        d3dContext->CopyResource(refNv12.get(), nv12Textures[slot].get());
                        refNv12Valid = true;
                    }
                    performSnapshotIfRequested(slot);
                    publishPreviewIfDue(slot, pts_ns);
                    frameWritten = true;
                } else if (rawSourceTex != nullptr) {
                    const WebcamOverlayLive overlay = m_state.SnapshotWebcamOverlay();
                    const auto comp_t0 = std::chrono::steady_clock::now();
                    ID3D11Texture2D* sdrSourceTex = toneMapIfHdr(rawSourceTex);
                    if (sdrSourceTex == nullptr) {
                        nvenc.ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    ID3D11Texture2D* vpInput = compositeFrameGpu(sdrSourceTex, overlay);
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

                    // Convert RGB frame to NV12/P010 via VideoProcessorBlt
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

                        const auto vp_t0 = std::chrono::steady_clock::now();
                        hr = videoContext->VideoProcessorBlt(videoProcessor.get(), videoOutputViews[slot].get(), 0, 1,
                                                             &stream);
                        const auto vp_t1 = std::chrono::steady_clock::now();
                        m_state.diagnostics.OnVpbltSubmit(
                            vp_t1, std::chrono::duration<double, std::milli>(vp_t1 - vp_t0).count());
                        inputView = nullptr;

                        if (SUCCEEDED(hr)) {
                            // Save NV12 as reference for future duplicate frames
                            if (refNv12 != nullptr) {
                                d3dContext->CopyResource(refNv12.get(), nv12Textures[slot].get());
                                refNv12Valid = true;
                            }
                            // Capture frame snapshot on real (non-duplicate) frames.
                            performSnapshotIfRequested(slot);
                            // Live preview tap on real (non-duplicate) frames (throttled internally).
                            publishPreviewIfDue(slot, pts_ns);
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

        // Present-cadence tap state (DXGI OD only): previous frame's LastPresentTime (QPC).
        uint64_t vfrLastPresentQpc = 0;

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
                    // Format guard: skip foreign-format frames; fatal on size
                    // change (explicit failure, not a silent CopyResource no-op).
                    {
                        const OdFrameCheck check = checkOdFrame(rawTex);
                        if (check != OdFrameCheck::Ok) {
                            rawTex->Release();
                            odSrc.ReleaseFrame();
                            if (check == OdFrameCheck::Fatal)
                                break; // RecordFailure has set stop_requested
                            continue;  // Skip: try the next frame
                        }
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
                    // Present-cadence tap (VRR/CFR judder correlation), mirroring the CFR path.
                    if (diag_recording && info.LastPresentTime.QuadPart != 0 && qpcFreq != 0) {
                        const auto presentQpc = static_cast<uint64_t>(info.LastPresentTime.QuadPart);
                        if (vfrLastPresentQpc != 0 && presentQpc > vfrLastPresentQpc) {
                            const double intervalMs = static_cast<double>(presentQpc - vfrLastPresentQpc) * 1000.0 /
                                                      static_cast<double>(qpcFreq);
                            m_state.diagnostics.OnSourcePresentInterval(std::chrono::steady_clock::now(), intervalMs,
                                                                        info.AccumulatedFrames);
                        }
                        vfrLastPresentQpc = presentQpc;
                    }
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
                // Seed the first WGC frame from the wait loop so a static
                // window still encodes at least one real frame (WGC only
                // delivers frames on repaint).
                if (latestTex == nullptr && seedWgcTex != nullptr) {
                    latestTex = std::move(seedWgcTex);
                    latestFrameTicks100ns = static_cast<int64_t>(Qpc100ns(qpcFreq));
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

                if (slot >= 0 && hdrNativeActive) {
                    // Native HDR10 (VFR): composite webcam/cursor in linear scRGB
                    // FP16, then convert straight into the P010 slot.
                    const WebcamOverlayLive overlay = m_state.SnapshotWebcamOverlay();
                    const auto comp_t0 = std::chrono::steady_clock::now();
                    ID3D11Texture2D* nativeSrc = compositeFrameGpu(latestTex.get(), overlay);
                    const auto comp_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnCompositorSubmit(
                        comp_t1, std::chrono::duration<double, std::milli>(comp_t1 - comp_t0).count(),
                        needsGpuCompositor);
                    if (nativeSrc == nullptr) {
                        nvenc.ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    if (useOdCapture) {
                        odCapturedTexValid = false;
                    }
                    const auto conv_t0 = std::chrono::steady_clock::now();
                    if (!encodeNativeHdrSlot(nativeSrc, slot)) {
                        nvenc.ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    const auto conv_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnVpbltSubmit(
                        conv_t1, std::chrono::duration<double, std::milli>(conv_t1 - conv_t0).count());
                    latestTex = nullptr;

                    performSnapshotIfRequested(slot);
                    publishPreviewIfDue(slot, framePts_ns);
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
                } else if (slot >= 0) {
                    const WebcamOverlayLive overlay = m_state.SnapshotWebcamOverlay();
                    const auto comp_t0 = std::chrono::steady_clock::now();
                    ID3D11Texture2D* sdrSourceTex = toneMapIfHdr(latestTex.get());
                    if (sdrSourceTex == nullptr) {
                        nvenc.ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    ID3D11Texture2D* vpInput = compositeFrameGpu(sdrSourceTex, overlay);
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

                    // RGB -> NV12/P010 via VideoProcessorBlt into the selected slot's view
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

                        const auto vp_t0 = std::chrono::steady_clock::now();
                        hr = videoContext->VideoProcessorBlt(videoProcessor.get(), videoOutputViews[slot].get(), 0, 1,
                                                             &stream);
                        const auto vp_t1 = std::chrono::steady_clock::now();
                        m_state.diagnostics.OnVpbltSubmit(
                            vp_t1, std::chrono::duration<double, std::milli>(vp_t1 - vp_t0).count());

                        inputView = nullptr;
                        latestTex = nullptr;

                        if (SUCCEEDED(hr)) {
                            // Capture frame snapshot on real frames (VFR path).
                            performSnapshotIfRequested(slot);
                            // Live preview tap on real frames (VFR path; throttled internally).
                            publishPreviewIfDue(slot, framePts_ns);

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
