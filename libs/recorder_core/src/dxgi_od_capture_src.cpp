#include "dxgi_od_capture_src.h"

#include <cstdio>

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

    // QI to IDXGIOutput1 for DuplicateOutput
    winrt::com_ptr<IDXGIOutput1> output1 = matchedOutput.as<IDXGIOutput1>();
    if (!output1) {
        out_error = "IDXGIOutput1 not supported (requires DXGI 1.2 / Windows 8+)";
        return false;
    }

    winrt::com_ptr<IDXGIOutputDuplication> duplication;
    hr = output1->DuplicateOutput(device, duplication.put());
    if (FAILED(hr)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "DuplicateOutput failed 0x%08lX", static_cast<unsigned long>(hr));
        out_error = buf;
        return false;
    }

    DXGI_OUTDUPL_DESC desc{};
    duplication->GetDesc(&desc);

    m_duplication = std::move(duplication);
    m_width = desc.ModeDesc.Width;
    m_height = desc.ModeDesc.Height;
    m_format = desc.ModeDesc.Format;
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
