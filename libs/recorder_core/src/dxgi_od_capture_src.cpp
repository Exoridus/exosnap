#include "dxgi_od_capture_src.h"

#include <recorder_core/sdr_white_level.h>

#include <dxgi1_6.h>

#include <cstdio>
#include <iterator>
#include <vector>

namespace recorder_core {

// ---------------------------------------------------------------------------
// FindAdapterForMonitor
// ---------------------------------------------------------------------------

bool FindAdapterForMonitor(HMONITOR hmonitor, IDXGIAdapter1** out_adapter, std::string& out_error) {
    if (!hmonitor || !out_adapter) {
        out_error = "null argument";
        return false;
    }

    winrt::com_ptr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "CreateDXGIFactory1 failed 0x%08lX", static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }

    winrt::com_ptr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
        winrt::com_ptr<IDXGIOutput> output;
        for (UINT j = 0; adapter->EnumOutputs(j, output.put()) != DXGI_ERROR_NOT_FOUND; ++j) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == hmonitor) {
                *out_adapter = adapter.detach();
                return true;
            }
            output = nullptr;
        }
        adapter = nullptr;
    }

    out_error = "no adapter found owning the specified HMONITOR";
    return false;
}

// ---------------------------------------------------------------------------
// DxgiOdCaptureSrc
// ---------------------------------------------------------------------------

DxgiOdCaptureSrc::~DxgiOdCaptureSrc() {
    Close();
}

// Queries the OS "SDR content brightness" reference white of the monitor, in
// nits (DISPLAYCONFIG_SDR_WHITE_LEVEL; raw 1000 == 80 nits). Returns 0.0f when
// the monitor cannot be matched or any DisplayConfig call fails — callers
// treat 0 as unknown and fall back to the 203-nit default.
static float QuerySdrWhiteLevelNits(HMONITOR hmonitor) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmonitor, &mi) == FALSE) {
        return 0.0f;
    }

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
        return 0.0f;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        return 0.0f;
    }
    paths.resize(pathCount);

    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }
        if (wcscmp(source.viewGdiDeviceName, mi.szDevice) != 0) {
            continue;
        }

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) {
            return 0.0f;
        }
        return SdrWhiteLevelRawToNits(white.SDRWhiteLevel);
    }
    return 0.0f;
}

// Fill HDR facts from an already-matched DXGI output + its monitor. Mirrors the
// exact ordering used at Open() below: the SDR white level is read first (it is
// the OS SDR-content reference, independent of the HDR colour-space gate), then
// the primaries/luminance/hdr_active are read from IDXGIOutput6::GetDesc1 when the
// output exposes it. Shared by Open() (OD path) and QueryDisplayHdrFacts (WGC path)
// so both paths derive the identical facts.
static void FillHdrFactsFromOutput(IDXGIOutput* output, HMONITOR hmonitor, HdrDisplayFacts& facts) {
    facts = HdrDisplayFacts{};
    facts.sdr_white_level_nits = QuerySdrWhiteLevelNits(hmonitor);
    if (output == nullptr) {
        return;
    }
    winrt::com_ptr<IDXGIOutput> outputPtr;
    outputPtr.copy_from(output);
    if (winrt::com_ptr<IDXGIOutput6> output6 = outputPtr.try_as<IDXGIOutput6>()) {
        DXGI_OUTPUT_DESC1 desc1{};
        if (SUCCEEDED(output6->GetDesc1(&desc1))) {
            facts.hdr_active = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            facts.red_primary_x = desc1.RedPrimary[0];
            facts.red_primary_y = desc1.RedPrimary[1];
            facts.green_primary_x = desc1.GreenPrimary[0];
            facts.green_primary_y = desc1.GreenPrimary[1];
            facts.blue_primary_x = desc1.BluePrimary[0];
            facts.blue_primary_y = desc1.BluePrimary[1];
            facts.white_point_x = desc1.WhitePoint[0];
            facts.white_point_y = desc1.WhitePoint[1];
            facts.max_luminance_nits = desc1.MaxLuminance;
            facts.min_luminance_nits = desc1.MinLuminance;
        }
    }
}

