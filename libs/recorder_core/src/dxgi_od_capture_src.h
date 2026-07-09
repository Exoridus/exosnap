#pragma once

// DXGI Output Duplication capture backend for Monitor targets.
// Wraps IDXGIOutputDuplication — passive GPU-buffer read, no VRR interference,
// no OS capture indicator. Used in place of WGC when target is a Monitor.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <winrt/base.h>

#include <recorder_core/codec_types.h>
#include <recorder_core/hdr_native.h>

namespace recorder_core {

class DxgiOdCaptureSrc {
  public:
    DxgiOdCaptureSrc() = default;
    ~DxgiOdCaptureSrc();

    DxgiOdCaptureSrc(const DxgiOdCaptureSrc&) = delete;
    DxgiOdCaptureSrc& operator=(const DxgiOdCaptureSrc&) = delete;

    // Find the adapter/output owning hmonitor, create IDXGIOutputDuplication.
    // device must be created from the same adapter (use FindAdapterForMonitor first).
    bool Open(ID3D11Device* device, HMONITOR hmonitor, std::string& out_error);

    uint32_t Width() const noexcept {
        return m_width;
    }
    uint32_t Height() const noexcept {
        return m_height;
    }
    DXGI_FORMAT Format() const noexcept {
        return m_format;
    }
    // Refresh rate of the duplicated output in Hz (integer; 0 if unknown).
    // Available after Open() succeeds; derived from DXGI_OUTDUPL_DESC.ModeDesc.RefreshRate.
    uint32_t RefreshRateHz() const noexcept {
        return m_refresh_rate_hz;
    }

    // True when the duplicated output is currently in an HDR colour space
    // (DXGI_OUTPUT_DESC1.ColorSpace == RGB_FULL_G2084_NONE_P2020). Available
    // after Open(). This gates use of the reported peak luminance: a display in
    // SDR mode reports its EDID luminance caps, which are not the active tone-
    // map reference.
    bool HdrActive() const noexcept {
        return m_hdr_active;
    }
    // Reported display peak luminance in cd/m^2 from IDXGIOutput6::GetDesc1
    // (0 if unknown / not queryable). Only meaningful when HdrActive() is true.
    float MaxLuminanceNits() const noexcept {
        return m_max_luminance_nits;
    }

    // Full HDR facts of the duplicated display (DXGI_OUTPUT_DESC1: hdr_active,
    // chromaticity primaries, white point, luminance range). Available after
    // Open(). Feeds native HDR10 mastering-display metadata; the primaries /
    // luminance are only meaningful when hdr_active is true.
    const HdrDisplayFacts& DisplayFacts() const noexcept {
        return m_hdr_facts;
    }

    // Non-blocking (timeout_ms=0) or timed acquire.
    // On success: returns true; *out_texture is borrowed until ReleaseFrame().
    // On timeout: returns false, *out_hr == DXGI_ERROR_WAIT_TIMEOUT.
    // On access lost: returns false, *out_hr == DXGI_ERROR_ACCESS_LOST.
    bool TryAcquireFrame(uint32_t timeout_ms, ID3D11Texture2D** out_texture, DXGI_OUTDUPL_FRAME_INFO* out_info,
                         HRESULT* out_hr);

    // Release the currently held frame. No-op if none held.
    void ReleaseFrame();

    // Fetch the pointer (cursor) shape for the current frame.
    // Must be called while a frame is held and out_info.PointerShapeBufferSize > 0.
    bool GetFramePointerShape(DXGI_OUTDUPL_POINTER_SHAPE_INFO* out_shape_info, std::vector<uint8_t>& out_bitmap);

    bool IsOpen() const noexcept {
        return m_duplication != nullptr;
    }
    void Close();

    // Recreate the duplication after a recoverable acquire loss (ClassifyOd-
    // AcquireFailure -> Recover, i.e. DXGI_ERROR_ACCESS_LOST) while keeping the
    // same D3D device and encode session alive. Releases any held frame, tears
    // down the stale IDXGIOutputDuplication (Close()), and rebuilds it.
    //
    // The output is re-resolved by the STABLE GDI device name (captured at Open),
    // not by the HMONITOR passed at Open: a monitor that fully leaves and re-joins
    // the topology (hot-plug, KVM/EDID renegotiation) comes back with a NEW
    // HMONITOR handle, so the stored handle would never match again. The device
    // name (\\.\DISPLAYn) survives that, and a fresh DXGI factory is used so the
    // enumeration reflects the current topology (the capture device's own adapter
    // enumeration is a stale snapshot after a topology change). The output can be
    // briefly un-enumerable while the renegotiation settles, so a single call may
    // fail and leave the source closed; the caller polls this under
    // DecideOdReopen()'s budget. Returns true when the duplication is live again;
    // on success the width/height/format are re-read from the fresh duplication (a
    // changed size or format is caught by the drain's per-frame guard).
    bool Reopen(ID3D11Device* device, std::string& out_error);

