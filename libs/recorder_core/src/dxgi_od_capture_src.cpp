#include "dxgi_od_capture_src.h"

#include <dxgi1_5.h>

#include <cstdio>
#include <iterator>

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

    // Prefer IDXGIOutput5::DuplicateOutput1 (Win10 1703+): declaring the
    // supported SDR formats lets DXGI hand us the desktop's native surface —
    // BGRA8 on an 8-bit desktop, R10G10B10A2 on a 10 bpc SDR desktop — instead
    // of relying on the legacy API's implicit 8-bit-only behavior. On desktops
    // whose format is not in the list (e.g. FP16 HDR/Advanced Color),
    // DuplicateOutput1 fails and we fall back to the legacy API below, which
    // provides a tone-mapped BGRA8 compatibility surface.
    winrt::com_ptr<IDXGIOutputDuplication> duplication;
    HRESULT dup1Hr = E_NOINTERFACE;
    if (winrt::com_ptr<IDXGIOutput5> output5 = matchedOutput.try_as<IDXGIOutput5>()) {
        static constexpr DXGI_FORMAT kSupportedFormats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM};
        dup1Hr = output5->DuplicateOutput1(device, 0, static_cast<UINT>(std::size(kSupportedFormats)),
                                           kSupportedFormats, duplication.put());
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
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM;
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
