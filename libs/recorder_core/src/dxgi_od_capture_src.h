#pragma once

// DXGI Output Duplication capture backend for Monitor targets.
// Wraps IDXGIOutputDuplication — passive GPU-buffer read, no VRR interference,
// no OS capture indicator. Used in place of WGC when target is a Monitor.

#include <cstdint>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <winrt/base.h>

#include <recorder_core/codec_types.h>

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

  private:
    winrt::com_ptr<IDXGIOutputDuplication> m_duplication;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_refresh_rate_hz = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool m_frame_held = false;
    bool m_hdr_active = false;
    float m_max_luminance_nits = 0.0f;
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
    Sdr,        // BGRA8 / R10G10B10A2 desktop: straight to the VideoProcessor.
    HdrToneMap, // scRGB FP16 HDR desktop: tone-mapped to SDR BT.709 first.
};

// Resolve how a first-frame OD capture format should be treated for the given
// HDR handling mode. Returns false when the format cannot be recorded at all,
// or when it is an HDR (FP16) desktop while HDR handling is Off (a defined
// capture error, matching the pre-HDR behaviour). On success sets out_mode.
//
// Hdr10 currently resolves to HdrToneMap: the native HDR10 output path is not
// yet implemented, so an HDR desktop is tone-mapped to SDR either way.
bool ResolveOdCaptureMode(DXGI_FORMAT format, HdrMode hdr_mode, OdCaptureMode& out_mode) noexcept;

// Short human-readable name for the formats OD capture can plausibly see.
// Unknown values render as "DXGI_FORMAT(<n>)" into fallback_buf.
const char* OdCaptureFormatName(DXGI_FORMAT format, char* fallback_buf, size_t fallback_len) noexcept;

} // namespace recorder_core