bool QueryDisplayHdrFacts(HMONITOR hmonitor, HdrDisplayFacts& out_facts) {
    out_facts = HdrDisplayFacts{};
    if (!hmonitor) {
        return false;
    }
    // The SDR white level does not depend on finding a DXGI output, so seed it even
    // if the enumeration below fails to match (matches the OD ordering).
    out_facts.sdr_white_level_nits = QuerySdrWhiteLevelNits(hmonitor);

    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
        return false;
    }
    winrt::com_ptr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
        winrt::com_ptr<IDXGIOutput> output;
        for (UINT j = 0; adapter->EnumOutputs(j, output.put()) != DXGI_ERROR_NOT_FOUND; ++j) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == hmonitor) {
                FillHdrFactsFromOutput(output.get(), hmonitor, out_facts);
                return true;
            }
            output = nullptr;
        }
        adapter = nullptr;
    }
    return false;
}

bool DxgiOdCaptureSrc::Open(ID3D11Device* device, HMONITOR hmonitor, std::string& out_error) {
    if (!device || !hmonitor) {
        out_error = "null argument";
        return false;
    }

    // QI device -> IDXGIDevice -> adapter -> outputs
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "QI IDXGIDevice failed 0x%08lX", static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }

    winrt::com_ptr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(dxgiAdapter.put());
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "GetAdapter failed 0x%08lX", static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }

    // Find the IDXGIOutput that matches hmonitor
    winrt::com_ptr<IDXGIOutput> matchedOutput;
    winrt::com_ptr<IDXGIOutput> output;
    for (UINT j = 0; dxgiAdapter->EnumOutputs(j, output.put()) != DXGI_ERROR_NOT_FOUND; ++j) {
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == hmonitor) {
            matchedOutput = output;
            break;
        }
        output = nullptr;
    }

    if (!matchedOutput) {
        out_error = "device adapter does not own the specified HMONITOR";
        return false;
    }

    // HDR facts of the active output (IDXGIOutput6::GetDesc1), read before
    // duplicating so the format request can depend on the display's HDR state.
    // hdr_active is only true in a PQ/BT.2020 colour space — an SDR-mode display
    // still reports its EDID luminance caps here, which are not the active
    // reference; consumers gate their use of the luminance/primaries on it.
    // Full facts (hdr_active, chromaticity primaries, luminance range, SDR white
    // level) for native HDR10 mastering-display metadata and tone-map peak. The
    // primaries/luminance are the display's capabilities (the usual approximation
    // for content mastering values). Shared with the WGC path via the same helper.
    FillHdrFactsFromOutput(matchedOutput.get(), hmonitor, m_hdr_facts);
    m_hdr_active = m_hdr_facts.hdr_active;
    m_max_luminance_nits = m_hdr_facts.max_luminance_nits;

    // Prefer IDXGIOutput5::DuplicateOutput1 (Win10 1703+): declaring the
    // supported formats lets DXGI hand us the desktop's native surface — BGRA8
    // on an 8-bit desktop, R10G10B10A2 on a 10 bpc SDR desktop, and scRGB FP16
    // on an HDR/Advanced-Color desktop — instead of relying on the legacy API's
    // implicit 8-bit-only behavior.
    //
    // The array is a PRIORITY order, not just a set: DXGI provides the first
    // listed format it can supply. On an HDR display, FP16 must come first so the
    // duplication delivers the real scRGB linear signal; with BGRA8 first, DXGI
    // tone-maps the HDR desktop down to an 8-bit SDR compatibility surface
    // (measured) and the HDR signal never reaches the pipeline. On an SDR display
    // the priority is unchanged (BGRA8 for 8-bit, R10G10B10A2 for a 10 bpc
    // desktop) so SDR capture keeps its exact prior behaviour. Also note:
    // DuplicateOutput1 returns DXGI_ERROR_UNSUPPORTED for a process that is not
    // per-monitor-DPI-aware, which forces the legacy BGRA8 path below — the app
    // is DPI-aware (Qt sets Per-Monitor-v2), a required precondition for HDR
    // capture. If none of the listed formats can be provided, DuplicateOutput1
    // fails and we fall back to the legacy API.
    winrt::com_ptr<IDXGIOutputDuplication> duplication;
    HRESULT dup1Hr = E_NOINTERFACE;
    if (winrt::com_ptr<IDXGIOutput5> output5 = matchedOutput.try_as<IDXGIOutput5>()) {
        static constexpr DXGI_FORMAT kHdrFormats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
                                                      DXGI_FORMAT_B8G8R8A8_UNORM};
        static constexpr DXGI_FORMAT kSdrFormats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
                                                      DXGI_FORMAT_R16G16B16A16_FLOAT};
        const DXGI_FORMAT* formats = m_hdr_active ? kHdrFormats : kSdrFormats;
        dup1Hr = output5->DuplicateOutput1(device, 0, 3u, formats, duplication.put());
    }

    if (!duplication) {
        // Legacy fallback (BGRA8-only; DXGI converts HDR desktops to SDR).
        winrt::com_ptr<IDXGIOutput1> output1 = matchedOutput.try_as<IDXGIOutput1>();
        if (!output1) {
            out_error = "IDXGIOutput1 not supported (requires DXGI 1.2 / Windows 8+)";
            return false;
        }
        hr = output1->DuplicateOutput(device, duplication.put());
        if (FAILED(hr)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "DuplicateOutput failed 0x%08lX (DuplicateOutput1: 0x%08lX)",
                     static_cast<unsigned long>(hr), static_cast<unsigned long>(dup1Hr));
            out_error = buf;
            return false;
        }
    }

    DXGI_OUTDUPL_DESC desc{};
    duplication->GetDesc(&desc);

    m_duplication = std::move(duplication);
    m_width = desc.ModeDesc.Width;
    m_height = desc.ModeDesc.Height;
    m_format = desc.ModeDesc.Format;
    m_refresh_rate_hz = (desc.ModeDesc.RefreshRate.Denominator > 0)
                            ? (desc.ModeDesc.RefreshRate.Numerator / desc.ModeDesc.RefreshRate.Denominator)
                            : 0u;
    m_frame_held = false;
    return true;
}

