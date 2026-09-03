#include <exosnap/engine/dxgi_od_capture_src.h>

#include <exosnap/engine/hdr_color_space.h>
#include <exosnap/engine/sdr_white_level.h>

#include <dxgi1_6.h>

#include <chrono>
#include <cstdio>
#include <iterator>
#include <vector>

namespace exosnap::engine {

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

// The output's current mode, by its stable GDI device name (od_output_signature.h).
// The DEVMODE is the authoritative mode; HDR comes from the output's colour space
// (IDXGIOutput6::GetDesc1), found again through a fresh factory so the answer is
// the display's state NOW, not what the duplication's factory enumerated at open.
static OutputModeSignature ReadOutputModeSignature(const std::wstring& device_name) {
    OutputModeSignature sig;
    if (device_name.empty())
        return sig;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &dm))
        return sig;
    sig.width = dm.dmPelsWidth;
    sig.height = dm.dmPelsHeight;
    sig.refresh_hz = dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0u;
    sig.orientation = (dm.dmFields & DM_DISPLAYORIENTATION) ? dm.dmDisplayOrientation : 0u;
    sig.known = true;

    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
        return sig;
    for (UINT a = 0;; ++a) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, adapter.put()) == DXGI_ERROR_NOT_FOUND)
            break;
        for (UINT o = 0;; ++o) {
            winrt::com_ptr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, output.put()) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || device_name != desc.DeviceName)
                continue;
            if (winrt::com_ptr<IDXGIOutput6> output6 = output.try_as<IDXGIOutput6>()) {
                DXGI_OUTPUT_DESC1 desc1{};
                if (SUCCEEDED(output6->GetDesc1(&desc1)))
                    sig.hdr_active = IsHdrColorSpace(desc1.ColorSpace);
            }
            return sig;
        }
    }
    return sig;
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
            facts.hdr_active = IsHdrColorSpace(desc1.ColorSpace);
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

// Resolve the IDXGIOutput to duplicate for `device`, using a FRESH DXGI factory so
// the current display topology is seen (a device's original adapter/output
// enumeration and HMONITOR handles go stale after a monitor hot-plug or mode/
// topology change — the exact loss recovery must survive). The search is filtered
// to the device's own adapter by LUID, so DuplicateOutput's same-adapter
// requirement holds. Matches the output whose DXGI_OUTPUT_DESC matches either
// `match_monitor` (initial Open, by handle) or `match_name` (Reopen, by stable GDI
// device name, when the handle has changed). Returns the matched output + its desc.
static bool ResolveOutputForDevice(ID3D11Device* device, HMONITOR match_monitor, const std::wstring& match_name,
                                   IDXGIOutput** out_output, DXGI_OUTPUT_DESC* out_desc, std::string& out_error) {
    if (!device || !out_output) {
        out_error = "null argument";
        return false;
    }
    // The device's adapter LUID is stable across topology changes; the fresh
    // enumeration below is filtered to it.
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())))) {
        out_error = "QI IDXGIDevice failed";
        return false;
    }
    winrt::com_ptr<IDXGIAdapter> devAdapter;
    if (FAILED(dxgiDevice->GetAdapter(devAdapter.put()))) {
        out_error = "GetAdapter failed";
        return false;
    }
    DXGI_ADAPTER_DESC devAdapterDesc{};
    devAdapter->GetDesc(&devAdapterDesc);
    const LUID devLuid = devAdapterDesc.AdapterLuid;

    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
        out_error = "CreateDXGIFactory1 failed";
        return false;
    }

    winrt::com_ptr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 aDesc{};
        if (SUCCEEDED(adapter->GetDesc1(&aDesc)) && aDesc.AdapterLuid.LowPart == devLuid.LowPart &&
            aDesc.AdapterLuid.HighPart == devLuid.HighPart) {
            winrt::com_ptr<IDXGIOutput> output;
            for (UINT j = 0; adapter->EnumOutputs(j, output.put()) != DXGI_ERROR_NOT_FOUND; ++j) {
                DXGI_OUTPUT_DESC d{};
                if (SUCCEEDED(output->GetDesc(&d))) {
                    const bool matched = match_monitor ? (d.Monitor == match_monitor)
                                                       : (!match_name.empty() && match_name == d.DeviceName);
                    if (matched) {
                        if (out_desc)
                            *out_desc = d;
                        *out_output = output.detach();
                        return true;
                    }
                }
                output = nullptr;
            }
        }
        adapter = nullptr;
    }
    out_error = "no matching output on the capture device's adapter";
    return false;
}

bool DxgiOdCaptureSrc::Open(ID3D11Device* device, HMONITOR hmonitor, std::string& out_error) {
    if (!device || !hmonitor) {
        out_error = "null argument";
        return false;
    }

    // Resolve the output via a fresh factory (see ResolveOutputForDevice) and record
    // the stable GDI device name so Reopen() can re-find it by name after a hot-plug.
    HRESULT hr = S_OK;
    winrt::com_ptr<IDXGIOutput> matchedOutput;
    DXGI_OUTPUT_DESC matchedDesc{};
    if (!ResolveOutputForDevice(device, hmonitor, std::wstring{}, matchedOutput.put(), &matchedDesc, out_error))
        return false;
    m_device_name = matchedDesc.DeviceName;
    m_open_signature = ReadOutputModeSignature(m_device_name);
    m_signature_changed = false;
    m_signature_checked_at = std::chrono::steady_clock::now();

    // Topology watch (see TopologyChangedSinceOpen). A factory reports IsCurrent()
    // false once the adapter/output set it enumerated has changed, which is the
    // only honest signal that a silent duplication may be pointing at a topology
    // that no longer exists. A factory that cannot be created leaves the watch
    // unarmed -- the check then reports "unchanged" and costs nothing.
    m_topology_factory = nullptr;
    {
        winrt::com_ptr<IDXGIFactory1> watch;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(watch.put())))) {
            m_topology_factory = watch;
        }
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
    // DXGI_OUTDUPL_DESC.ModeDesc is an unreliable source for the mode timing — for
    // desktop duplication RefreshRate is frequently {0,0} (the same ModeDesc whose
    // Format the capture-format policy warns must not be trusted, see the header). Fall
    // back to the authoritative current display mode via GDI, keyed by the stable
    // device name captured above. dmDisplayFrequency is whole Hz; 0/1 mean "hardware
    // default" and are treated as unknown so the ring keeps its conservative fallback.
    if (m_refresh_rate_hz == 0 && !m_device_name.empty()) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(m_device_name.c_str(), ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
            m_refresh_rate_hz = dm.dmDisplayFrequency;
        }
    }
    m_frame_held = false;
    return true;
}

