#include <exosnap/engine/wgc_session_config.h>

#include <winrt/Windows.Graphics.Capture.h>

namespace exosnap::engine {
namespace {

namespace wgc = winrt::Windows::Graphics::Capture;

constexpr int64_t kOneMillisecondTicks = 10'000;

} // namespace

HRESULT ConfigureWgcMinUpdateInterval(const winrt::Windows::Foundation::IInspectable& session,
                                      WgcMinUpdateIntervalResult* out) noexcept {
    if (out == nullptr)
        return E_POINTER;

    *out = {};
    out->requested_ticks = kOneMillisecondTicks;

    auto* unknown = reinterpret_cast<::IUnknown*>(winrt::get_abi(session));
    if (unknown == nullptr)
        return E_POINTER;

    winrt::com_ptr<winrt::impl::abi_t<wgc::IGraphicsCaptureSession5>> interval_session;
    HRESULT hr = unknown->QueryInterface(winrt::guid_of<wgc::IGraphicsCaptureSession5>(), interval_session.put_void());
    if (hr == E_NOINTERFACE)
        return S_OK;
    if (FAILED(hr))
        return hr;

    out->supported = true;
    hr = interval_session->put_MinUpdateInterval(kOneMillisecondTicks);
    if (FAILED(hr))
        return hr;

    int64_t effective_ticks = 0;
    hr = interval_session->get_MinUpdateInterval(&effective_ticks);
    if (FAILED(hr))
        return hr;

    out->effective_ticks = effective_ticks;
    return S_OK;
}

} // namespace exosnap::engine
