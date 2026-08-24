#include "video_thread.h"

#include "annexb_to_avcc.h"
#include "annexb_to_hvcc.h"
#include "av_epoch_align.h"
#include "codec_private.h"
#include "gpu_compositor.h"
#include "gpu_hdr_pq.h"
#include "gpu_rgb_to_ayuv.h"
#include "hdr_preview.h"
#include "hdr_tonemap.h"
#include <exosnap/engine/dxgi_od_capture_src.h>

#include "preview_publish_gate.h"
#include "session_internal.h"
#include "split_sentinel_policy.h"
#include "yuv_to_bgra.h"
#include <exosnap/engine/cursor_sprite.h>
#include <exosnap/engine/gpu_hdr_tonemap.h>
#include <exosnap/engine/gpu_timestamp_profiler.h>
#include <exosnap/engine/hdr_native.h>
#include <exosnap/engine/interfaces/VideoEncoderFactory.h>
#include <exosnap/engine/od_acquire_classify.h>
#include <exosnap/engine/preview_shared_texture.h>
#include <exosnap/engine/preview_tap.h>
#include <exosnap/engine/util/com_apartment.h>
#include <exosnap/engine/visual_generations.h>
#include <exosnap/engine/wgc_acquire_classify.h>

#include <exosnap/engine/frame_pacing.h>
#include <exosnap/engine/logging/logging.h>
#include <exosnap/engine/packet_types.h>
#include <exosnap/engine/sdr_white_level.h>
#include <exosnap/engine/webcam_placement.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <dwmapi.h>
#include <dxgi.h>

#include <array>
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
// The D3D11 device (d3dDevice), its immediate context (d3dContext) and the
// video context (videoContext) are created locally in Run() and owned for the
// lifetime of that call.  They are used EXCLUSIVELY on this VideoThread — no
// other thread in RecorderSession may call any method on these interfaces.
// Nothing here is borrowed from the session; the device is not shared.
// ============================================================================

namespace exosnap::engine {

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

// Win32CursorBitmap / CaptureWin32CursorBitmap / ScaleCoordinateToSource /
// ClipCursorSprite moved to <recorder_core/cursor_sprite.h>, shared with the
// DXGI preview's cursor sprite.

// A 10-bit R10G10B10A2 tone-map intermediate must be a supported VideoProcessor
// *input* on this driver, or every CreateVideoProcessorInputView call for it
// would fail per tick with no recorded failure — silent frame starvation. This
// demotes to BGRA8 (with a WARN log) when the check fails, and is a no-op for
// any other format. Shared by the OD first-frame negotiation and the WGC HDR
// tone-map init, since both choose the same toneMapIntermediateFormat and are
// exposed to the same driver limitation.
DXGI_FORMAT DemoteToneMapFormatIfUnsupportedVpInput(ID3D11VideoProcessorEnumerator* video_enum,
                                                    DXGI_FORMAT tone_map_intermediate_format) {
    if (tone_map_intermediate_format != DXGI_FORMAT_R10G10B10A2_UNORM)
        return tone_map_intermediate_format;
    UINT support = 0;
    const HRESULT hr = video_enum->CheckVideoProcessorFormat(tone_map_intermediate_format, &support);
    if (SUCCEEDED(hr) && (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) != 0)
        return tone_map_intermediate_format;
    logging::log(logging::LogLevel::Warn, "video_thread",
                 "10-bit tone-map intermediate (R10G10B10A2) is not a supported VideoProcessor input on "
                 "this driver; falling back to BGRA8 (8-bit tone-map precision)",
                 {});
    return DXGI_FORMAT_B8G8R8A8_UNORM;
}

// Turns a caught WGC acquire exception into the session's own outcome.
//
// A swallowed exception here is invisible in the worst possible way: the encode
// loop keeps emitting the last held frame, so a source that has permanently
// stopped delivering looks like a healthy recording for as long as the user
// leaves it running. Every class therefore terminates the session -- source loss
// through the same clean path the item's Closed event uses (`source_lost`, no
// failure), device loss and anything unclassified as an explicit failure that
// names its HRESULT.
//
// `source_lost` is set for every class so the caller's existing loop exit runs
// unchanged; RecordFailure additionally raises stop_requested.
void HandleWgcAcquireException(SessionState& state, int32_t hr, const char* context, bool& source_lost) {
    source_lost = true;
    char buf[160];
    switch (ClassifyWgcAcquireFailure(hr)) {
    case WgcAcquireFailure::SourceLost:
        snprintf(buf, sizeof(buf), "WGC %s: capture source lost 0x%08X", context, static_cast<unsigned int>(hr));
        logging::log(logging::LogLevel::Warn, "video_thread", buf, {});
        return;
    case WgcAcquireFailure::DeviceLost:
        snprintf(buf, sizeof(buf), "WGC %s: capture device lost 0x%08X", context, static_cast<unsigned int>(hr));
        state.RecordFailure(hr, ErrorPhase::VideoCapture, buf);
        return;
    case WgcAcquireFailure::Unexpected:
        snprintf(buf, sizeof(buf), "WGC %s: unexpected acquire failure 0x%08X", context, static_cast<unsigned int>(hr));
        state.RecordFailure(hr, ErrorPhase::VideoCapture, buf);
        return;
    }
}

// Non-WinRT exceptions carry no HRESULT to classify. They still must not be
// swallowed: the catch is a boundary, not a retry.
void HandleWgcUnknownException(SessionState& state, const char* context, bool& source_lost) {
    source_lost = true;
    char buf[160];
    snprintf(buf, sizeof(buf), "WGC %s: non-WinRT exception during frame acquire", context);
    state.RecordFailure(E_UNEXPECTED, ErrorPhase::VideoCapture, buf);
}

} // namespace

VideoThread::VideoThread(std::shared_ptr<SessionState> state) : m_state_ptr(std::move(state)), m_state(*m_state_ptr) {
}

VideoThread::~VideoThread() {
    // Start() gave the running thread shared ownership of this object, so a
    // joinable thread here has already returned from Run() (the final release
    // may even happen on the worker thread itself, where join() would
    // deadlock). Detaching a finished thread only releases its handle.
    if (m_thread.joinable())
        m_thread.detach();
}