  private:
    winrt::com_ptr<IDXGIOutputDuplication> m_duplication;
    // Stable GDI device name (DXGI_OUTPUT_DESC.DeviceName, e.g. "\\.\DISPLAY2") of
    // the duplicated output, captured at Open(). Used by Reopen() to re-find the
    // output after its HMONITOR handle changes across a hot-plug. Never cleared by
    // Close() so recovery can re-resolve.
    std::wstring m_device_name;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_refresh_rate_hz = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool m_frame_held = false;
    bool m_hdr_active = false;
    float m_max_luminance_nits = 0.0f;
    HdrDisplayFacts m_hdr_facts;
};

// Locate the IDXGIAdapter1 that owns hmonitor.
// Returns true and sets *out_adapter on success.
bool FindAdapterForMonitor(HMONITOR hmonitor, IDXGIAdapter1** out_adapter, std::string& out_error);

// ---------------------------------------------------------------------------
// OD capture-format support policy
// ---------------------------------------------------------------------------
// The desktop framebuffer duplicated by IDXGIOutputDuplication is not always
// BGRA8: a 10 bpc SDR desktop (e.g. NVIDIA "Output color depth: 10 bpc")
// composites to R10G10B10A2, and an HDR/Advanced-Color desktop to FP16.
// DXGI_OUTDUPL_DESC.ModeDesc.Format describes the *desktop* surface; the
// frames AcquireNextFrame actually delivers may differ (measured: the legacy
// DuplicateOutput API reports an FP16 ModeDesc on an Advanced-Color desktop
// but hands out BGRA8 compatibility frames). Format decisions must therefore
// be made from the acquired frame's texture desc, never from ModeDesc alone.

// True for the frame formats the recording pipeline can consume as OD input:
// BGRA8 (8-bit SDR desktop), R10G10B10A2 (10 bpc SDR desktop), and
// R16G16B16A16_FLOAT (scRGB FP16 on an HDR/Advanced-Color desktop). The SDR
// formats go straight to the D3D11 VideoProcessor (RGB->NV12/P010); FP16 is
// first tone-mapped to an SDR BT.709 surface by the compute path and then
// follows the same VideoProcessor route.
bool IsSupportedOdCaptureFormat(DXGI_FORMAT format) noexcept;

// How a captured OD frame format is handled by the encode pipeline.
enum class OdCaptureMode {
    Sdr,        // BGRA8 / SDR R10G10B10A2 desktop: straight to the VideoProcessor.
    SdrScrgb,   // scRGB FP16 SDR desktop (Advanced Color Management): linear SDR
                // content (reference white == 1.0). sRGB-encoded to an SDR surface
                // with no roll-off, then handled exactly like Sdr.
    HdrToneMap, // scRGB FP16 HDR desktop: tone-mapped to SDR BT.709 first.
    HdrNative,  // HDR desktop kept as native HDR10: PQ/BT.2020 P010 (scRGB FP16
                // is transferred to PQ; an HDR10 R10G10B10A2 desktop is already PQ).
};

// Resolve how a first-frame OD capture format should be treated for the given
// HDR handling mode. Returns false when the format cannot be recorded at all,
// or when it is an HDR-active FP16 desktop while HDR handling is Off (a defined
// capture error, matching the pre-HDR behaviour). On success sets out_mode.
//
// hdr_active disambiguates the two desktops that share a pixel format: an HDR10
// R10G10B10A2 desktop from an SDR 10 bpc one, and an HDR scRGB FP16 desktop from
// an SDR Advanced-Color one. hdr10_output_supported is true only for codecs that
// can encode HDR10 (HEVC/AV1); when false an HDR desktop requested as Hdr10 is
// tone-mapped to SDR instead of kept native.
bool ResolveOdCaptureMode(DXGI_FORMAT format, HdrMode hdr_mode, bool hdr_active, bool hdr10_output_supported,
                          OdCaptureMode& out_mode) noexcept;

// The frame-pool pixel format and capture mode chosen for a WGC (window) target.
struct WgcCapturePlan {
    DXGI_FORMAT frame_pool_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    OdCaptureMode mode = OdCaptureMode::Sdr;
};

// Decide the WGC frame-pool format + capture mode for a window target given the
// hosting display's HDR state, the requested HDR handling mode, and whether the
// codec can carry HDR10. Unlike the OD path (which negotiates the format from real
// frames), WGC lets us *request* the frame-pool format, so this is a pure up-front
// decision. An FP16 (scRGB) pool is requested only when the display is HDR-active
// AND HDR handling is on; otherwise BGRA8 exactly as before, so an SDR desktop (or
// HDR handling Off) records byte-identically to the historic window-capture path.
// FP16 then feeds the same tone-map / native-HDR10 machinery as the OD FP16 route:
//   - Hdr10 + HDR10-capable codec -> HdrNative (PQ/BT.2020 P010)
//   - otherwise (TonemapSdr, or Hdr10 on H.264) -> HdrToneMap (scRGB -> SDR BT.709)
[[nodiscard]] WgcCapturePlan ResolveWgcCapturePlan(bool hdr_active, HdrMode hdr_mode,
                                                   bool hdr10_output_supported) noexcept;

// Query the HDR facts of the display containing hmonitor (IDXGIOutput6::GetDesc1
// primaries/luminance/colour space + DISPLAYCONFIG_SDR_WHITE_LEVEL), the same
// facts the OD backend reads at Open(). Shared by the WGC (window) capture path,
// which resolves its hosting monitor via MonitorFromWindow at session start.
// Enumerates DXGI outputs via a fresh factory to find the matching output; on any
// failure out_facts is left with hdr_active == false (sdr_white_level_nits is still
// filled from DisplayConfig when available, 0 == unknown). Returns true when the
// monitor was matched to a DXGI output (regardless of HDR state).
bool QueryDisplayHdrFacts(HMONITOR hmonitor, HdrDisplayFacts& out_facts);

// Short human-readable name for the formats OD capture can plausibly see.
// Unknown values render as "DXGI_FORMAT(<n>)" into fallback_buf.
const char* OdCaptureFormatName(DXGI_FORMAT format, char* fallback_buf, size_t fallback_len) noexcept;

// How the OD drain must react to a TryAcquireFrame() failure HRESULT.
enum class OdAcquireFailAction {
    Idle,    // No frame this poll tick (DXGI_ERROR_WAIT_TIMEOUT / S_OK): keep draining.
    Recover, // DXGI_ERROR_ACCESS_LOST: duplication handle stale but the D3D device is
             // alive — recreate the duplication (Close()+Open()) and continue the same
             // encode session. Caused by mode/topology changes (fullscreen, refresh/HDR
             // switch, shared-monitor re-negotiation).
    Fail,    // DXGI_ERROR_DEVICE_REMOVED/HUNG/RESET or any unexpected HRESULT: the source
             // is unrecoverable in place — raise source-loss so the recording ends
             // cleanly (EOS -> finalise) instead of looping through a dead GPU.
};

// Classify a TryAcquireFrame() failure HRESULT into the drain's reaction. Pure and
// D3D-free so the recording-loss policy is unit-pinned. Any HRESULT not explicitly
// reasoned about is treated as Fail (fail closed, never loop silently).
OdAcquireFailAction ClassifyOdAcquireFailure(HRESULT hr) noexcept;

// ---------------------------------------------------------------------------
// OD live re-duplication (recovery) policy
// ---------------------------------------------------------------------------
// Once an acquire loss is classified Recover, the drain rebuilds the duplication
// (Reopen()) and continues the SAME encode session and output file, holding the
// last captured frame (frozen, not black) for the duration of the gap. Because the
// output can be absent momentarily — or indefinitely — while the topology
// renegotiation settles, Reopen() is polled until it returns. The retry is
// unbounded by default: the recording ends only on an explicit user stop or an
// unrecoverable DEVICE_REMOVED, matching OBS/desktop-duplication practice (the user
// decides when to stop). An optional budget can still bound it. This decision is
// pure and D3D-free so the retry policy is unit-pinned like ClassifyOdAcquireFailure.

// What the reopen retry loop should do after one Reopen() attempt.
enum class OdReopenAction {
    Continue,   // Reopen() succeeded: resume draining the same encode session.
    RetryAfter, // Not recovered yet: wait, then retry (unbounded, or still in budget).
    GiveUp,     // Bounded budget exhausted without recovery: end the recording cleanly
                // (source-loss -> EOS -> finalise). Never produced for an unbounded budget.
};

struct OdReopenDecision {
    OdReopenAction action = OdReopenAction::GiveUp;
    // How long to wait before the next Reopen() attempt. Only meaningful for
    // RetryAfter; clamped to the remaining window when a budget is set.
    std::chrono::milliseconds retry_delay{0};
};

// Decide the next step of the reopen retry loop from the last attempt's result,
// the time elapsed since the loss began, and an optional recovery budget. Success
// always wins (Continue), even past any budget — recovered footage beats a strict
// deadline. Otherwise the loop keeps retrying with RetryAfter(poll_delay). When
// budget is set, elapsed at/past it yields GiveUp (clean stop) and the retry delay
// is clamped to the remaining window so the loop cannot overshoot; when budget is
// std::nullopt the retry is unbounded (GiveUp is never returned). Time is a
// parameter so no wall clock is read here.
OdReopenDecision DecideOdReopen(bool reopened, std::chrono::milliseconds elapsed,
                                std::optional<std::chrono::milliseconds> budget,
                                std::chrono::milliseconds poll_delay) noexcept;

} // namespace recorder_core
