#pragma once

// A HubSourceProducer backed by Windows.Graphics.Capture (WGC), driving one
// GraphicsCaptureItem for the lifetime of an Open()/Close() pair.
//
// The D3D device is injected, not created here: every WGC producer in the
// process shares it, so a consumer can read back a HubFrame's texture on the
// device it already owns. Without that seam the copy would cross devices and
// need a shared handle for nothing.
//
// Threading: every method -- Open, Close, PollFrame -- must run on ONE thread,
// and that thread must already be a single-threaded COM apartment that pumps
// messages. WGC delivers frames and the item.Closed callback through that pump;
// this class neither calls CoInitializeEx nor runs a message loop. The caller
// owns both.

#include <atomic>
#include <cstdint>
#include <string>

#include <d3d11.h>

#include <winrt/base.h>

#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include "services/CaptureSourceHub.h"
#include "services/CaptureSourceKey.h"

namespace exosnap {

class WgcSourceProducer final : public HubSourceProducer {
  public:
    WgcSourceProducer(CaptureSourceKey key, winrt::com_ptr<ID3D11Device> device);
    ~WgcSourceProducer() override;

    WgcSourceProducer(const WgcSourceProducer&) = delete;
    WgcSourceProducer& operator=(const WgcSourceProducer&) = delete;

    bool Open(std::string& err) override;
    void Close() override;
    ProducerPoll PollFrame(HubFrame& out) override;

  private:
    CaptureSourceKey key_;
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device_{nullptr};

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::event_token closed_token_{};
    winrt::Windows::Graphics::SizeInt32 pool_size_{};

    // Set from the item.Closed callback on the pumping thread, read by PollFrame
    // on that same thread. Atomic only so the flag has a defined value across the
    // callback boundary, not because a second thread ever touches it.
    std::atomic<bool> source_closed_{false};

    // The frame pool recycles its surfaces, so a held frame must be a copy the
    // producer owns. Reallocated only when the frame's dimensions change.
    winrt::com_ptr<ID3D11Texture2D> copy_texture_;
    uint32_t copy_width_ = 0;
    uint32_t copy_height_ = 0;

    uint64_t generation_ = 0;
};

} // namespace exosnap