void DxgiOdCaptureSrc::Close() {
    if (m_frame_held && m_duplication) {
        m_duplication->ReleaseFrame();
        m_frame_held = false;
    }
    m_duplication = nullptr;
    m_width = 0;
    m_height = 0;
    m_refresh_rate_hz = 0;
}

bool DxgiOdCaptureSrc::TryAcquireFrame(uint32_t timeout_ms, ID3D11Texture2D** out_texture,
                                       DXGI_OUTDUPL_FRAME_INFO* out_info, HRESULT* out_hr) {
    if (!m_duplication || m_frame_held) {
        if (out_hr)
            *out_hr = E_INVALIDARG;
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    winrt::com_ptr<IDXGIResource> resource;
    HRESULT hr = m_duplication->AcquireNextFrame(timeout_ms, &info, resource.put());

    if (out_hr)
        *out_hr = hr;

    if (FAILED(hr)) {
        return false;
    }

    winrt::com_ptr<ID3D11Texture2D> tex;
    hr = resource->QueryInterface(IID_PPV_ARGS(tex.put()));
    if (FAILED(hr)) {
        m_duplication->ReleaseFrame();
        if (out_hr)
            *out_hr = hr;
        return false;
    }

    if (out_info)
        *out_info = info;
    if (out_texture)
        *out_texture = tex.detach();
    m_frame_held = true;
    return true;
}

void DxgiOdCaptureSrc::ReleaseFrame() {
    if (m_frame_held && m_duplication) {
        m_duplication->ReleaseFrame();
        m_frame_held = false;
    }
}

// ---------------------------------------------------------------------------
// OD capture-format support policy (see header for rationale)
// ---------------------------------------------------------------------------

bool IsSupportedOdCaptureFormat(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM ||
           format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool ResolveOdCaptureMode(DXGI_FORMAT format, HdrMode hdr_mode, bool hdr_active, bool hdr10_output_supported,
                          OdCaptureMode& out_mode) noexcept {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        out_mode = OdCaptureMode::Sdr;
        return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        // A 10 bpc desktop composites to R10G10B10A2 in BOTH SDR (gamma/BT.709)
        // and HDR10 (PQ/BT.2020) mode — the format alone cannot tell them apart,
        // so hdr_active disambiguates. An HDR10 desktop with the native path
        // requested is passed straight through (already PQ-encoded); every SDR
        // 10 bpc desktop keeps its existing SDR handling unchanged.
        if (hdr_active && hdr_mode == HdrMode::Hdr10 && hdr10_output_supported) {
            out_mode = OdCaptureMode::HdrNative;
            return true;
        }
        out_mode = OdCaptureMode::Sdr;
        return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        // scRGB FP16 HDR desktop. Off keeps the pre-HDR behaviour (a defined
        // capture error). Hdr10 with an HDR10-capable codec keeps the native
        // PQ/BT.2020 signal; otherwise (TonemapSdr, or Hdr10 on H.264) the
        // desktop is tone-mapped down to SDR.
        if (hdr_mode == HdrMode::Off) {
            return false;
        }
        out_mode = (hdr_mode == HdrMode::Hdr10 && hdr10_output_supported) ? OdCaptureMode::HdrNative
                                                                          : OdCaptureMode::HdrToneMap;
        return true;
    default:
        return false;
    }
}

WgcCapturePlan ResolveWgcCapturePlan(bool hdr_active, HdrMode hdr_mode, bool hdr10_output_supported) noexcept {
    // SDR desktop, or HDR handling disabled: keep the historic BGRA8 window path
    // byte-identical (DWM tone-maps an HDR window down to SDR into this pool).
    if (!hdr_active || hdr_mode == HdrMode::Off) {
        return WgcCapturePlan{DXGI_FORMAT_B8G8R8A8_UNORM, OdCaptureMode::Sdr};
    }
    // HDR-active display + HDR handling on: request a scRGB FP16 pool so the real
    // HDR signal reaches the pipeline instead of DWM's SDR tone-map. Native HDR10
    // when the codec can carry it; otherwise tone-map to SDR BT.709.
    const OdCaptureMode mode =
        (hdr_mode == HdrMode::Hdr10 && hdr10_output_supported) ? OdCaptureMode::HdrNative : OdCaptureMode::HdrToneMap;
    return WgcCapturePlan{DXGI_FORMAT_R16G16B16A16_FLOAT, mode};
}

const char* OdCaptureFormatName(DXGI_FORMAT format, char* fallback_buf, size_t fallback_len) noexcept {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return "B8G8R8A8_UNORM (8-bit SDR)";
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return "R10G10B10A2_UNORM (10 bpc SDR)";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return "R16G16B16A16_FLOAT (HDR/Advanced Color)";
    default:
        if (fallback_buf != nullptr && fallback_len > 0) {
            snprintf(fallback_buf, fallback_len, "DXGI_FORMAT(%u)", static_cast<unsigned>(format));
            return fallback_buf;
        }
        return "DXGI_FORMAT(?)";
    }
}

