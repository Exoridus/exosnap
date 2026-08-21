#include "DxgiSourceProducer.h"

#include "../diagnostics/AppLog.h"

#include <cstdio>
#include <iterator>

namespace exosnap {
namespace {

// Reopen attempts enumerate the topology and create a device; pace them so the
// hub's per-tick retry (60 Hz pump) does not hammer DXGI while a display is
// renegotiating. 250 ms keeps reconnection prompt without the hammering.
constexpr std::chrono::milliseconds kOpenThrottle{250};

// Resolve the stable GDI device name back to the CURRENT HMONITOR. The handle
// changes across a hot-plug; the name does not.
HMONITOR FindMonitorByDeviceName(const std::wstring& device_name) {
    struct Ctx {
        const std::wstring* name = nullptr;
        HMONITOR found = nullptr;
    } ctx{&device_name, nullptr};

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(param);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info) != FALSE && *c->name == info.szDevice) {
                c->found = monitor;
                return FALSE; // stop enumerating
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

} // namespace

DxgiSourceProducer::DxgiSourceProducer(std::wstring device_name) : device_name_(std::move(device_name)) {
}

DxgiSourceProducer::~DxgiSourceProducer() {
    Close();
}

bool DxgiSourceProducer::Open(std::string& err) {
    const auto now = std::chrono::steady_clock::now();
    if (last_open_attempt_ != std::chrono::steady_clock::time_point{} && now - last_open_attempt_ < kOpenThrottle) {
        err = "reopen throttled";
        return false;
    }
    last_open_attempt_ = now;

    Close();

    const HMONITOR monitor = FindMonitorByDeviceName(device_name_);
    if (monitor == nullptr) {
        err = "output not in the current topology";
        return false;
    }

    // Adapter-matched device, recreated per open: required for DuplicateOutput
    // on multi-GPU systems, and what makes a DEVICE_REMOVED recoverable here.
    winrt::com_ptr<IDXGIAdapter1> adapter;
    if (!recorder_core::FindAdapterForMonitor(monitor, adapter.put(), err)) {
        return false;
    }

    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device_.put(), nullptr,
                                   context_.put());
    if (FAILED(hr)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "D3D11CreateDevice failed 0x%08lX", static_cast<unsigned long>(hr));
        err = buf;
        Close();
        return false;
    }

    if (!od_.Open(device_.get(), monitor, err)) {
        Close();
        return false;
    }
    copied_since_open_ = false;
    return true;
}

void DxgiSourceProducer::Close() {
    od_.Close();
    copy_texture_ = nullptr;
    copy_width_ = 0;
    copy_height_ = 0;
    copy_format_ = DXGI_FORMAT_UNKNOWN;
    context_ = nullptr;
    device_ = nullptr;
}

ProducerPoll DxgiSourceProducer::PollFrame(HubFrame& out) {
    if (!od_.IsOpen())
        return ProducerPoll::Lost;

    ID3D11Texture2D* frame_tex = nullptr;
    DXGI_OUTDUPL_FRAME_INFO info{};
    HRESULT hr = S_OK;
    if (!od_.TryAcquireFrame(0, &frame_tex, &info, &hr)) {
        // Recover and Fail both map to Lost: the hub reopens unbounded, and this
        // producer recreates its device per open, so even a DEVICE_REMOVED is
        // worth retrying here (unlike the engine mid-session, which must end the
        // recording cleanly instead).
        return recorder_core::ClassifyOdAcquireFailure(hr) == recorder_core::OdAcquireFailAction::Idle
                   ? ProducerPoll::NoFrame
                   : ProducerPoll::Lost;
    }

    if (frame_tex == nullptr) {
        od_.ReleaseFrame();
        return ProducerPoll::NoFrame;
    }

    // A metadata-only acquire (cursor move on an unchanged desktop) carries no
    // new pixels; skip the copy. The preview draws the live cursor itself, so
    // nothing is lost. The FIRST frame of an open is always copied regardless:
    // on a static desktop it is the only image this duplication will deliver
    // for a while, and its LastPresentTime may legitimately be zero.
    const bool metadata_only = info.LastPresentTime.QuadPart == 0;
    if (metadata_only && copied_since_open_) {
        od_.ReleaseFrame();
        return ProducerPoll::NoFrame;
    }

    D3D11_TEXTURE2D_DESC desc{};
    frame_tex->GetDesc(&desc);

    if (!copy_texture_ || copy_width_ != desc.Width || copy_height_ != desc.Height || copy_format_ != desc.Format) {
        copy_texture_ = nullptr;
        D3D11_TEXTURE2D_DESC copy_desc{};
        copy_desc.Width = desc.Width;
        copy_desc.Height = desc.Height;
        copy_desc.MipLevels = 1;
        copy_desc.ArraySize = 1;
        copy_desc.Format = desc.Format;
        copy_desc.SampleDesc.Count = 1;
        copy_desc.Usage = D3D11_USAGE_DEFAULT;
        copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const HRESULT texHr = device_->CreateTexture2D(&copy_desc, nullptr, copy_texture_.put());
        if (FAILED(texHr)) {
            od_.ReleaseFrame();
            diagnostics::AppLog::warning(QStringLiteral("dxgi-hub"),
                                         QStringLiteral("frame copy allocation failed 0x%1")
                                             .arg(static_cast<unsigned long>(texHr), 8, 16, QChar('0')));
            return ProducerPoll::Lost;
        }
        copy_width_ = desc.Width;
        copy_height_ = desc.Height;
        copy_format_ = desc.Format;
    }

    // The duplication frame is borrowed until ReleaseFrame, so the held frame
    // must be a copy the producer owns.
    context_->CopyResource(copy_texture_.get(), frame_tex);
    od_.ReleaseFrame();
    copied_since_open_ = true;

    out.texture = copy_texture_;
    out.width = copy_width_;
    out.height = copy_height_;
    out.generation = ++generation_;
    return ProducerPoll::Frame;
}

} // namespace exosnap