void VideoThread::Start() {
    // Self-ownership handoff: the lambda keeps this worker (and through
    // m_state_ptr the SessionState) alive until Run() returns, so dropping the
    // session's handle on a hung producer can never dangle the state the
    // thread still writes through.
    m_thread = std::thread([self = shared_from_this()] { self->Run(); });
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
    // ComApartment's destructor calls CoUninitialize() exactly when this call
    // owned the reference (S_OK/S_FALSE) at every exit path below -- never on
    // RPC_E_CHANGED_MODE, where this call acquired no reference to release.
    exosnap::engine::ComApartment comApartment(COINIT_APARTMENTTHREADED);
    HRESULT hr = comApartment.result();
    if (!comApartment.usable()) {
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

    // Real GPU-execution-time profilers for the three GPU passes on the immediate
    // context (composite, scRGB->SDR tone-map, RGB->YUV blit). Strictly additive
    // and non-blocking: they only bracket the passes and harvest earlier frames'
    // results with DONOTFLUSH, never stalling the hot path. Init lazily once the
    // device exists (guarded below), staying inert if timestamp queries are
    // unsupported. Distinct from the existing CPU-submission windows.
    GpuStageTimer compositeGpuTimer;
    GpuStageTimer tonemapGpuTimer;
    GpuStageTimer vpbltGpuTimer;
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
            return;
        }

        hr = d3dDevice->QueryInterface(IID_PPV_ARGS(videoDevice.put()));
        if (FAILED(hr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "QI ID3D11VideoDevice failed 0x%08lX", static_cast<unsigned long>(hr));
            m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
            return;
        }

        hr = d3dContext->QueryInterface(IID_PPV_ARGS(videoContext.put()));
        if (FAILED(hr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "QI ID3D11VideoContext failed 0x%08lX", static_cast<unsigned long>(hr));
            m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
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
    // The OS SDR reference white, in the same reference-white multiples. An HDR
    // desktop composes SDR content at that level rather than at scRGB's nominal
    // 80 nits, so the tone-map has to divide it back out; 1.0 is the
    // scene-referred identity used until the capture source is known.
    float hdrPaperWhiteScale = 1.0f;
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
            return;
        }
        hdrPeakScale = HdrPeakScale(odSrc.HdrActive(), odSrc.MaxLuminanceNits());
        hdrPaperWhiteScale = SdrPaperWhiteScale(odSrc.DisplayFacts().sdr_white_level_nits);
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
        hdrPaperWhiteScale = SdrPaperWhiteScale(wgcHdrFacts.sdr_white_level_nits);
        expectNativeHdr =
            IsHdr10NativeEffective(m_state.config.hdr_mode, wgcHdrFacts.hdr_active, m_state.config.video_codec);

        char wgcFmtBuf[32];
        const logging::LogField wgcFields[] = {
            {"hdr_active", BoolText(wgcHdrFacts.hdr_active)},
            {"framePoolFormat", OdCaptureFormatName(wgcPlan.frame_pool_format, wgcFmtBuf, sizeof(wgcFmtBuf))},
            {"handling",
             wgcPlan.mode == OdCaptureMode::HdrNative
                 ? "native HDR10 PQ/BT.2020 -> P010"
                 : (wgcPlan.mode == OdCaptureMode::HdrToneMap ? "HDR scRGB -> SDR BT.709 tone-map" : "SDR")}};
        logging::log(logging::LogLevel::Info, "video_thread", "WGC capture plan resolved",
                     std::span<const logging::LogField>(wgcFields, std::size(wgcFields)));
    }

    // hdr_active as resolved above, kept for the periodic re-check below (same
    // monitor handle used on both paths — the WGC path's documented "resolved once
    // at session start" limitation applies here too).
    const bool initialHdrActive = useOdCapture ? odSrc.HdrActive() : wgcHdrFacts.hdr_active;
    const HMONITOR hdrCheckMonitor =
        useOdCapture ? reinterpret_cast<HMONITOR>(target.native_id)
                     : MonitorFromWindow(reinterpret_cast<HWND>(target.native_id), MONITOR_DEFAULTTONEAREST);

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
         << (m_state.config.video_codec == VideoCodec::H264
                 ? "H264_NVENC"
                 : (m_state.config.video_codec == VideoCodec::Hevc ? "HEVC_NVENC" : "AV1_NVENC"))
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
        return;
    }

    const std::optional<OutputGeometry> outputGeometry =
        ResolveOutputGeometry({sourceContentWidth, sourceContentHeight}, {encodeWidth, encodeHeight});
    if (!outputGeometry.has_value()) {
        std::ostringstream err;
        err << "output geometry invalid for source " << sourceContentWidth << "x" << sourceContentHeight
            << " and output " << encodeWidth << "x" << encodeHeight << "; preInit={" << diag.str() << "}";
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::VideoCapture, err.str());
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
    // Encoder dispatch (IVideoEncoder-refactor design spec). Vendor is
    // hard-coded to Nvidia until the AMD wave threads real device selection
    // from libs/capability through to this call.
    std::unique_ptr<IVideoEncoder> encoder =
        m_state.video_encoder_factory->Create(exosnap::capability::AdapterVendor::Nvidia);
    if (!encoder) {
        m_state.RecordFailure(E_FAIL, ErrorPhase::Prepare,
                              "No video encoder available for the configured adapter vendor");
        return;
    }
    {
        encoder->SetCodec(m_state.config.video_codec);
        encoder->SetBitDepth(m_state.config.bit_depth);
        encoder->SetChroma(m_state.config.chroma);
        encoder->SetCq(m_state.config.cq);
        encoder->SetRateControl(m_state.config.nvenc_rate_control, m_state.config.nvenc_bitrate_kbps);
        encoder->SetPreset(m_state.config.nvenc_preset);
        // Keyframe interval (Settings → Advanced → Video). Must be set before
        // Configure() so InitEncoder derives gopLength/idrPeriod from it; without
        // this the encoder silently stays at its 2 s default and the selector has
        // no effect.
        encoder->SetKeyframeIntervalSecs(m_state.config.keyframe_interval_secs);
        // Submission regime. The keyframe cadence is media-time based either way;
        // this only decides whether NVENC's frame-counting gopLength/idrPeriod
        // backstop stays armed. Under VFR the loop below submits every frame the
        // source produces, so a source faster than the configured rate would trip
        // that counter before the media-time boundary — see ComputeNvencGopBackstop.
        encoder->SetConstantFrameRate(m_state.config.cfr);
        // Color signaling (fix for color-range-signaling bug): the encoded
        // bitstream itself must carry the same color description as the
        // VideoProcessor conversion below and the Matroska Colour element
        // (mux_thread.cpp), otherwise players/ffprobe that read color info from
        // the bitstream (all of them for AV1 — verified; container tags are
        // ignored) see an untagged/wrong-range stream regardless of correct
        // container tagging.
        encoder->SetColor(m_state.config.color);

        std::string err;
        if (!encoder->Open(d3dDevice.get(), err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::Prepare, "NVENC open: " + err);
            return;
        }
        if (!encoder->Configure(encodeWidth, encodeHeight, m_state.config.frame_rate_num, m_state.config.frame_rate_den,
                                err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoEncode,
                                  "NVENC configure: " + err + "; preInit={" + diag.str() + "}");
            return;
        }

        // Capture the resolved encoder init parameters for the live diagnostics
        // snapshot and the on-disk session report, and emit them once as a
        // structured event so they reach the JSONL even if a later failure means no
        // final snapshot is produced. hdr_mode is a session-level concept the
        // encoder does not know, so it is filled from the config here.
        EncoderInitInfo enc_init = encoder->GetInitInfo();
        enc_init.hdr_mode = m_state.config.hdr_mode;
        m_state.diagnostics.SetEncoderInitInfo(enc_init);

        const char* rc_name = enc_init.rc_mode == RateControlMode::ConstantQuality   ? "cq"
                              : enc_init.rc_mode == RateControlMode::VariableBitrate ? "vbr"
                              : enc_init.rc_mode == RateControlMode::ConstantBitrate ? "cbr"
                                                                                     : "lossless";
        const std::vector<exosnap::engine::logging::LogField> init_fields = {
            {"codec", std::to_string(static_cast<int>(enc_init.codec))},
            {"preset", std::to_string(static_cast<int>(enc_init.preset))},
            {"rc", rc_name},
            {"target_kbps", std::to_string(enc_init.target_bitrate_kbps)},
            {"max_kbps", std::to_string(enc_init.max_bitrate_kbps)},
            {"gop", std::to_string(enc_init.gop_length)},
            {"bit_depth", enc_init.bit_depth == BitDepth::Bit10 ? "10" : "8"},
            {"spatial_aq", enc_init.spatial_aq ? "1" : "0"},
        };
        exosnap::engine::logging::log(
            exosnap::engine::logging::LogLevel::Info, "encoder", "encoder.init",
            std::span<const exosnap::engine::logging::LogField>(init_fields.data(), init_fields.size()));
    }

    // --- NV12 / P010 texture ring + video processor ---
    // For 10-bit recording (HEVC Main10 / AV1 10-bit, ADR 0032 SDR BT.709) the
    // VideoProcessor converts RGB → P010 instead of NV12, and the encode ring +
    // reference texture use DXGI_FORMAT_P010. The output color space stays studio
    // BT.709 (no HDR/BT.2020 here — that is a later slice).
    const bool tenBit = (m_state.config.bit_depth == BitDepth::Bit10);

    // Expert 4:4:4 (8-bit H.264/HEVC): the VideoProcessor cannot emit 4:4:4, so it
    // performs geometry (crop/scale/letterbox) into a full-range BGRA intermediate,
    // and a compute shader converts that into the packed AYUV surface NVENC
    // consumes (NV_ENC_BUFFER_FORMAT_AYUV). The 4:2:0 path is untouched. 4:4:4 is
    // 8-bit only and mutually exclusive with 10-bit / native HDR (blocked upstream).
    const bool chroma444 = (m_state.config.chroma == ChromaSubsampling::Cs444);
    // Encode texture registered with NVENC: NV12/P010 for 4:2:0, AYUV for 4:4:4.
    const DXGI_FORMAT encodeFormat = chroma444 ? DXGI_FORMAT_AYUV : (tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12);

    // Number of pipelined capture/encode slots (shared by every per-slot texture array).
    static constexpr int32_t kSlotCount = 8;

    // 4:4:4: the VideoProcessor output is a separate BGRA intermediate (RGB, geometry
    // only); the shader then writes AYUV into the encode textures. RgbToAyuvConverter
    // carries the BT.709 + range conversion the VideoProcessor does for 4:2:0.
    winrt::com_ptr<ID3D11Texture2D> vpRgbTextures[kSlotCount];
    RgbToAyuvConverter rgbToAyuv;
    if (chroma444) {
        const bool fullRange = m_state.config.color.range != ColorRange::Limited;
        std::string ayuvErr;
        if (!rgbToAyuv.Init(d3dDevice.get(), d3dContext.get(), encodeWidth, encodeHeight, fullRange, ayuvErr)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::Prepare, "RGB->AYUV shader init: " + ayuvErr);
            return;
        }
    }

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
        return;
    }

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
            return;
        }

        hr = videoDevice->CreateVideoProcessor(videoEnum.get(), 0, videoProcessor.put());
        if (FAILED(hr)) {
            char buf[80];
            snprintf(buf, sizeof(buf), "CreateVideoProcessor failed 0x%08lX", static_cast<unsigned long>(hr));
            m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
            return;
        }

        // Create the encode textures + VideoProcessor output views for each slot.
        // 4:2:0: the VP output view targets the NV12/P010 encode texture directly.
        // 4:4:4: the VP output view targets a BGRA intermediate (geometry only); the
        //        shader converts it into the AYUV encode texture registered below.
        for (int32_t i = 0; i < kSlotCount; ++i) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = encodeWidth;
            desc.Height = encodeHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = encodeFormat; // NV12 (8-bit) / P010 (10-bit) / AYUV (4:4:4)
            desc.SampleDesc = {1, 0};
            desc.Usage = D3D11_USAGE_DEFAULT;
            // AYUV encode textures are written by the compute shader (UAV) and copied
            // for frame duplication; NV12/P010 are VideoProcessorBlt render targets.
            desc.BindFlags = chroma444 ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_RENDER_TARGET;

            hr = d3dDevice->CreateTexture2D(&desc, nullptr, nv12Textures[i].put());
            if (FAILED(hr)) {
                char buf[96];
                snprintf(buf, sizeof(buf), "CreateTexture2D(%s[%d]) failed 0x%08lX",
                         chroma444 ? "AYUV" : (tenBit ? "P010" : "NV12"), static_cast<int>(i),
                         static_cast<unsigned long>(hr));
                m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
                return;
            }

            // The VideoProcessor output view target: BGRA intermediate for 4:4:4,
            // else the encode texture itself.
            ID3D11Texture2D* vpOutTex = nv12Textures[i].get();
            if (chroma444) {
                D3D11_TEXTURE2D_DESC rgbDesc = desc;
                rgbDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                rgbDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                hr = d3dDevice->CreateTexture2D(&rgbDesc, nullptr, vpRgbTextures[i].put());
                if (FAILED(hr)) {
                    char buf[96];
                    snprintf(buf, sizeof(buf), "CreateTexture2D(vpRgb[%d]) failed 0x%08lX", static_cast<int>(i),
                             static_cast<unsigned long>(hr));
                    m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
                    return;
                }
                vpOutTex = vpRgbTextures[i].get();
            }

            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc{};
            ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            ovDesc.Texture2D.MipSlice = 0;

            hr = videoDevice->CreateVideoProcessorOutputView(vpOutTex, videoEnum.get(), &ovDesc,
                                                             videoOutputViews[i].put());
            if (FAILED(hr)) {
                char buf[80];
                snprintf(buf, sizeof(buf), "CreateVideoProcessorOutputView[%d] failed 0x%08lX", static_cast<int>(i),
                         static_cast<unsigned long>(hr));
                m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
                return;
            }

            std::string err;
            if (!encoder->RegisterSlotTexture(i, nv12Textures[i].get(), err)) {
                m_state.RecordFailure(E_FAIL, ErrorPhase::Prepare, "NVENC register slot: " + err);
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
        if (chroma444) {
            // 4:4:4: the VideoProcessor does geometry only, emitting full-range BGRA
            // (no chroma subsampling); the RGB->AYUV shader applies BT.709 + the
            // selected quantization range. Input and output are both full-range RGB.
            if (videoContext1) {
                videoContext1->VideoProcessorSetStreamColorSpace1(videoProcessor.get(), 0,
                                                                  DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
                videoContext1->VideoProcessorSetOutputColorSpace1(videoProcessor.get(),
                                                                  DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            } else {
                D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgbColorSpace{};
                rgbColorSpace.Usage = 0;
                rgbColorSpace.RGB_Range = 0; // full-range RGB (0-255)
                rgbColorSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
                videoContext->VideoProcessorSetStreamColorSpace(videoProcessor.get(), 0, &rgbColorSpace);
                videoContext->VideoProcessorSetOutputColorSpace(videoProcessor.get(), &rgbColorSpace);
            }
        } else if (videoContext1) {
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
    //
    // Two roles, and they differ by pacing path. Without phase-correct pacing this is
    // the current captured desktop, rewritten by every acquire. With it, the ring
    // takes over the drain and this becomes the HELD SCREEN: the last emitted
    // desktop, rotated in by AdoptEmittedAsHeldScreen, which is what
    // ShouldRecompositeHeldScreen re-composites on a tick with no fresh capture but a
    // moved cursor or webcam. Either way it must track the newest screen the encode
    // has seen — a write that happens only once leaves the recording reverting to
    // that one frame for the rest of the session.
    winrt::com_ptr<ID3D11Texture2D> odCapturedTex;
    DXGI_FORMAT odFrameFormat = DXGI_FORMAT_UNKNOWN; // set when odCapturedTex is created
    bool odCapturedTexValid = false;

    // HDR (scRGB FP16) capture: odCapturedTex/ring textures hold the raw FP16
    // frames, and a per-emitted-frame tone-map pass converts them into hdrSdrTex
    // (an SDR BGRA8 surface) that then follows the normal SDR VideoProcessor
    // route. hdrToneMapActive is decided during first-frame negotiation.
    bool hdrToneMapActive = false;
    // The tone-map pass runs the SDR curve instead: the source is an SDR desktop
    // delivered as linear scRGB (Advanced Color Management), not an HDR one.
    bool hdrToneMapSdrSource = false;
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

    VisualGenerations visualGenerations{};
    VisualFrameKey lastCompositedKey{};
    bool haveLastCompositedKey = false;
    bool lastCursorCaptureEnabled = m_state.config.capture_cursor;
    uint64_t lastWebcamFrameGeneration = 0;
    bool haveWebcamFrameGeneration = false;

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
        // An SDR scRGB (Advanced Color) desktop runs the same shader pass as the
        // tone-map — FP16 source -> SDR intermediate -> VideoProcessor — but with
        // the SDR curve (clamp + sRGB OETF, no roll-off).
        const bool sdrScrgb = (capMode == OdCaptureMode::SdrScrgb);
        const bool toneMap = (capMode == OdCaptureMode::HdrToneMap) || sdrScrgb;
        const bool nativeHdr = (capMode == OdCaptureMode::HdrNative);
        // The caller committed BT.2020/PQ colour metadata + 10-bit for a native
        // session; if the display instead delivered a surface that resolves to
        // something else, the tags would not match the pixels. Fail explicitly
        // rather than encode a mislabelled stream. Two distinct causes reach this
        // branch: the display never delivered a native-HDR-capable surface (a real
        // duplication/capability limitation), or the display's own HDR toggle was
        // switched off after expectNativeHdr was decided at session start (a live
        // state change, not a capability gap). odSrc.HdrActive() distinguishes
        // them: Open() AND Reopen() both re-read it live, so by the time the first
        // frame is finally negotiated (possibly after one or more start-hold
        // Reopen() retries) it reflects the display's CURRENT HDR state, while
        // expectNativeHdr stays frozen from the moment the session opened OD.
        if (expectNativeHdr && !nativeHdr) {
            char fmtBuf[32];
            std::ostringstream err;
            if (!odSrc.HdrActive()) {
                err << "DXGI OD: HDR was turned off on this display after the recording "
                    << "started, before the first frame could be captured; the session's "
                    << "already-committed HDR10 colour metadata no longer matches the "
                    << "desktop. Stop and restart the recording. preInit={" << diag.str() << "}";
            } else {
                err << "DXGI OD: native HDR10 output was configured but the display delivered a "
                    << OdCaptureFormatName(rawDesc.Format, fmtBuf, sizeof(fmtBuf))
                    << " surface that cannot carry it (Advanced-Color duplication unavailable). "
                    << "preInit={" << diag.str() << "}";
            }
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
            // Shared with the WGC HDR init path — see DemoteToneMapFormatIfUnsupportedVpInput.
            if (toneMap) {
                vpInputFormat = DemoteToneMapFormatIfUnsupportedVpInput(videoEnum.get(), vpInputFormat);
                toneMapIntermediateFormat = vpInputFormat;
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
        hdrToneMapSdrSource = sdrScrgb;
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
        // The provider delivers a converted BGRA frame (source pixel format ->
        // BGRA, e.g. NV12/MJPEG decode). Time that CPU conversion+copy from the
        // engine's side; the upload of camBgra to the GPU is part of the composite
        // pass measured by compositeGpuTimer.
        const auto cam_t0 = std::chrono::steady_clock::now();
        uint64_t camGeneration = 0;
        const bool gotCam = m_state.config.webcam.frame_provider->TryGetFrame(camW, camH, camBgra, camGeneration);
        const auto cam_t1 = std::chrono::steady_clock::now();
        if (gotCam) {
            m_state.diagnostics.OnWebcamConvert(cam_t1,
                                                std::chrono::duration<double, std::milli>(cam_t1 - cam_t0).count());
        }
        if (gotCam && (!haveWebcamFrameGeneration || camGeneration != lastWebcamFrameGeneration)) {
            haveWebcamFrameGeneration = true;
            lastWebcamFrameGeneration = camGeneration;
            ++visualGenerations.webcam;
            m_state.diagnostics.OnWebcamGenerationChanged();
        }
        if (!gotCam) {
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

        const CursorSpriteClip clip =
            ClipCursorSprite(odCursorPosX, odCursorPosY, static_cast<int32_t>(odCursorShapeInfo.Width),
                             static_cast<int32_t>(odCursorShapeInfo.Height), static_cast<int32_t>(compositorWidth),
                             static_cast<int32_t>(compositorHeight));
        if (!clip.visible)
            return true;

        const uint32_t pitch =
            odCursorShapeInfo.Pitch != 0 ? odCursorShapeInfo.Pitch : static_cast<uint32_t>(odCursorShapeInfo.Width * 4);
        const size_t minBytes = static_cast<size_t>(odCursorShapeInfo.Height - 1) * pitch +
                                static_cast<size_t>(odCursorShapeInfo.Width) * 4;
        if (odCursorBitmap.size() < minBytes) {
            return true;
        }

        odCursorUploadBgra.resize(static_cast<size_t>(clip.w) * clip.h * 4);
        for (int32_t row = 0; row < clip.h; ++row) {
            const size_t srcOff =
                static_cast<size_t>(clip.bitmap_off_y + row) * pitch + static_cast<size_t>(clip.bitmap_off_x) * 4;
            const uint8_t* srcRow = odCursorBitmap.data() + srcOff;
            uint8_t* dstRow = odCursorUploadBgra.data() + static_cast<size_t>(row) * clip.w * 4;
            std::memcpy(dstRow, srcRow, static_cast<size_t>(clip.w) * 4);
        }

        WebcamPixelRect rect;
        rect.x = clip.x;
        rect.y = clip.y;
        rect.w = clip.w;
        rect.h = clip.h;

        std::string compErr;
        if (!gpuCompositor.DrawCursor(odCursorUploadBgra.data(), clip.w, clip.h, rect, compErr)) {
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

        const int32_t cx = ScaleCoordinateToSource(cursorInfo.ptScreenPos.x - wgcCursorBounds.left,
                                                   static_cast<int32_t>(sourceWidth), boundsW) -
                           wgcCursorBitmap.hotspot_x;
        const int32_t cy = ScaleCoordinateToSource(cursorInfo.ptScreenPos.y - wgcCursorBounds.top,
                                                   static_cast<int32_t>(sourceHeight), boundsH) -
                           wgcCursorBitmap.hotspot_y;
        const CursorSpriteClip clip =
            ClipCursorSprite(cx, cy, wgcCursorBitmap.width, wgcCursorBitmap.height, static_cast<int32_t>(sourceWidth),
                             static_cast<int32_t>(sourceHeight));
        if (!clip.visible) {
            return true;
        }

        wgcCursorUploadBgra.resize(static_cast<size_t>(clip.w) * clip.h * 4);
        for (int32_t row = 0; row < clip.h; ++row) {
            const size_t srcOff =
                (static_cast<size_t>(clip.bitmap_off_y + row) * wgcCursorBitmap.width + clip.bitmap_off_x) * 4u;
            const uint8_t* srcRow = wgcCursorBitmap.bgra.data() + srcOff;
            uint8_t* dstRow = wgcCursorUploadBgra.data() + static_cast<size_t>(row) * clip.w * 4u;
            std::memcpy(dstRow, srcRow, static_cast<size_t>(clip.w) * 4u);
        }

        WebcamPixelRect rect;
        rect.x = clip.x;
        rect.y = clip.y;
        rect.w = clip.w;
        rect.h = clip.h;

        std::string compErr;
        if (!gpuCompositor.DrawCursor(wgcCursorUploadBgra.data(), clip.w, clip.h, rect, compErr)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoCapture, "GPU WGC cursor composite: " + compErr);
            return false;
        }
        return true;
    };

    // scRGB FP16 -> SDR BT.709 for an HDR desktop. Runs once per emitted frame,
    // replacing the FP16 capture texture with an SDR BGRA8 surface before
    // compositing/VideoProcessor. A no-op (returns source unchanged) on SDR
    // desktops. Returns nullptr and records the failure if the pass fails.
    // Bring up the GPU-execution-time profilers now the device exists. Inert (and
    // silently skipped at every call site) if the adapter cannot create timestamp
    // queries — measurement is best-effort and never a hard dependency.
    compositeGpuTimer.Init(d3dDevice.get());
    tonemapGpuTimer.Init(d3dDevice.get());
    vpbltGpuTimer.Init(d3dDevice.get());

    auto toneMapIfHdr = [&](ID3D11Texture2D* source) -> ID3D11Texture2D* {
        if (!hdrToneMapActive || source == nullptr) {
            return source;
        }
        std::string tmErr;
        tonemapGpuTimer.Begin(d3dContext.get());
        const bool ok = hdrToneMapper.Convert(source, hdrSdrTex.get(), tmErr);
        tonemapGpuTimer.End(d3dContext.get());
        if (const auto gpu_ms = tonemapGpuTimer.Poll(d3dContext.get())) {
            m_state.diagnostics.OnHdrTonemapGpuTime(std::chrono::steady_clock::now(), *gpu_ms);
        }
        if (!ok) {
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
        // Bracket the whole composite pass for real GPU-execution time. End() is
        // issued on every return path so a failed draw never leaves the query pair
        // (and thus the disjoint query) unbalanced.
        compositeGpuTimer.Begin(d3dContext.get());
        auto endCompositeTimer = [&]() {
            compositeGpuTimer.End(d3dContext.get());
            if (const auto gpu_ms = compositeGpuTimer.Poll(d3dContext.get())) {
                m_state.diagnostics.OnCompositionGpuTime(std::chrono::steady_clock::now(), *gpu_ms);
            }
        };
        if (!gpuCompositor.BeginFrame(source, compErr)) {
            endCompositeTimer();
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoCapture, "GPU compositor begin: " + compErr);
            return nullptr;
        }
        if (!drawWebcamGpu(overlay)) {
            endCompositeTimer();
            return nullptr;
        }
        if (!drawCursorGpu()) {
            endCompositeTimer();
            return nullptr;
        }
        if (!drawWin32CursorGpu()) {
            endCompositeTimer();
            return nullptr;
        }
        endCompositeTimer();
        return gpuCompositor.Result();
    };

    // --- WGC frame pool and session (Window-only path) ---
    bool sourceLost = false;
    // OD recovery hold state (Recover branch, i.e. DXGI_ERROR_ACCESS_LOST). While
    // holding, the encode loop keeps emitting at CFR cadence (the last frame is
    // held, not black) and retries Reopen() on a throttle instead of blocking —
    // blocking froze the whole recording for the gap and never recovered a monitor
    // that returned with a new HMONITOR. Cleared when Reopen() succeeds.
    bool odHolding = false;
    auto odLastReopenAttempt = std::chrono::steady_clock::now();
    // How long to wait between Reopen() attempts after a recoverable OD acquire
    // loss. The retry itself is UNBOUNDED (std::nullopt budget below): the output
    // can be absent briefly (a mode/topology flip, EDID/HPD re-negotiation blacking
    // the desktop for a moment) or indefinitely (a display left switched off), and
    // the recording keeps going — holding the last captured frame — until it
    // returns or the user stops. 250 ms is well inside the norm (OBS re-creates its
    // duplicator at most every 3 s); polling faster gains nothing because the OS
    // topology renegotiation dominates the recovery latency.
    constexpr auto kOdReopenPollDelay = std::chrono::milliseconds{250};
    // React to a DXGI OD TryAcquireFrame() failure HRESULT. ACCESS_LOST is a
    // transient loss (mode/topology change, device alive): rebuild the duplication
    // and continue the SAME encode session and output file, leaving a gap during
    // which the last captured frame is held (frozen, not black). DEVICE_REMOVED /
    // any unexpected HRESULT is unrecoverable: record a failure carrying the HRESULT
    // so it reaches the app log ([record.failure]) — the engine's logging::log
    // only feeds the ring buffer. Either way the segment is still finalised, so
    // footage is kept.
    const auto HandleOdAcquireFailure = [&](HRESULT hr) {
        switch (ClassifyOdAcquireFailure(hr)) {
        case OdAcquireFailAction::Idle:
            break; // no frame available this poll — normal
        case OdAcquireFailAction::Recover:
            // Non-blocking: enter the holding state. The old inner poll loop blocked
            // here for the entire gap, so the encode loop never ran — the timeline
            // froze (recorded duration stopped at the moment of loss regardless of
            // gap length) and a monitor that returned with a new HMONITOR was never
            // re-resolved. Instead the outer encode loop keeps emitting at CFR
            // cadence (holding the last frame) and retries Reopen() on a throttle
            // (top of the encode loop) until the output returns or the user stops.
            // Unbounded by design; DEVICE_REMOVED / unexpected HRESULTs still end the
            // recording via the Fail branch below.
            odHolding = true;
            break;
        case OdAcquireFailAction::Fail: {
            char msg[96];
            snprintf(msg, sizeof(msg), "DXGI OD frame acquire lost: hr=0x%08X", static_cast<unsigned>(hr));
            m_state.RecordFailure(hr, ErrorPhase::VideoCapture, msg);
            break;
        }
        }
    };

    // Periodic HDR-state re-check: hdr_active is otherwise resolved once above and
    // never revisited, so a Windows HDR toggle mid-recording silently kept encoding
    // under stale tone-map/native-HDR10 decisions — the last pre-toggle frame
    // duplicated forever with only a one-time WARN log. Cleanly stop instead:
    // RecordFailure() finalizes the file normally (same as any other capture
    // failure) so the user can start a fresh recording with facts resolved anew.
    // Polled rather than acted on every frame — an interactive HDR toggle doesn't
    // need sub-second detection, and re-querying DXGI output desc every frame
    // would be wasted GPU-adjacent work in the hot loop.
    auto lastHdrCheckAt = std::chrono::steady_clock::now();
    constexpr auto kHdrCheckPollDelay = std::chrono::seconds{2};
    const auto CheckHdrStateChanged = [&]() {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastHdrCheckAt < kHdrCheckPollDelay) {
            return;
        }
        lastHdrCheckAt = now;
        HdrDisplayFacts freshFacts;
        if (!QueryDisplayHdrFacts(hdrCheckMonitor, freshFacts)) {
            return; // transient query failure — don't false-positive a stop
        }
        if (freshFacts.hdr_active != initialHdrActive) {
            m_state.RecordFailure(E_ABORT, ErrorPhase::VideoCapture,
                                  initialHdrActive ? "Windows HDR was turned off during recording"
                                                   : "Windows HDR was turned on during recording");
        }
    };

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
            return;
        }
    } // end if (!useOdCapture) — WGC session init

    // The WGC frame pool RECYCLES its surfaces: once a frame object is
    // released, WGC renders future frames into the same texture. Anything the
    // encode loop keeps beyond the lifetime of the frame object must therefore
    // be a copy into an engine-owned texture — the same invariant the preview
    // producer follows (WgcSourceProducer, ADR 0041). One persistent texture is
    // enough for seed/pending/held alike: they only ever mean "the latest
    // captured frame", exactly like the OD path's persistent odCapturedTex.
    // Sized from the SESSION's source size (the pool was created at exactly that
    // size and its surfaces never change size — a resize is reported by
    // ContentSize instead) and created lazily from the first frame's format;
    // returns nullptr after recording the failure.
    winrt::com_ptr<ID3D11Texture2D> wgcCapturedTex;
    auto copyWgcFrame = [&](ID3D11Texture2D* rawTex) -> ID3D11Texture2D* {
        D3D11_TEXTURE2D_DESC rawDesc{};
        rawTex->GetDesc(&rawDesc);
        // The pool surface must be the session's source size, or CopyResource
        // below would silently no-op (mismatched dimensions) and the compositor
        // would read a stale/empty texture. Kept as an explicit check — the same
        // one the OD path applies to every acquired frame — so any divergence
        // surfaces as the honest size failure instead of an opaque black frame.
        if (rawDesc.Width != sourceWidth || rawDesc.Height != sourceHeight) {
            std::ostringstream err;
            err << "capture source size changed during session from " << sourceWidth << "x" << sourceHeight << " to "
                << rawDesc.Width << "x" << rawDesc.Height << "; restart recording to reconfigure encoder";
            m_state.RecordFailure(E_INVALIDARG, ErrorPhase::VideoCapture, err.str());
            return nullptr;
        }
        if (wgcCapturedTex == nullptr) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = sourceWidth;
            desc.Height = sourceHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = rawDesc.Format;
            desc.SampleDesc = {1, 0};
            desc.Usage = D3D11_USAGE_DEFAULT;
            // Same bind rule as odCapturedTex: tone-map / native-HDR shader
            // passes sample the capture as an SRV; the plain SDR path feeds
            // the VideoProcessor from a render-target texture.
            desc.BindFlags =
                (hdrToneMapActive || hdrNativeActive) ? D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_RENDER_TARGET;
            const HRESULT copyHr = d3dDevice->CreateTexture2D(&desc, nullptr, wgcCapturedTex.put());
            if (FAILED(copyHr)) {
                char buf[80];
                snprintf(buf, sizeof(buf), "CreateTexture2D(wgcCapturedTex) failed 0x%08lX",
                         static_cast<unsigned long>(copyHr));
                m_state.RecordFailure(copyHr, ErrorPhase::VideoCapture, buf);
                return nullptr;
            }
        }
        d3dContext->CopyResource(wgcCapturedTex.get(), rawTex);
        return wgcCapturedTex.get();
    };

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

        // OD start-hold recovery: a game entering exclusive fullscreen exactly as
        // recording starts throws ACCESS_LOST before the first frame. Rather than
        // failing the start, enter a bounded hold and poll Reopen() under
        // kOdStartHoldBudget (the same recovery the drain uses mid-session, but
        // bounded — the user is waiting on "Preparing"). While holding, the 5 s
        // first-frame guard is suspended (FirstFrameWaitStep); after a successful
        // reopen the deadline anchor (tStart) restarts, giving a fresh 5 s window.
        bool odStartHolding = false;
        auto odStartLossBegan = std::chrono::steady_clock::now();
        auto odStartLastReopen = odStartLossBegan;

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
            const FirstFrameWaitAction waitAction = FirstFrameWaitStep(odStartHolding, elapsed, kTimeoutSec);
            if (waitAction == FirstFrameWaitAction::TimeoutFail) {
                if (!useOdCapture) {
                    // Honest cause: name the window state and the exclusive-fullscreen
                    // possibility instead of a bare "timeout" (a window in FSE cannot
                    // be captured by WGC at all — record the monitor instead).
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                             "WGC: no frame within 5 s (window minimized=%d cloaked=%d). A window in exclusive "
                             "fullscreen cannot be captured — switch the game to borderless, or record the "
                             "monitor instead.",
                             windowMinimized ? 1 : 0, windowCloaked ? 1 : 0);
                    logging::LogField fields[] = {{"backend", "wgc"},
                                                  {"reason", "first_frame_timeout"},
                                                  {"window_minimized", windowMinimized ? "true" : "false"},
                                                  {"window_cloaked", windowCloaked ? "true" : "false"}};
                    logging::log(logging::LogLevel::Warn, "video_thread", "WGC first-frame timeout",
                                 std::span<const logging::LogField>(fields, std::size(fields)));
                    m_state.RecordFailure(HRESULT_FROM_WIN32(ERROR_TIMEOUT), ErrorPhase::VideoCapture, buf);
                    if (captureSession != nullptr)
                        captureSession.Close();
                    if (framePool != nullptr)
                        framePool.Close();
                } else {
                    m_state.RecordFailure(HRESULT_FROM_WIN32(ERROR_TIMEOUT), ErrorPhase::VideoCapture,
                                          "DXGI OD: timeout waiting for first frame (5 s)");
                }
                return;
            }
            if (waitAction == FirstFrameWaitAction::HoldStep) {
                // Start-hold active: drive Reopen() under the bounded budget. The
                // 5 s guard is suspended so the deadline belongs to the reopen budget.
                const auto now = std::chrono::steady_clock::now();
                bool reopened = false;
                if (now - odStartLastReopen >= kOdReopenPollDelay) {
                    odStartLastReopen = now;
                    std::string reopenErr;
                    reopened = odSrc.Reopen(d3dDevice.get(), reopenErr);
                }
                const auto sinceLoss = std::chrono::duration_cast<std::chrono::milliseconds>(now - odStartLossBegan);
                const OdReopenDecision dec =
                    DecideOdReopen(reopened, sinceLoss, kOdStartHoldBudget, kOdReopenPollDelay);
                if (dec.action == OdReopenAction::Continue) {
                    odStartHolding = false;
                    QueryPerformanceCounter(&tStart); // fresh first-frame deadline
                } else if (dec.action == OdReopenAction::GiveUp) {
                    m_state.RecordFailure(DXGI_ERROR_ACCESS_LOST, ErrorPhase::VideoCapture,
                                          "DXGI OD: display did not return within 15 s of an access loss at start "
                                          "(a fullscreen or display-mode change was likely in progress).");
                    return;
                }
                Sleep(10); // keep stop-request latency low while holding
                continue;
            }

            if (!useOdCapture && sourceLost) {
                m_state.RecordFailure(E_ABORT, ErrorPhase::VideoCapture, "WGC: source lost before first frame");
                if (captureSession != nullptr)
                    captureSession.Close();
                if (framePool != nullptr)
                    framePool.Close();
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
                    // A fullscreen/mode transition is in progress. Do not fail the
                    // start: enter the bounded start-hold and poll Reopen() (handled
                    // by the HoldStep branch above) until the display returns or the
                    // 15 s budget is exhausted.
                    if (!odStartHolding) {
                        odStartHolding = true;
                        odStartLossBegan = std::chrono::steady_clock::now();
                        odStartLastReopen = odStartLossBegan;
                        logging::log(logging::LogLevel::Info, "video_thread",
                                     "DXGI OD access lost before first frame — entering bounded start-hold", {});
                    }
                }
                // DXGI_ERROR_WAIT_TIMEOUT: no frame yet, loop again
            } else {
                try {
                    auto frame = framePool.TryGetNextFrame();
                    if (frame != nullptr) {
                        // Copy the frame out of the pool as the encode-loop
                        // seed (see seedWgcTex above) — the pool recycles the
                        // surface, so borrowing it would let WGC render future
                        // frames into the held seed. Only a frame whose visible
                        // content matches the configured capture size may seed
                        // the encoder (the pool surface itself never changes
                        // size — ContentSize is the real signal); on mismatch
                        // the drain loop reports the honest size-changed
                        // failure on the next frame.
                        const auto seedContent = frame.ContentSize();
                        if (seedContent.Width == static_cast<int32_t>(sourceWidth) &&
                            seedContent.Height == static_cast<int32_t>(sourceHeight)) {
                            auto surface = frame.Surface();
                            auto access =
                                surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                            winrt::com_ptr<ID3D11Texture2D> tex;
                            if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put())))) {
                                ID3D11Texture2D* const copied = copyWgcFrame(tex.get());
                                if (copied == nullptr) {
                                    // copyWgcFrame already recorded the failure.
                                    if (captureSession != nullptr)
                                        captureSession.Close();
                                    if (framePool != nullptr)
                                        framePool.Close();
                                    return;
                                }
                                seedWgcTex.copy_from(copied);
                            }
                        }
                        gotFirst = true;
                    } else {
                        Sleep(1);
                    }
                } catch (const winrt::hresult_error& e) {
                    // Without this the real cause would be masked: the loop would
                    // keep retrying until the 5 s guard expired and then report a
                    // first-frame timeout for a device that was already gone.
                    HandleWgcAcquireException(m_state, e.code().value, "first frame", sourceLost);
                } catch (...) {
                    HandleWgcUnknownException(m_state, "first frame", sourceLost);
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
        // Probe the VP input support for the chosen tone-map intermediate before
        // using it. The OD path performs this same check as part of its
        // first-frame negotiation (see the vpInputSupported lambda above); WGC
        // has no equivalent negotiation step, so without this check a driver
        // that rejects R10G10B10A2 as a VideoProcessor input would fail
        // CreateVideoProcessorInputView on every tick with no recorded failure
        // — frames dropping silently instead of falling back to BGRA8.
        toneMapIntermediateFormat = DemoteToneMapFormatIfUnsupportedVpInput(videoEnum.get(), toneMapIntermediateFormat);

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
            if (!useOdCapture) {
                if (captureSession != nullptr)
                    captureSession.Close();
                if (framePool != nullptr)
                    framePool.Close();
            }
            return;
        }
        if (!hdrToneMapper.Init(d3dDevice.get(), d3dContext.get(), sourceWidth, sourceHeight, hdrPeakScale,
                                hdrToneMapSdrSource, tmErr, hdrPaperWhiteScale)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::Prepare, "HDR tone-map init: " + tmErr);
            if (!useOdCapture) {
                if (captureSession != nullptr)
                    captureSession.Close();
                if (framePool != nullptr)
                    framePool.Close();
            }
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
            if (!useOdCapture) {
                if (captureSession != nullptr)
                    captureSession.Close();
                if (framePool != nullptr)
                    framePool.Close();
            }
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
    // The PTS of the specific frame whose submission consumes the forced-
    // IDR request — set by maybeArmSplit, consumed by ShouldEmitSplitSentinel
    // in routePacket. Binds the sentinel to that exact frame instead of
    // whichever keyframe happens to route next, which is required once
    // ReapCompleted can surface earlier, already-in-flight packets in the same
    // iteration (see split_sentinel_policy.h).
    uint64_t split_forced_pts_ns = 0;
    SplitTriggerSource split_armed_trigger = SplitTriggerSource::ManualButton;
    // Whether an additional trigger arriving while `split_armed` is already
    // pending has been logged for the current boundary (see maybeArmSplit).
    // Reset every time a new boundary arms so the next coalesced trigger, if
    // any, is reported too instead of only ever the first one.
    bool split_armed_secondary_logged = false;

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
    // 4:4:4: after the VideoProcessorBlt produced the geometry-corrected BGRA
    // intermediate for `slot`, convert it into the AYUV encode texture the encoder
    // registered. No-op (returns true) for the 4:2:0 path, which keeps writing the
    // NV12/P010 texture directly. Called right after each successful Blt, before the
    // reference-save and encode.
    auto finalizeEncodeSurface = [&](int32_t slot, std::string& err) -> bool {
        if (!chroma444)
            return true;
        return rgbToAyuv.Convert(vpRgbTextures[slot].get(), nv12Textures[slot].get(), err);
    };

    auto maybeArmSplit = [&](uint64_t pts_ns) {
        const uint64_t seq = m_state.split_request_seq.load();
        const bool manual = (seq != split_last_seq);
        const bool automatic = (pts_ns >= next_auto_threshold_ns);
        if (split_armed) {
            // A boundary is already pending (forced IDR requested, waiting for the
            // next keyframe to land). A second trigger landing in this window still
            // coalesces into the same boundary -- one forced IDR is correct, the
            // segment split itself is unaffected -- but it previously vanished from
            // the log with no trace. Note it once per pending boundary so a manual
            // split and an automatic split that land close together are both
            // visible instead of only whichever one armed first.
            if ((manual || automatic) && !split_armed_secondary_logged) {
                split_armed_secondary_logged = true;
                logging::LogField fields[] = {{"segment_index", std::to_string(current_segment_index + 1u)},
                                              {"trigger", manual ? "manual" : "automatic"},
                                              {"session_pts_ms", std::to_string(pts_ns / 1000000ULL)}};
                logging::log(logging::LogLevel::Info, "video_thread",
                             "additional split trigger coalesced into pending boundary",
                             std::span<const logging::LogField>(fields, std::size(fields)));
            }
            return; // a boundary is already pending; coalesce further requests
        }
        if (!manual && !automatic)
            return;
        split_last_seq = seq; // consume manual requests up to here (coalesced)
        split_armed = true;
        split_forced_pts_ns = pts_ns; // this call's frame is the one that will carry FORCEIDR
        split_armed_secondary_logged = false;
        split_armed_trigger = manual ? static_cast<SplitTriggerSource>(m_state.split_last_trigger.load())
                                     : SplitTriggerSource::AutomaticDuration;
        encoder->RequestKeyframe();
        m_state.diagnostics.OnForcedKeyframe();
        m_state.diagnostics.SetSplitPending(true);
        // Both conditions can be true in the same call (e.g. a seq-based split --
        // manual button, hotkey, or automatic size-split -- coincides with the
        // independent duration timer crossing its threshold); name both triggers
        // instead of letting the ternary silently pick one.
        const char* trigger_label = (manual && automatic) ? "manual+automatic" : (manual ? "manual" : "automatic");
        logging::LogField fields[] = {{"segment_index", std::to_string(current_segment_index + 1u)},
                                      {"trigger", trigger_label},
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
            if (m_state.config.video_codec == VideoCodec::H264 && !h264CodecPrivateReady) {
                std::vector<uint8_t> spsPps;
                if (annexb::ExtractH264SpsAndPps(pkt.bytes.data(), pkt.bytes.size(), spsPps)) {
                    std::lock_guard lk(m_state.premux_mutex);
                    m_state.codec_private.h264_sps_pps = std::move(spsPps);
                    m_state.codec_private.h264_ready = true;
                    h264CodecPrivateReady = true;
                    m_state.premux_cv.notify_all();
                }
            } else if (m_state.config.video_codec == VideoCodec::Hevc && !hevcCodecPrivateReady) {
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
            } else if (m_state.config.video_codec == VideoCodec::Av1 && !av1CodecPrivateReady) {
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
                std::unique_lock mlk(m_state.mux_mutex);
                // Bounded steady-state queue: wait for room BEFORE the split
                // sentinel so the sentinel and its keyframe stay adjacent; block
                // briefly, then fail cleanly — never drop frames or grow without
                // limit.
                if (!m_state.WaitForMuxQueueSpace(mlk)) {
                    mlk.unlock();
                    m_state.RecordFailure(E_OUTOFMEMORY, ErrorPhase::Mux,
                                          "Mux queue limit exceeded: the output destination "
                                          "cannot keep up with the recording");
                    return false;
                }
                if (ShouldEmitSplitSentinel(split_armed, split_forced_pts_ns, pkt.keyframe, pkt.pts_ns)) {
                    ++current_segment_index;
                    segment_start_session_pts_ns = pkt.pts_ns;
                    if (split_auto_enabled) {
                        next_auto_threshold_ns = segment_start_session_pts_ns + split_auto_interval_ns;
                    }
                    MuxItem split_item;
                    split_item.payload = SplitSentinel{current_segment_index, split_armed_trigger};
                    m_state.PushMuxItemLocked(std::move(split_item)); // sentinel: bypasses the bound
                    split_armed = false;
                }
                MuxItem mux_item;
                mux_item.payload = std::move(pkt);
                m_state.PushMuxItemLocked(std::move(mux_item));
            }
        }

        {
            std::lock_guard slk(m_state.stats_mutex);
            m_state.stats.video_frames_captured = videoFramesCaptured;
            m_state.stats.duplicated_video_frames = duplicatedFrames;
            m_state.stats.dropped_or_skipped_video_frames = droppedFrames + slotStallCount;
            m_state.stats.encoded_video_packets++;
            m_state.stats.video_bytes += pkt_bytes_count;
            // Publish the video media duration live (previously only at teardown) so
            // the duration-skew metric — video vs audio media time — is available to
            // live diagnostics, not just the final snapshot. lastVideoPts is the newest
            // emitted video PTS and is monotonic.
            m_state.stats.video_duration_ns = lastVideoPts;
        }
        return true;
    };

    // Report per-packet diagnostics (encode latency, order-validation
    // mismatch counters) — shared between the EncodeFrame submit path and
    // the ReapCompleted drain path below so both origins account
    // identically.
    auto reportPacketDiagnostics = [&](const EncodedVideoPacket& pkt, std::chrono::steady_clock::time_point now) {
        if (pkt.encode_latency_ms >= 0.0)
            m_state.diagnostics.OnEncodeLatency(now, pkt.encode_latency_ms);
        if (pkt.output_ts_mismatch)
            m_state.diagnostics.OnOutputTsMismatch();
        if (pkt.keyframe_prediction_mismatch)
            m_state.diagnostics.OnKeyframePredictionMismatch();
    };

    // Drain packets completed since the last EncodeFrame/ReapCompleted call:
    // async encoders may signal completions of earlier submissions here
    // rather than returning them inline from EncodeFrame. Called once per
    // outer loop iteration with wait_head_ms=0 (non-blocking) so freed
    // output/input slots are visible before this iteration's tick-emit, and
    // again with a bounded wait when AcquireFreeSlot finds no free slot,
    // before that counts as a backpressure drop. Sync encoders (and the sync
    // fallback) no-op here — they already returned their packet(s) inline
    // from EncodeFrame.
    auto reapAndRoute = [&](uint32_t wait_head_ms) -> bool {
        std::vector<EncodedVideoPacket> reaped;
        std::string reapErr;
        if (!encoder->ReapCompleted(reaped, reapErr, wait_head_ms)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoEncode, "NVENC async reap: " + reapErr);
            return false;
        }
        const auto reap_t = std::chrono::steady_clock::now();
        for (EncodedVideoPacket& pkt : reaped) {
            reportPacketDiagnostics(pkt, reap_t);
            if (!routePacket(std::move(pkt)))
                return false;
        }
        return true;
    };

    // Bounded wait for ReapCompleted when a slot acquisition fails before
    // counting it as a backpressure drop. Kept short: this blocks the outer
    // loop, and a real encoder stall is better served by the existing
    // sustained-lag resync than by a longer per-tick wait here.
    constexpr uint32_t kSlotWaitMs = 4;

    // --- Frame snapshot (CaptureFrame) ---
    // Lazily created staging texture (USAGE_STAGING + CPU_ACCESS_READ) for the
    // NV12/P010/AYUV→BGRA readback (format follows encodeFormat).
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

    // Perform a one-shot encode-surface→BGRA readback (NV12/P010/AYUV) if a
    // snapshot is pending. Normal recording calls this on a real frame. Pause
    // calls it with the last completed real-frame slot, which remains owned by
    // this VideoThread and is not overwritten while the encode loop is paused.
    // NOTE: The Map(D3D11_MAP_READ) call below provides the minimal synchronization point;
    //       it stalls the thread until the GPU completes the CopyResource, typically <1 ms.
    auto performSnapshotIfRequested = [&](ID3D11Texture2D* source) {
        if (!m_state.snapshot_requested.load() || source == nullptr)
            return;

        // Lazily allocate the staging texture on first use. Matches encodeFormat
        // (NV12 for 8-bit, P010 for 10-bit, AYUV for 4:4:4) so every encode
        // surface layout can snapshot.
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

        // Copy the final encode-ready frame (NV12/P010/AYUV) to the staging texture.
        d3dContext->CopyResource(snapshotStagingTex.get(), source);

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

        std::vector<uint8_t> bgra;
        bgra.resize(static_cast<size_t>(encodeWidth) * encodeHeight * 4u);

        // The encode-surface layouts are mutually exclusive:
        //   - 4:4:4 -> AYUV, single plane, 4 bytes/pixel [V, U, Y, A]; always
        //     8-bit SDR (native HDR10 snaps chroma to 4:2:0, see
        //     ApplyHdr10NativeEncode), so it never overlaps the HDR branch.
        //   - 4:2:0 -> planar NV12 (8-bit) / P010 (10-bit), where native HDR10
        //     P010 holds PQ/BT.2020 and needs the tone-mapping decode.
        if (chroma444) {
            // Decode the packed AYUV encode surface with the exact inverse of
            // the RGB->AYUV shader, using the color space the session actually
            // configured (see color_metadata.h / RecorderConfig::color).
            PackedAyuvFrame ayuvSrc;
            ayuvSrc.data = static_cast<const uint8_t*>(mapped.pData);
            ayuvSrc.stride_bytes = mapped.RowPitch;
            ayuvSrc.width = encodeWidth;
            ayuvSrc.height = encodeHeight;

            YuvToBgraParams colorParams;
            colorParams.matrix = m_state.config.color.matrix;
            colorParams.range = m_state.config.color.range;
            ConvertAyuvToBgra(ayuvSrc, colorParams, bgra.data(), encodeWidth * 4u);
        } else {
            // NV12/P010 layout:
            //   Plane Y:  rows 0 .. height-1, each row = RowPitch bytes
            //   Plane UV: rows height .. height+height/2-1, interleaved U V, same RowPitch
            const auto* y_plane = static_cast<const uint8_t*>(mapped.pData);
            const auto* uv_plane = y_plane + static_cast<size_t>(mapped.RowPitch) * encodeHeight;

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
                // Convert using the color space the session actually configured
                // (see color_metadata.h / RecorderConfig::color) rather than a
                // hard-coded assumption — the encoder always writes BT.709-tagged
                // output with the user-selected range, so the readback must match.
                YuvToBgraParams colorParams;
                colorParams.matrix = m_state.config.color.matrix;
                colorParams.range = m_state.config.color.range;
                ConvertYuv420ToBgra(yuvSrc, colorParams, bgra.data(), encodeWidth * 4u);
            }
        }

        d3dContext->Unmap(snapshotStagingTex.get(), 0);

        if (pending_cb)
            pending_cb(true, encodeWidth, encodeHeight, std::move(bgra), {});
    };
    int32_t lastRealFrameSlot = -1;

    // --- Live WYSIWYG preview tap: shared GPU texture ---
    // The preview shows exactly what the encoder receives by sharing the
    // composited, pre-encode source frame with the preview renderer via an
    // NT-handle + keyed-mutex texture. Zero CPU copies; the encode path is never
    // stalled (0 ms keyed-mutex acquire, drop on contention). Zero cost when no
    // consumer registered the callback.
    //
    // The shared texture is created lazily from the FIRST tapped frame so it
    // matches that surface's exact format (B8G8R8A8 for SDR/tone-map, R10G10B10A2
    // for 10-bit, R16G16B16A16_FLOAT for native HDR10). The NT handle is handed
    // to the consumer once, on creation, together with the display transform the
    // consumer must apply (an FP16 scRGB tap is tone-mapped preview-side).
    //
    // The only untapped session is the already-PQ R10G10B10A2 native sub-path:
    // its surface is non-linear PQ with no linear intermediate, so the preview
    // keeps its own WGC capture there. See product-spec / KNOWN_LIMITATIONS.
    const PreviewTapPlan previewTapPlan = ResolvePreviewTapPlan(hdrNativeActive, hdrPqInputIsPq, hdrPeakScale);
    PreviewSharedTexture previewSharedTex;
    bool previewSharedInitFailed = false;
    bool previewTransportPoisoned = false;
    PreviewPublishGate previewGate(kPreviewMinIntervalNs);

    auto tapPreviewSource = [&](ID3D11Texture2D* vpInput, uint64_t pts_ns) {
        if (!m_state.preview_shared_handle_cb)
            return; // no consumer registered -- zero cost beyond this check
        if (previewSharedInitFailed || vpInput == nullptr || !previewTapPlan.tap_enabled)
            return;
        // Counted AFTER the cheap disqualifiers and BEFORE the gate: a zero here
        // says the tap never had a frame to offer, which is a different defect
        // from a tap that offered frames the transport then refused.
        m_state.diagnostics.OnPreviewTapFrameSeen();
        if (!previewGate.ShouldPublish(pts_ns))
            return; // throttle to ~30 Hz
        m_state.diagnostics.OnPreviewTapGatePass();

        if (!previewSharedTex.Valid()) {
            D3D11_TEXTURE2D_DESC srcDesc{};
            vpInput->GetDesc(&srcDesc);
            HANDLE ntHandle = nullptr;
            std::string err;
            if (!previewSharedTex.Create(d3dDevice.get(), srcDesc.Width, srcDesc.Height, srcDesc.Format, &ntHandle,
                                         err)) {
                previewSharedInitFailed = true;
                logging::log(logging::LogLevel::Warn, "video_thread",
                             "preview shared texture init failed; preview tap disabled for this session: " + err, {});
                return;
            }
            // One-shot: ownership of the NT handle transfers to the consumer, which
            // opens it on its render device and CloseHandle's it. Must return fast
            // and must not touch D3D on this (video) thread.
            m_state.preview_shared_handle_cb(ntHandle, srcDesc.Width, srcDesc.Height, previewTapPlan.desc);
            m_state.diagnostics.OnPreviewTapSharedTextureReady();
        }

        // Non-blocking publish of the composited frame (observation-only; the encode
        // path continues regardless of whether the preview picked up this frame).
        // Time the CPU submission cost of the copy; the display present itself runs
        // on the consumer's (UI) render thread, outside this engine path.
        const auto prev_t0 = std::chrono::steady_clock::now();
        const PreviewSharedTexture::PublishResult publish = previewSharedTex.TryPublish(d3dContext.get(), vpInput);
        const bool published = publish.published();
        const auto prev_t1 = std::chrono::steady_clock::now();
        m_state.diagnostics.OnPreviewCopy(prev_t1,
                                          std::chrono::duration<double, std::milli>(prev_t1 - prev_t0).count());
        m_state.diagnostics.OnPreviewTapPublish(publish.acquire, publish.released_ok);
        // Logged once per session, not per frame: an abandoned keyed mutex or a
        // failed release does not recover, so every later tick would repeat it.
        if (!previewTransportPoisoned && (publish.acquire == PreviewAcquireOutcome::Abandoned ||
                                          publish.acquire == PreviewAcquireOutcome::Failed || !publish.released_ok)) {
            previewTransportPoisoned = true;
            logging::log(logging::LogLevel::Warn, "video_thread",
                         std::string("preview transport failed and will not recover this session: ") +
                             (publish.acquire == PreviewAcquireOutcome::Abandoned ? "keyed mutex abandoned"
                              : publish.acquire == PreviewAcquireOutcome::Failed  ? "acquire failed"
                                                                                  : "release failed"),
                         {});
        }
        // Only a frame that reached the shared texture is worth waking the
        // consumer for. A contention drop means the consumer has not taken the
        // PREVIOUS frame yet, so its redraw is already pending — signalling it
        // again would add a render without adding a picture.
        if (published && m_state.preview_frame_published_cb) {
            m_state.preview_frame_published_cb();
            m_state.diagnostics.OnPreviewTapPublishedEdge();
        }
    };

    // Cache the VideoProcessor input view across ticks. The encode input handed to
    // VideoProcessorBlt is usually the SAME texture object every tick — the
    // compositor's stable Result() texture, or (still desktop / newest-at-tick) the
    // persistent capture texture — so recreating the input view per frame was pure
    // churn. The view is a light wrapper that reads the texture's current contents at
    // Blt time, so reusing it across re-blts is correct. Keyed by the texture pointer;
    // a new pointer (a phase-correct ring entry or a format/size change — distinct
    // texture objects; WGC frames land in one persistent copy texture) rebuilds it. Returns nullptr
    // if the view cannot be created (the caller then skips the tick, as before).
    winrt::com_ptr<ID3D11VideoProcessorInputView> cachedInputView;
    ID3D11Texture2D* cachedInputViewTex = nullptr;
    auto acquireInputView = [&](ID3D11Texture2D* vpInput) -> ID3D11VideoProcessorInputView* {
        if (cachedInputViewTex == vpInput && cachedInputView != nullptr)
            return cachedInputView.get();
        cachedInputView = nullptr;
        cachedInputViewTex = nullptr;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc{};
        ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivDesc.Texture2D.MipSlice = 0;
        ivDesc.Texture2D.ArraySlice = 0;
        winrt::com_ptr<ID3D11VideoProcessorInputView> view;
        HRESULT ivHr = videoDevice->CreateVideoProcessorInputView(vpInput, videoEnum.get(), &ivDesc, view.put());
        if (FAILED(ivHr) || view == nullptr)
            return nullptr;
        cachedInputView = view;
        cachedInputViewTex = vpInput;
        return cachedInputView.get();
    };

    if (m_state.config.cfr) {
        // ====================================================================
        // CFR path: QPC-driven scheduler — duplicate/drop to hit constant rate
        // ====================================================================

        // Reference encode texture for frame duplication (NV12 8-bit / P010 10-bit)
        winrt::com_ptr<ID3D11Texture2D> refNv12;
        bool refNv12Valid = false;
        VisualFrameKey refNv12Key{}; // what refNv12 currently contains, once refNv12Valid

        // Per-slot content tracking: what generation each NVENC ring slot
        // currently holds, so a duplicate tick can skip the CopyResource
        // entirely when the slot already contains refNv12Key (CV-RETAIN-004).
        // Sized to match the hardcoded NVENC ring size (nvenc_encoder.h,
        // NvencEncoder::m_slots) and this file's own nv12Textures array;
        // making that size dynamic is CV-PERF-007, out of scope here.
        std::array<VisualFrameKey, 8> slotContainedKey{};
        std::array<bool, 8> slotContainedValid{};

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

        // --- Sustained-encoder-lag resync (honest timeline) ---
        // The catch-up loop below emits at most kMaxCatchUpFrames per outer iteration
        // so a brief stall cannot burst the GPU. If the encoder is *persistently*
        // slower than real time, that cap is hit every iteration and the media clock
        // (cfr_frame_idx x frame_interval) falls ever further behind the wall clock —
        // the file would end with less video than audio, silently out of sync. When
        // the media clock has trailed the wall clock by more than one full catch-up
        // budget for kSustainedLagResyncTicks consecutive iterations, the timeline is
        // resynchronised: the frame indices that could never be emitted in real time
        // are skipped and counted as real (backpressure) drops so media time snaps
        // back to the wall clock instead of compressing it. The threshold is one
        // budget because a shorter lag is, by construction, recoverable within a
        // single catch-up iteration and needs no resync.
        constexpr int kSustainedLagResyncTicks = 3;
        int sustainedLagTicks = 0;

        // Seed the first WGC frame from the wait loop so a static window still
        // encodes from t=0 (WGC only delivers frames on repaint).
        winrt::com_ptr<ID3D11Texture2D> pendingWgcTex = std::move(seedWgcTex);
        // The seed is consumed. The drain path below tests seedWgcTex against
        // nullptr to decide whether it still has to be picked up, so say so
        // explicitly rather than leaning on the moved-from state.
        seedWgcTex = nullptr;
        // The last screen frame that was encoded, kept alive after it was consumed.
        // A still desktop produces no new frames, but the webcam keeps moving; the
        // held screen is what the live webcam gets composited onto. The OD path needs
        // no equivalent — odCapturedTex is persistent and already holds the last frame.
        winrt::com_ptr<ID3D11Texture2D> heldWgcTex;

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
        // Reused scratch for linearising the round-robin ring into ascending present
        // order each tick — hoisted out of the hot loop so the two vectors are
        // allocated once (reserved to the ring size) and only cleared per tick.
        std::vector<uint64_t> presentQpcsAscending;
        std::vector<size_t> liveIndexToRingSlot;
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
            presentQpcsAscending.reserve(ringN);
            liveIndexToRingSlot.reserve(ringN);
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

            CheckHdrStateChanged();
            if (m_state.stop_requested.load()) {
                break;
            }

            // OD recovery: while holding after a recoverable acquire loss, retry
            // Reopen() on a throttle rather than draining a dead/closed source. On
            // success the drain below resumes on the fresh duplication (same encode
            // session, same file); until then the loop still emits held frames.
            if (odHolding) {
                const auto reopen_now = std::chrono::steady_clock::now();
                if (reopen_now - odLastReopenAttempt >= kOdReopenPollDelay) {
                    odLastReopenAttempt = reopen_now;
                    std::string reopenErr;
                    if (odSrc.Reopen(d3dDevice.get(), reopenErr))
                        odHolding = false;
                }
            }

            const CaptureDrainStep drainStep = NextCaptureDrainStep(useOdCapture, odHolding);
            if (drainStep == CaptureDrainStep::DrainOd) {
                // DXGI OD: drain all available frames. Newest-at-tick copies into
                // odCapturedTex; phase-correct copies into the present-QPC ring.
                const auto acq_t0 = std::chrono::steady_clock::now();
                while (true) {
                    ID3D11Texture2D* rawTex = nullptr;
                    DXGI_OUTDUPL_FRAME_INFO info{};
                    HRESULT odHr = S_OK;
                    if (!odSrc.TryAcquireFrame(0, &rawTex, &info, &odHr)) {
                        // Previously only DXGI_ERROR_ACCESS_LOST set sourceLost; every
                        // other HRESULT (notably DXGI_ERROR_DEVICE_REMOVED) fell through
                        // this bare break with no source-loss, so the worker looped
                        // through a dead source until the fixed join budget detached it.
                        HandleOdAcquireFailure(odHr);
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
                    const OdAcquireKind acquireKind =
                        ClassifyOdAcquire(info.LastPresentTime.QuadPart != 0, info.LastMouseUpdateTime.QuadPart != 0,
                                          m_state.config.capture_cursor);
                    if (acquireKind == OdAcquireKind::Ignorable) {
                        rawTex->Release();
                        odSrc.ReleaseFrame();
                        if (diag_recording)
                            m_state.diagnostics.OnCursorOnlyCaptureEventIgnored();
                        continue;
                    }
                    if (acquireKind == OdAcquireKind::DesktopPresent) {
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
                            entry.presentQpc = static_cast<uint64_t>(info.LastPresentTime.QuadPart);
                            ringHead = (ringHead + 1) % captureRing.size();
                            phaseRingHasFrame = true;
                        } else {
                            d3dContext->CopyResource(odCapturedTex.get(), rawTex);
                        }
                        ++visualGenerations.screen;
                        if (diag_recording)
                            m_state.diagnostics.OnScreenGenerationChanged();
                    } else {
                        // CursorOnly: no desktop texture copy, no ring entry — the held
                        // frame (screen unchanged) gets recomposited with the new cursor
                        // by ShouldRecompositeHeldScreen instead (Task 6).
                        if (diag_recording)
                            m_state.diagnostics.OnPhaseRingCursorOnlyEventIgnored();
                    }
                    rawTex->Release();
                    if (info.PointerShapeBufferSize > 0 && m_state.config.capture_cursor) {
                        if (odSrc.GetFramePointerShape(&odCursorShapeInfo, odCursorBitmap)) {
                            odCursorShapeValid = true;
                            ++visualGenerations.cursor; // shape/bitmap changed
                        }
                    }
                    if (info.LastMouseUpdateTime.QuadPart != 0) {
                        const bool visibilityChanged = odCursorVisible != (info.PointerPosition.Visible != FALSE);
                        const bool positionChanged = odCursorPosX != info.PointerPosition.Position.x ||
                                                     odCursorPosY != info.PointerPosition.Position.y;
                        odCursorVisible = info.PointerPosition.Visible != FALSE;
                        odCursorPosX = info.PointerPosition.Position.x;
                        odCursorPosY = info.PointerPosition.Position.y;
                        if (visibilityChanged || positionChanged)
                            ++visualGenerations.cursor;
                    }
                    if (m_state.config.capture_cursor != lastCursorCaptureEnabled) {
                        lastCursorCaptureEnabled = m_state.config.capture_cursor;
                        ++visualGenerations.cursor;
                    }
                    odSrc.ReleaseFrame();
                    if (diag_recording && acquireKind == OdAcquireKind::DesktopPresent)
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
                    if (!usePhaseCorrect && acquireKind == OdAcquireKind::DesktopPresent) {
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
            } else if (drainStep == CaptureDrainStep::DrainWgc) {
                // WGC: drain frame pool — keep latest (always drain, even when paused)
                const auto acq_t0 = std::chrono::steady_clock::now();
                try {
                    // TryGetNextFrame hands back the OLDEST queued frame, and
                    // only the newest is ever encoded. Walk to the newest first
                    // and copy that one — copying every queued frame would burn
                    // a full-surface GPU copy per coalesced frame (the preview
                    // producer drains the same way, WgcSourceProducer::PollFrame).
                    // Every frame walked past still counts as captured and, if
                    // it displaced an unencoded one, as a coalesce drop.
                    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{nullptr};
                    for (;;) {
                        auto next = framePool.TryGetNextFrame();
                        if (next == nullptr)
                            break;
                        const bool diag_recording = !m_state.pause_requested.load();
                        if (diag_recording)
                            m_state.diagnostics.OnFrameCaptured();
                        if (frame != nullptr || pendingWgcTex != nullptr) {
                            ++droppedFrames;
                            if (diag_recording)
                                m_state.diagnostics.OnFrameDroppedCoalesced();
                        }
                        frame = next;
                    }
                    if (frame != nullptr) {
                        // A window resize does NOT resize the pool's surfaces:
                        // WGC keeps rendering the (new-size) content into a
                        // corner of the old-size surface, so the texture
                        // descriptor always matches the pool and can never
                        // signal the resize. The frame's ContentSize is what
                        // actually changes. The encoder and compositor are
                        // fixed at the session's source size, so a real change
                        // is an explicit failure (same contract as the OD
                        // path), never silent corner content.
                        const auto content = frame.ContentSize();
                        if (content.Width != static_cast<int32_t>(sourceWidth) ||
                            content.Height != static_cast<int32_t>(sourceHeight)) {
                            std::ostringstream err;
                            err << "capture source size changed during session from " << sourceWidth << "x"
                                << sourceHeight << " to " << content.Width << "x" << content.Height
                                << "; restart recording to reconfigure encoder";
                            m_state.RecordFailure(E_INVALIDARG, ErrorPhase::VideoCapture, err.str());
                            sourceLost = true;
                        } else {
                            auto surface = frame.Surface();
                            auto access =
                                surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                            winrt::com_ptr<ID3D11Texture2D> tex;
                            if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put())))) {
                                // Copy out of the pool while the frame object is
                                // still alive — the pool recycles this surface as
                                // soon as the frame is released (see wgcCapturedTex).
                                ID3D11Texture2D* const copied = copyWgcFrame(tex.get());
                                if (copied == nullptr) {
                                    // copyWgcFrame already recorded the failure.
                                    sourceLost = true;
                                } else {
                                    pendingWgcTex.copy_from(copied);
                                }
                            }
                        }
                    }
                } catch (const winrt::hresult_error& e) {
                    HandleWgcAcquireException(m_state, e.code().value, "CFR drain", sourceLost);
                } catch (...) {
                    HandleWgcUnknownException(m_state, "CFR drain", sourceLost);
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
                if (lastRealFrameSlot >= 0)
                    performSnapshotIfRequested(nv12Textures[static_cast<size_t>(lastRealFrameSlot)].get());
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

            // Drain any async completions before this iteration's tick-emit
            // so freed slots are available to the catch-up loop below.
            if (!reapAndRoute(0))
                goto end_encode_loop;

            // Sustained-encoder-lag resync: keep the media clock honest against the
            // wall clock (see the state declaration above). The lag is how far media
            // time (next_tick) trails the wall clock right now.
            const uint64_t lag100ns =
                (currentElapsed100ns > next_tick_100ns) ? (currentElapsed100ns - next_tick_100ns) : 0;
            if (lag100ns > kMaxCatchUpFrames * frame_interval_100ns) {
                ++sustainedLagTicks;
            } else {
                sustainedLagTicks = 0;
            }
            if (sustainedLagTicks >= kSustainedLagResyncTicks) {
                const uint64_t skip = ComputeCatchUpSkip(lag100ns, frame_interval_100ns, kMaxCatchUpFrames);
                if (skip > 0) {
                    cfr_frame_idx += skip;
                    next_tick_100ns += skip * frame_interval_100ns;
                    droppedFrames += skip;
                    for (uint64_t d = 0; d < skip; ++d)
                        m_state.diagnostics.OnFrameDroppedBackpressure();
                    logging::LogField fields[] = {{"skipped_frames", std::to_string(skip)},
                                                  {"lag_ms", std::to_string(lag100ns / 10000ULL)}};
                    logging::log(logging::LogLevel::Info, "video_thread",
                                 "sustained encoder lag: resynchronised the CFR timeline to the wall clock "
                                 "(skipped frames counted as drops)",
                                 std::span<const logging::LogField>(fields, std::size(fields)));
                }
                sustainedLagTicks = 0;
            }

            // Emit CFR frames while we're behind, capped at 1 second to avoid
            // burst workload after process suspension.
            uint64_t catchUpFrames = 0;
            while (currentElapsed100ns >= next_tick_100ns && catchUpFrames < kMaxCatchUpFrames) {
                // Whole-tick frame-time bracket (composite + tonemap + VP-Blt +
                // encode + route). Emitted only on the emit path below, so dropped
                // ticks are not counted as frame-time samples.
                const auto tick_t0 = std::chrono::steady_clock::now();
                const uint64_t pts_ns = cfr_frame_idx * frame_interval_ns;

                int32_t slot = encoder->AcquireFreeSlot();
                if (slot < 0) {
                    // No free input slot: give the async encoder a bounded chance to
                    // reap a completion and free one before counting a drop.
                    if (!reapAndRoute(kSlotWaitMs))
                        goto end_encode_loop;
                    slot = encoder->AcquireFreeSlot();
                }
                if (slot < 0) {
                    ++slotStallCount;
                    m_state.diagnostics.OnSlotStall();
                    m_state.diagnostics.OnFrameDroppedBackpressure();
                    break; // retry this tick next outer iteration once a slot is free
                }

                bool frameWritten = false;

                // Determine source texture for this tick
                ID3D11Texture2D* rawSourceTex = nullptr;
                if (usePhaseCorrect) {
                    // Linearise the round-robin ring to ascending (oldest -> newest)
                    // present order; ringHead points at the oldest physical slot.
                    // Reuses the hoisted scratch (allocated once, cleared per tick).
                    presentQpcsAscending.clear();
                    liveIndexToRingSlot.clear();
                    const size_t ringN = captureRing.size();
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
                        lastEmittedPresentQpc = presentQpcsAscending[dec.index];
                        // Consume the emitted entry and every skipped/older one so they
                        // are not re-selected or counted again as eviction drops.
                        for (auto& entry : captureRing) {
                            if (entry.presentQpc != 0 && entry.presentQpc <= lastEmittedPresentQpc)
                                entry.presentQpc = 0;
                        }
                        // odCapturedTex doubles as the held screen (see its declaration).
                        // The entry was consumed by the loop above, so rotating it in here
                        // hands the drain back a free slot of identical description.
                        AdoptEmittedAsHeldScreen(captureRing[liveIndexToRingSlot[dec.index]].tex, odCapturedTex);
                        rawSourceTex = odCapturedTex.get();
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

                // A screen capture only yields a frame when the screen changes, but the
                // webcam does not stop moving with it. Without this, a still desktop
                // duplicates the last composited frame and the picture-in-picture freezes
                // inside the recording. Composite the live webcam onto the held screen
                // instead and encode it as a real frame.
                //
                // OD recovery: during a hold the drain is skipped, so rawSourceTex is null
                // and the CFR duplicate path below re-emits the last frame (frozen) until
                // Reopen() succeeds. Re-compositing is forbidden there — it touches
                // display-tied GPU resources while the captured output is gone.
                ID3D11Texture2D* const heldScreenTex = useOdCapture ? odCapturedTex.get() : heldWgcTex.get();
                const VisualFrameKey currentVisualKey = MakeVisualFrameKey(visualGenerations);
                const bool cursorOverlayMoved =
                    !haveLastCompositedKey ||
                    currentVisualKey.cursor_generation != lastCompositedKey.cursor_generation ||
                    currentVisualKey.overlay_generation != lastCompositedKey.overlay_generation;
                const bool webcamMoved = m_state.SnapshotWebcamOverlay().enabled && webcamProviderAvailable &&
                                         (!haveLastCompositedKey ||
                                          currentVisualKey.webcam_generation != lastCompositedKey.webcam_generation);
                const bool dynamicOverlayChanged = cursorOverlayMoved || webcamMoved;
                if (ShouldRecompositeHeldScreen(rawSourceTex != nullptr, odHolding, dynamicOverlayChanged,
                                                heldScreenTex != nullptr)) {
                    rawSourceTex = heldScreenTex;
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
                        encoder->ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    if (useOdCapture) {
                        odCapturedTexValid = false;
                    } else {
                        heldWgcTex = std::move(pendingWgcTex);
                        // Consumed. The next iteration tests pendingWgcTex against
                        // nullptr to detect a fresh frame, so clear it explicitly
                        // instead of relying on the moved-from state.
                        pendingWgcTex = nullptr;
                    }

                    // Live WYSIWYG preview tap: share the composited (or raw) FP16
                    // scRGB frame; the preview tone-maps it for display. Throttled and
                    // non-blocking; never stalls the encode below.
                    tapPreviewSource(nativeSrc, pts_ns);

                    const auto conv_t0 = std::chrono::steady_clock::now();
                    if (!encodeNativeHdrSlot(nativeSrc, slot)) {
                        encoder->ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    const auto conv_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnVpbltSubmit(
                        conv_t1, std::chrono::duration<double, std::milli>(conv_t1 - conv_t0).count());
                    if (refNv12 != nullptr) {
                        d3dContext->CopyResource(refNv12.get(), nv12Textures[slot].get());
                        refNv12Valid = true;
                        refNv12Key = currentVisualKey; // from Task 6, computed earlier this tick
                    }
                    if (slot >= 0 && static_cast<size_t>(slot) < slotContainedKey.size()) {
                        slotContainedKey[slot] = currentVisualKey;
                        slotContainedValid[slot] = true;
                    }
                    lastRealFrameSlot = slot;
                    performSnapshotIfRequested(nv12Textures[static_cast<size_t>(slot)].get());
                    frameWritten = true;
                    lastCompositedKey = currentVisualKey;
                    haveLastCompositedKey = true;
                    m_state.diagnostics.OnFullComposition();
                } else if (rawSourceTex != nullptr) {
                    const WebcamOverlayLive overlay = m_state.SnapshotWebcamOverlay();
                    const auto comp_t0 = std::chrono::steady_clock::now();
                    ID3D11Texture2D* sdrSourceTex = toneMapIfHdr(rawSourceTex);
                    if (sdrSourceTex == nullptr) {
                        encoder->ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    ID3D11Texture2D* vpInput = compositeFrameGpu(sdrSourceTex, overlay);
                    const auto comp_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnCompositorSubmit(
                        comp_t1, std::chrono::duration<double, std::milli>(comp_t1 - comp_t0).count(),
                        needsGpuCompositor);
                    if (vpInput == nullptr) {
                        encoder->ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    if (useOdCapture) {
                        odCapturedTexValid = false;
                    } else {
                        heldWgcTex = std::move(pendingWgcTex);
                        // Consumed. The next iteration tests pendingWgcTex against
                        // nullptr to detect a fresh frame, so clear it explicitly
                        // instead of relying on the moved-from state.
                        pendingWgcTex = nullptr;
                    }

                    // Live WYSIWYG preview tap: share the composited pre-encode frame.
                    // Works for 4:2:0/4:2:2 AND 4:4:4 (tapped before RGB->AYUV). Throttled
                    // and non-blocking; never stalls the encode below.
                    tapPreviewSource(vpInput, pts_ns);

                    // Convert RGB frame to NV12/P010 via VideoProcessorBlt using the
                    // per-tick-cached input view (recreated only when vpInput changes).
                    ID3D11VideoProcessorInputView* inputView = acquireInputView(vpInput);

                    if (inputView != nullptr) {
                        D3D11_VIDEO_PROCESSOR_STREAM stream{};
                        stream.Enable = TRUE;
                        stream.pInputSurface = inputView;

                        const auto vp_t0 = std::chrono::steady_clock::now();
                        vpbltGpuTimer.Begin(d3dContext.get());
                        hr = videoContext->VideoProcessorBlt(videoProcessor.get(), videoOutputViews[slot].get(), 0, 1,
                                                             &stream);
                        vpbltGpuTimer.End(d3dContext.get());
                        const auto vp_t1 = std::chrono::steady_clock::now();
                        m_state.diagnostics.OnVpbltSubmit(
                            vp_t1, std::chrono::duration<double, std::milli>(vp_t1 - vp_t0).count());
                        if (const auto vp_gpu_ms = vpbltGpuTimer.Poll(d3dContext.get())) {
                            m_state.diagnostics.OnRgbToYuvGpuTime(vp_t1, *vp_gpu_ms);
                        }

                        std::string ayuvErr;
                        if (SUCCEEDED(hr) && !finalizeEncodeSurface(slot, ayuvErr)) {
                            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoEncode, "RGB->AYUV convert: " + ayuvErr);
                            encoder->ReleaseSlot(slot);
                            goto end_encode_loop;
                        }
                        if (SUCCEEDED(hr)) {
                            // Save the encode surface as reference for future duplicate frames
                            if (refNv12 != nullptr) {
                                d3dContext->CopyResource(refNv12.get(), nv12Textures[slot].get());
                                refNv12Valid = true;
                                refNv12Key = currentVisualKey; // from Task 6, computed earlier this tick
                            }
                            if (slot >= 0 && static_cast<size_t>(slot) < slotContainedKey.size()) {
                                slotContainedKey[slot] = currentVisualKey;
                                slotContainedValid[slot] = true;
                            }
                            // Capture frame snapshot on real (non-duplicate) frames.
                            lastRealFrameSlot = slot;
                            performSnapshotIfRequested(nv12Textures[static_cast<size_t>(slot)].get());
                            frameWritten = true;
                            lastCompositedKey = currentVisualKey;
                            haveLastCompositedKey = true;
                            m_state.diagnostics.OnFullComposition();
                        }
                    }
                } else if (refNv12Valid) {
                    // Duplicate: the slot may already hold exactly what refNv12
                    // holds (from a previous real composite or a previous
                    // duplicate copy) — if so, skip the redundant GPU copy and
                    // just re-submit the slot's existing content.
                    const bool slotAlreadyCurrent = slot >= 0 && static_cast<size_t>(slot) < slotContainedKey.size() &&
                                                    slotContainedValid[static_cast<size_t>(slot)] &&
                                                    slotContainedKey[static_cast<size_t>(slot)] == refNv12Key;
                    if (!slotAlreadyCurrent) {
                        d3dContext->CopyResource(nv12Textures[slot].get(), refNv12.get());
                        if (slot >= 0 && static_cast<size_t>(slot) < slotContainedKey.size()) {
                            slotContainedKey[static_cast<size_t>(slot)] = refNv12Key;
                            slotContainedValid[static_cast<size_t>(slot)] = true;
                        }
                        m_state.diagnostics.OnYuvSlotCopy();
                    } else {
                        m_state.diagnostics.OnYuvSlotCopySkipped();
                    }
                    frameWritten = true;
                    ++duplicatedFrames;
                    m_state.diagnostics.OnFrameDuplicated();
                    m_state.diagnostics.OnReusedYuvFrame();
                }

                if (!frameWritten) {
                    // Release the slot and skip the tick. Two very different causes end
                    // up here and must not share a counter: a source frame was available
                    // and its conversion (input view / VideoProcessorBlt) failed, which
                    // costs real picture, versus nothing to encode yet at session start,
                    // which is benign pacing. See ClassifyCfrTickDrop.
                    encoder->ReleaseSlot(slot);
                    ++droppedFrames;
                    if (ClassifyCfrTickDrop(rawSourceTex != nullptr, refNv12 != nullptr) ==
                        CfrTickDropCause::ProcessingFailure) {
                        m_state.diagnostics.OnFrameDroppedProcessingFailure();
                    } else {
                        m_state.diagnostics.OnFrameDroppedCfr();
                    }
                    cfr_frame_idx++;
                    next_tick_100ns += frame_interval_100ns;
                    anyWork = true;
                    continue;
                }

                // Arm a split boundary (manual or automatic) for this submission
                // so its forced IDR opens the next segment. Done before encode.
                maybeArmSplit(pts_ns);

                std::vector<EncodedVideoPacket> pkts;
                std::string encErr;
                m_state.diagnostics.OnEncodeSubmitted();
                const auto enc_t0 = std::chrono::steady_clock::now();
                bool encOk = encoder->EncodeFrame(slot, pts_ns, encodeWidth, encodeHeight, pkts, encErr);
                const auto enc_t1 = std::chrono::steady_clock::now();
                // Call-site CPU cost of the submit; the true submit->ready latency
                // (P5-P7-correct) travels on each packet and is reported only when set.
                m_state.diagnostics.OnEncodeSubmitCost(
                    enc_t1, std::chrono::duration<double, std::milli>(enc_t1 - enc_t0).count());
                for (const EncodedVideoPacket& pkt : pkts)
                    reportPacketDiagnostics(pkt, enc_t1);

                if (!encOk) {
                    m_state.RecordFailure(E_FAIL, ErrorPhase::VideoEncode, "NVENC encode (CFR): " + encErr);
                    goto end_encode_loop;
                }

                ++videoFramesCaptured;
                lastVideoPts = pts_ns;

                for (EncodedVideoPacket& pkt : pkts) {
                    if (!routePacket(std::move(pkt)))
                        goto end_encode_loop;
                }

                const auto tick_t1 = std::chrono::steady_clock::now();
                m_state.diagnostics.OnVideoTickTime(
                    tick_t1, std::chrono::duration<double, std::milli>(tick_t1 - tick_t0).count());

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
        // Floor for the not-yet-established epoch: the session start, advanced
        // by any pause that happens BEFORE the first frame arrives. Without the
        // advance, an early pause would sit before the clamped epoch and be
        // burned into the file as a lead-in.
        uint64_t vfrEpochFloor100ns = m_state.session_start_qpc_100ns;

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

            CheckHdrStateChanged();
            if (m_state.stop_requested.load()) {
                break;
            }

            // OD recovery: throttled Reopen() while holding (see the CFR loop above).
            if (odHolding) {
                const auto reopen_now = std::chrono::steady_clock::now();
                if (reopen_now - odLastReopenAttempt >= kOdReopenPollDelay) {
                    odLastReopenAttempt = reopen_now;
                    std::string reopenErr;
                    if (odSrc.Reopen(d3dDevice.get(), reopenErr))
                        odHolding = false;
                }
            }

            bool anyWork = false;

            // Drain any async completions before this iteration's tick-emit
            // so freed slots are available to the encode branches below.
            if (!reapAndRoute(0))
                goto end_encode_loop;

            winrt::com_ptr<ID3D11Texture2D> latestTex;
            int64_t latestFrameTicks100ns = 0;

            const CaptureDrainStep drainStep = NextCaptureDrainStep(useOdCapture, odHolding);
            if (drainStep == CaptureDrainStep::DrainOd) {
                // DXGI OD: drain available frames, copy to odCapturedTex, keep newest
                while (true) {
                    ID3D11Texture2D* rawTex = nullptr;
                    DXGI_OUTDUPL_FRAME_INFO info{};
                    HRESULT odHr = S_OK;
                    if (!odSrc.TryAcquireFrame(0, &rawTex, &info, &odHr)) {
                        // See the phase-correct drain above: DEVICE_REMOVED (and any
                        // unexpected HRESULT) ends the recording with the HRESULT recorded
                        // instead of looping through a dead source.
                        HandleOdAcquireFailure(odHr);
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
                    const bool diag_recording = !m_state.pause_requested.load();
                    const OdAcquireKind acquireKind =
                        ClassifyOdAcquire(info.LastPresentTime.QuadPart != 0, info.LastMouseUpdateTime.QuadPart != 0,
                                          m_state.config.capture_cursor);
                    if (acquireKind == OdAcquireKind::Ignorable) {
                        rawTex->Release();
                        odSrc.ReleaseFrame();
                        if (diag_recording)
                            m_state.diagnostics.OnCursorOnlyCaptureEventIgnored();
                        continue;
                    }
                    if (acquireKind == OdAcquireKind::DesktopPresent) {
                        d3dContext->CopyResource(odCapturedTex.get(), rawTex);
                        ++visualGenerations.screen;
                        if (diag_recording)
                            m_state.diagnostics.OnScreenGenerationChanged();
                    }
                    rawTex->Release();
                    if (info.PointerShapeBufferSize > 0 && m_state.config.capture_cursor) {
                        if (odSrc.GetFramePointerShape(&odCursorShapeInfo, odCursorBitmap)) {
                            odCursorShapeValid = true;
                            ++visualGenerations.cursor;
                        }
                    }
                    if (info.LastMouseUpdateTime.QuadPart != 0) {
                        const bool visibilityChanged = odCursorVisible != (info.PointerPosition.Visible != FALSE);
                        const bool positionChanged = odCursorPosX != info.PointerPosition.Position.x ||
                                                     odCursorPosY != info.PointerPosition.Position.y;
                        odCursorVisible = info.PointerPosition.Visible != FALSE;
                        odCursorPosX = info.PointerPosition.Position.x;
                        odCursorPosY = info.PointerPosition.Position.y;
                        if (visibilityChanged || positionChanged)
                            ++visualGenerations.cursor;
                    }
                    if (m_state.config.capture_cursor != lastCursorCaptureEnabled) {
                        lastCursorCaptureEnabled = m_state.config.capture_cursor;
                        ++visualGenerations.cursor;
                    }
                    // Convert DXGI LastPresentTime (QPC ticks) to 100ns units — only
                    // meaningful for a real desktop present.
                    if (acquireKind == OdAcquireKind::DesktopPresent) {
                        const auto lpt = static_cast<uint64_t>(info.LastPresentTime.QuadPart);
                        latestFrameTicks100ns =
                            static_cast<int64_t>(lpt / qpcFreq * 10000000ULL + lpt % qpcFreq * 10000000ULL / qpcFreq);
                    }
                    odSrc.ReleaseFrame();
                    if (diag_recording && acquireKind == OdAcquireKind::DesktopPresent)
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
                    if (acquireKind == OdAcquireKind::DesktopPresent) {
                        if (odCapturedTexValid) {
                            ++droppedFrames;
                            if (diag_recording)
                                m_state.diagnostics.OnFrameDroppedCoalesced();
                        }
                        odCapturedTexValid = true;
                    }
                }
                if (odCapturedTexValid) {
                    latestTex = odCapturedTex; // borrow — not released in loop
                    if (latestFrameTicks100ns == 0)
                        latestFrameTicks100ns = static_cast<int64_t>(Qpc100ns(qpcFreq));
                }
            } else if (drainStep == CaptureDrainStep::DrainWgc) {
                // WGC: drain frame pool — keep latest (always drain, even when paused)
                try {
                    // Drain to the newest queued frame before copying — see the
                    // CFR drain above (and WgcSourceProducer::PollFrame): only
                    // the newest is encoded, so only the newest is worth a
                    // full-surface GPU copy.
                    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{nullptr};
                    for (;;) {
                        auto next = framePool.TryGetNextFrame();
                        if (next == nullptr)
                            break;
                        const bool diag_recording = !m_state.pause_requested.load();
                        if (diag_recording)
                            m_state.diagnostics.OnFrameCaptured();
                        if (frame != nullptr || latestTex != nullptr) {
                            ++droppedFrames;
                            if (diag_recording)
                                m_state.diagnostics.OnFrameDroppedCoalesced();
                        }
                        frame = next;
                    }
                    if (frame != nullptr) {
                        // Same as the CFR drain: the pool surface never changes
                        // size on a window resize — ContentSize is the real
                        // signal — and the surface is recycled by the pool, so
                        // the kept frame must be a copy (see wgcCapturedTex).
                        const auto content = frame.ContentSize();
                        if (content.Width != static_cast<int32_t>(sourceWidth) ||
                            content.Height != static_cast<int32_t>(sourceHeight)) {
                            std::ostringstream err;
                            err << "capture source size changed during session from " << sourceWidth << "x"
                                << sourceHeight << " to " << content.Width << "x" << content.Height
                                << "; restart recording to reconfigure encoder";
                            m_state.RecordFailure(E_INVALIDARG, ErrorPhase::VideoCapture, err.str());
                            sourceLost = true;
                        } else {
                            auto surface = frame.Surface();
                            auto access =
                                surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                            winrt::com_ptr<ID3D11Texture2D> tex;
                            if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put())))) {
                                ID3D11Texture2D* const copied = copyWgcFrame(tex.get());
                                if (copied == nullptr) {
                                    // copyWgcFrame already recorded the failure.
                                    sourceLost = true;
                                } else {
                                    latestTex.copy_from(copied);
                                    latestFrameTicks100ns = frame.SystemRelativeTime().count();
                                }
                            }
                        }
                    }
                } catch (const winrt::hresult_error& e) {
                    HandleWgcAcquireException(m_state, e.code().value, "VFR drain", sourceLost);
                } catch (...) {
                    HandleWgcUnknownException(m_state, "VFR drain", sourceLost);
                }
                // Seed the first WGC frame from the wait loop so a static
                // window still encodes at least one real frame (WGC only
                // delivers frames on repaint).
                if (latestTex == nullptr && seedWgcTex != nullptr) {
                    latestTex = std::move(seedWgcTex);
                    // The seed is a one-shot. This drain runs once per iteration
                    // and the guard above is what stops it from firing again, so
                    // clear the handle explicitly rather than relying on the
                    // moved-from state to keep the guard false.
                    seedWgcTex = nullptr;
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
                if (lastRealFrameSlot >= 0)
                    performSnapshotIfRequested(nv12Textures[static_cast<size_t>(lastRealFrameSlot)].get());
                Sleep(1);
                continue;
            }
            if (vfr_was_paused) {
                const uint64_t paused100ns = Qpc100ns(qpcFreq) - vfr_pause_start_100ns;
                if (videoEpochSet) {
                    videoEpochTicks100ns += static_cast<int64_t>(paused100ns);
                } else {
                    vfrEpochFloor100ns += paused100ns;
                }
                vfr_was_paused = false;
            }

            if (latestTex != nullptr) {
                // Establish video epoch (also published for the muxer's A/V
                // alignment). The epoch is clamped so it never precedes the
                // session start (plus any pause served before the first frame):
                // a first DXGI frame can carry a LastPresentTime from before
                // recording began (static desktop), and an origin in the past
                // inflates every later frame's PTS by that idle gap — the
                // published A/V epoch and the internal PTS origin must be the
                // SAME clamped value, or audio shifts away from the picture by
                // the gap.
                if (!videoEpochSet) {
                    videoEpochTicks100ns = ClampedVfrVideoEpochTicks100ns(latestFrameTicks100ns, vfrEpochFloor100ns);
                    videoEpochSet = true;
                    // Publish the epoch the PTS timeline is actually built on,
                    // not the moment this frame happened to be processed. VFR
                    // PTS is `frame timestamp - videoEpochTicks100ns`, and both
                    // WGC SystemRelativeTime and DXGI LastPresentTime are QPC
                    // readings in 100 ns units — the same axis as
                    // session_start_qpc_100ns — so publishing "now" instead put
                    // the A/V alignment out by the capture-to-process latency.
                    // (The CFR path has no such split: it derives PTS from the
                    // same epoch value it publishes.)
                    m_state.video_epoch_qpc_100ns.store(static_cast<uint64_t>(videoEpochTicks100ns));
                }

                int64_t deltaTicks = latestFrameTicks100ns - videoEpochTicks100ns;
                if (deltaTicks < 0)
                    deltaTicks = 0;
                uint64_t framePts_ns = static_cast<uint64_t>(deltaTicks) * 100ULL;
                if (videoFramesCaptured > 0 && framePts_ns <= lastVideoPts) {
                    framePts_ns = lastVideoPts + 1;
                }
                lastVideoPts = framePts_ns;

                // Whole-tick frame-time bracket (VFR): from slot acquire to after
                // the packet is routed. Emitted only on the emit paths below.
                const auto tick_t0 = std::chrono::steady_clock::now();

                // Acquire a free input slot
                int32_t slot = encoder->AcquireFreeSlot();
                if (slot < 0) {
                    // No free input slot: give the async encoder a bounded chance to
                    // reap a completion and free one before counting a drop.
                    if (!reapAndRoute(kSlotWaitMs))
                        goto end_encode_loop;
                    slot = encoder->AcquireFreeSlot();
                }

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
                        encoder->ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    if (useOdCapture) {
                        odCapturedTexValid = false;
                    }

                    // Live WYSIWYG preview tap: share the composited (or raw) FP16
                    // scRGB frame; the preview tone-maps it for display (see CFR path).
                    tapPreviewSource(nativeSrc, framePts_ns);

                    const auto conv_t0 = std::chrono::steady_clock::now();
                    if (!encodeNativeHdrSlot(nativeSrc, slot)) {
                        encoder->ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    const auto conv_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnVpbltSubmit(
                        conv_t1, std::chrono::duration<double, std::milli>(conv_t1 - conv_t0).count());
                    latestTex = nullptr;

                    lastRealFrameSlot = slot;
                    performSnapshotIfRequested(nv12Textures[static_cast<size_t>(slot)].get());
                    maybeArmSplit(framePts_ns);

                    std::vector<EncodedVideoPacket> pkts;
                    std::string encErr;
                    m_state.diagnostics.OnEncodeSubmitted();
                    const auto enc_t0 = std::chrono::steady_clock::now();
                    bool encOk = encoder->EncodeFrame(slot, framePts_ns, encodeWidth, encodeHeight, pkts, encErr);
                    const auto enc_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnEncodeSubmitCost(
                        enc_t1, std::chrono::duration<double, std::milli>(enc_t1 - enc_t0).count());
                    for (const EncodedVideoPacket& pkt : pkts)
                        reportPacketDiagnostics(pkt, enc_t1);
                    if (!encOk) {
                        m_state.RecordFailure(E_FAIL, ErrorPhase::VideoEncode, "NVENC encode: " + encErr);
                        break;
                    }
                    ++videoFramesCaptured;
                    for (EncodedVideoPacket& pkt : pkts) {
                        if (!routePacket(std::move(pkt)))
                            goto end_encode_loop;
                    }
                    const auto tick_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnVideoTickTime(
                        tick_t1, std::chrono::duration<double, std::milli>(tick_t1 - tick_t0).count());
                } else if (slot >= 0) {
                    const WebcamOverlayLive overlay = m_state.SnapshotWebcamOverlay();
                    const auto comp_t0 = std::chrono::steady_clock::now();
                    ID3D11Texture2D* sdrSourceTex = toneMapIfHdr(latestTex.get());
                    if (sdrSourceTex == nullptr) {
                        encoder->ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    ID3D11Texture2D* vpInput = compositeFrameGpu(sdrSourceTex, overlay);
                    const auto comp_t1 = std::chrono::steady_clock::now();
                    m_state.diagnostics.OnCompositorSubmit(
                        comp_t1, std::chrono::duration<double, std::milli>(comp_t1 - comp_t0).count(),
                        needsGpuCompositor);
                    if (vpInput == nullptr) {
                        encoder->ReleaseSlot(slot);
                        goto end_encode_loop;
                    }
                    if (useOdCapture) {
                        odCapturedTexValid = false;
                    }

                    // Live WYSIWYG preview tap: share the composited pre-encode frame
                    // (works for 4:4:4 too; non-blocking, never stalls the encode below).
                    tapPreviewSource(vpInput, framePts_ns);

                    // RGB -> NV12/P010 via VideoProcessorBlt into the selected slot's
                    // view, reusing the per-tick-cached input view (see acquireInputView).
                    ID3D11VideoProcessorInputView* inputView = acquireInputView(vpInput);

                    if (inputView != nullptr) {
                        D3D11_VIDEO_PROCESSOR_STREAM stream{};
                        stream.Enable = TRUE;
                        stream.pInputSurface = inputView;

                        const auto vp_t0 = std::chrono::steady_clock::now();
                        vpbltGpuTimer.Begin(d3dContext.get());
                        hr = videoContext->VideoProcessorBlt(videoProcessor.get(), videoOutputViews[slot].get(), 0, 1,
                                                             &stream);
                        vpbltGpuTimer.End(d3dContext.get());
                        const auto vp_t1 = std::chrono::steady_clock::now();
                        m_state.diagnostics.OnVpbltSubmit(
                            vp_t1, std::chrono::duration<double, std::milli>(vp_t1 - vp_t0).count());
                        if (const auto vp_gpu_ms = vpbltGpuTimer.Poll(d3dContext.get())) {
                            m_state.diagnostics.OnRgbToYuvGpuTime(vp_t1, *vp_gpu_ms);
                        }

                        latestTex = nullptr;

                        std::string ayuvErr;
                        if (SUCCEEDED(hr) && !finalizeEncodeSurface(slot, ayuvErr)) {
                            m_state.RecordFailure(E_FAIL, ErrorPhase::VideoEncode, "RGB->AYUV convert: " + ayuvErr);
                            encoder->ReleaseSlot(slot);
                            goto end_encode_loop;
                        }
                        if (SUCCEEDED(hr)) {
                            // Capture frame snapshot on real frames (VFR path).
                            lastRealFrameSlot = slot;
                            performSnapshotIfRequested(nv12Textures[static_cast<size_t>(slot)].get());

                            // Arm a split boundary for this submission (see CFR path).
                            maybeArmSplit(framePts_ns);

                            std::vector<EncodedVideoPacket> pkts;
                            std::string encErr;
                            m_state.diagnostics.OnEncodeSubmitted();
                            const auto enc_t0 = std::chrono::steady_clock::now();
                            bool encOk =
                                encoder->EncodeFrame(slot, framePts_ns, encodeWidth, encodeHeight, pkts, encErr);
                            const auto enc_t1 = std::chrono::steady_clock::now();
                            m_state.diagnostics.OnEncodeSubmitCost(
                                enc_t1, std::chrono::duration<double, std::milli>(enc_t1 - enc_t0).count());
                            for (const EncodedVideoPacket& pkt : pkts)
                                reportPacketDiagnostics(pkt, enc_t1);

                            if (!encOk) {
                                m_state.RecordFailure(E_FAIL, ErrorPhase::VideoEncode, "NVENC encode: " + encErr);
                                break;
                            }

                            ++videoFramesCaptured;

                            for (EncodedVideoPacket& pkt : pkts) {
                                if (!routePacket(std::move(pkt)))
                                    goto end_encode_loop;
                            }

                            const auto tick_t1 = std::chrono::steady_clock::now();
                            m_state.diagnostics.OnVideoTickTime(
                                tick_t1, std::chrono::duration<double, std::milli>(tick_t1 - tick_t0).count());
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
                    m_state.diagnostics.OnSlotStall();
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
                // Teardown, and deliberately silent: the session has already
                // ended, the file is finalized below, and a revoke/Close that
                // throws on a source that is already gone has nothing left to
                // report. Unlike the acquire paths above, nothing downstream can
                // mistake this for a running recording.
            }
        } catch (...) {
            // Same boundary as above, for Close() on the session or the pool.
        }
    }

    // --- Flush NVENC EOS ---
    {
        std::vector<EncodedVideoPacket> drainPkts;
        std::string flushErr;
        // flushErr is not escalated — a partial drain is acceptable; any encoded
        // output already in the mux queue is preserved regardless of flush outcome.
        encoder->Flush(drainPkts, flushErr);

        for (auto& pkt : drainPkts) {
            if (pkt.bytes.empty())
                continue;

            const size_t drain_bytes = pkt.bytes.size();

            if (pkt.keyframe) {
                if (m_state.config.video_codec == VideoCodec::H264 && !h264CodecPrivateReady) {
                    std::vector<uint8_t> spsPps;
                    if (annexb::ExtractH264SpsAndPps(pkt.bytes.data(), pkt.bytes.size(), spsPps)) {
                        std::lock_guard lk(m_state.premux_mutex);
                        m_state.codec_private.h264_sps_pps = std::move(spsPps);
                        m_state.codec_private.h264_ready = true;
                        h264CodecPrivateReady = true;
                        m_state.premux_cv.notify_all();
                    }
                } else if (m_state.config.video_codec == VideoCodec::Hevc && !hevcCodecPrivateReady) {
                    std::vector<uint8_t> vpsSpsPps;
                    if (annexb::ExtractHevcVpsSpsPps(pkt.bytes.data(), pkt.bytes.size(), vpsSpsPps)) {
                        std::lock_guard lk(m_state.premux_mutex);
                        m_state.codec_private.hevc_vps_sps_pps = std::move(vpsSpsPps);
                        m_state.codec_private.hevc_ready = true;
                        hevcCodecPrivateReady = true;
                        m_state.premux_cv.notify_all();
                    }
                } else if (m_state.config.video_codec == VideoCodec::Av1 && !av1CodecPrivateReady) {
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
                    std::unique_lock mlk(m_state.mux_mutex);
                    // Bounded steady-state queue (same policy as the live loop):
                    // wait for room, then fail cleanly rather than grow unbounded.
                    if (!m_state.WaitForMuxQueueSpace(mlk)) {
                        mlk.unlock();
                        m_state.RecordFailure(E_OUTOFMEMORY, ErrorPhase::Mux,
                                              "Mux queue limit exceeded while draining the video encoder");
                        break;
                    }
                    m_state.PushMuxItemLocked(std::move(mi));
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
    encoder->Destroy();

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
        m_state.PushMuxItemLocked(std::move(eos)); // sentinel: bypasses the queue bound
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
}

} // namespace exosnap::engine