OdAcquireFailAction ClassifyOdAcquireFailure(HRESULT hr) noexcept {
    switch (hr) {
    case S_OK:
    case DXGI_ERROR_WAIT_TIMEOUT:
        // No frame this poll tick — normal, keep draining.
        return OdAcquireFailAction::Idle;
    case DXGI_ERROR_ACCESS_LOST:
        // Duplication handle invalidated (mode/topology change) but the device is
        // alive: recreate the duplication and continue the same recording.
        return OdAcquireFailAction::Recover;
    default:
        // DXGI_ERROR_DEVICE_REMOVED / _HUNG / _RESET and every other unexpected
        // HRESULT: fail closed — end the recording cleanly rather than loop.
        return OdAcquireFailAction::Fail;
    }
}

bool DxgiOdCaptureSrc::GetFramePointerShape(DXGI_OUTDUPL_POINTER_SHAPE_INFO* out_shape_info,
                                            std::vector<uint8_t>& out_bitmap) {
    if (!m_duplication || !m_frame_held || !out_shape_info)
        return false;

    // First call: get required buffer size
    UINT required = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo{};
    HRESULT hr = m_duplication->GetFramePointerShape(0, nullptr, &required, &shapeInfo);

    if (hr != DXGI_ERROR_MORE_DATA && FAILED(hr))
        return false;

    if (required == 0)
        return false;

    out_bitmap.resize(required);
    hr = m_duplication->GetFramePointerShape(required, out_bitmap.data(), &required, &shapeInfo);
    if (FAILED(hr)) {
        out_bitmap.clear();
        return false;
    }

    *out_shape_info = shapeInfo;
    return true;
}

} // namespace recorder_core