void DxgiOdCaptureSrc::Close() {
    if (m_frame_held && m_duplication) {
        m_duplication->ReleaseFrame();
        m_frame_held = false;
    }
    m_duplication = nullptr;
    m_topology_factory = nullptr;
    m_width = 0;
    m_height = 0;
    m_refresh_rate_hz = 0;
}

bool DxgiOdCaptureSrc::Reopen(ID3D11Device* device, std::string& out_error) {
    // Drop any held frame and the stale duplication, then rebuild it on the still-
    // alive device. The monitor may return from a hot-plug with a NEW HMONITOR, so
    // re-resolve the output by the stable GDI device name (captured at Open) against
    // a fresh factory, then run the normal Open() path with the CURRENT handle.
    // Open() re-reads size/format/HDR facts and resets m_frame_held; on failure it
    // leaves the source closed so a subsequent poll attempt can try again.
    Close();
    if (!device) {
        out_error = "null argument";
        return false;
    }
    if (m_device_name.empty()) {
        out_error = "no stored device name for reopen";
        return false;
    }
    winrt::com_ptr<IDXGIOutput> matchedOutput;
    DXGI_OUTPUT_DESC matchedDesc{};
    if (!ResolveOutputForDevice(device, nullptr, m_device_name, matchedOutput.put(), &matchedDesc, out_error))
        return false;
    return Open(device, matchedDesc.Monitor, out_error);
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

bool DxgiOdCaptureSrc::TopologyChangedSinceOpen() const noexcept {
    if (m_topology_factory && m_topology_factory->IsCurrent() == FALSE)
        return true;
    if (m_signature_changed)
        return true;
    // Re-read at a bounded cadence: this is asked on every idle acquire timeout,
    // and the read enumerates DXGI outputs.
    constexpr auto kRecheckInterval = std::chrono::milliseconds(500);
    const auto now = std::chrono::steady_clock::now();
    if (now - m_signature_checked_at < kRecheckInterval)
        return false;
    m_signature_checked_at = now;
    m_signature_changed = OutputModeChanged(m_open_signature, ReadOutputModeSignature(m_device_name));
    return m_signature_changed;
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
        // An HDR desktop that arrived as PQ R10G10B10A2 (no FP16 surface on
        // offer) is still PQ/BT.2020: treating it as SDR would encode PQ codes
        // as gamma-2.2 BT.709 -- washed out and mis-tagged. Same policy as the
        // FP16 branch below: TonemapSdr, or Hdr10 on a codec without HDR10
        // output, tone-maps down; Off keeps the pre-HDR behaviour.
        if (hdr_active && hdr_mode != HdrMode::Off) {
            out_mode = OdCaptureMode::HdrToneMap;
            return true;
        }
        out_mode = OdCaptureMode::Sdr;
        return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        // scRGB FP16 desktop. As with R10G10B10A2, the format alone does not mean
        // HDR: with Advanced Color Management the desktop composites to scRGB FP16
        // while still in SDR mode. Such a desktop carries SDR content (reference
        // white == 1.0), so it is merely sRGB-encoded, never tone-mapped, and it
        // records in every mode -- including Off, which is not an HDR request.
        if (!hdr_active) {
            out_mode = OdCaptureMode::SdrScrgb;
            return true;
        }
        // A genuinely HDR-active desktop. Off keeps the pre-HDR behaviour (a
        // defined capture error). Hdr10 with an HDR10-capable codec keeps the
        // native PQ/BT.2020 signal; otherwise (TonemapSdr, or Hdr10 on H.264) the
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

OdReopenDecision DecideOdReopen(bool reopened, std::chrono::milliseconds elapsed,
                                std::optional<std::chrono::milliseconds> budget,
                                std::chrono::milliseconds poll_delay) noexcept {
    if (reopened) {
        // The duplication is live again — resume the same encode session. A late
        // success past any budget still continues: recovered footage beats a
        // strict deadline.
        return {OdReopenAction::Continue, std::chrono::milliseconds{0}};
    }
    if (budget && elapsed >= *budget) {
        // A budget was set and it is exhausted: end cleanly (the historic
        // ACCESS_LOST behaviour). With no budget the retry is unbounded.
        return {OdReopenAction::GiveUp, std::chrono::milliseconds{0}};
    }
    // Still recovering: wait the poll delay, then retry. When bounded, clamp the
    // wait to the remaining budget so the loop cannot sleep past the deadline and
    // stall the give-up.
    std::chrono::milliseconds delay = poll_delay;
    if (budget) {
        const std::chrono::milliseconds remaining = *budget - elapsed;
        delay = poll_delay < remaining ? poll_delay : remaining;
    }
    return {OdReopenAction::RetryAfter, delay};
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

} // namespace exosnap::engine
