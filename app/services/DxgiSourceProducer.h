#pragma once

// A HubSourceProducer backed by DXGI Output Duplication, driving one
// DxgiOdCaptureSrc for the lifetime of an Open()/Close() pair.
//
// Unlike the WGC producer, DxgiOdCaptureSrc brings neither a device nor a
// thread of its own: Open takes an adapter-matched ID3D11Device* from outside,
// and the poll loop lives with the caller. This producer therefore owns its own
// adapter-matched device — recreated at every (re)open, so a hot-plug that
// moves the output to another adapter, or a DEVICE_REMOVED that killed the old
// device, both heal on the next reopen. That is also why PollFrame never
// reports Fatal: with the device recreated per open, no failure here is
// terminal the way it is for the engine mid-session.
//
// The producer is keyed by the STABLE GDI device name ("\\.\DISPLAYn"), not by
// HMONITOR: a monitor that leaves and re-joins the topology comes back with a
// new HMONITOR, and the whole point of the hub is to hold and reconnect across
// exactly that.
//
// Threading: every method must run on ONE thread (the hub's pump thread). No
// COM apartment is required — Output Duplication is plain DXGI.

#include <chrono>
#include <cstdint>
#include <string>

#include <d3d11.h>

#include <winrt/base.h>

#include <exosnap/engine/dxgi_od_capture_src.h>
#include <exosnap/engine/hdr_native.h>

#include "services/CaptureSourceHub.h"

namespace exosnap {

class DxgiSourceProducer final : public HubSourceProducer {
  public:
    explicit DxgiSourceProducer(std::wstring device_name);
    ~DxgiSourceProducer() override;

    DxgiSourceProducer(const DxgiSourceProducer&) = delete;
    DxgiSourceProducer& operator=(const DxgiSourceProducer&) = delete;

    bool Open(std::string& err) override;
    void Close() override;
    ProducerPoll PollFrame(HubFrame& out) override;

    // The producer's device and context: the publisher creates the shared
    // preview texture on this device and copies frames with this context.
    // Valid between a successful Open and the matching Close.
    [[nodiscard]] ID3D11Device* Device() const noexcept {
        return device_.get();
    }
    [[nodiscard]] ID3D11DeviceContext* Context() const noexcept {
        return context_.get();
    }

    // HDR facts of the duplicated display, sampled at Open (see
    // DxgiOdCaptureSrc). Feed ResolveRawCaptureTapDesc for FP16 frames.
    [[nodiscard]] const exosnap::engine::HdrDisplayFacts& DisplayFacts() const noexcept {
        return od_.DisplayFacts();
    }

  private:
    std::wstring device_name_;

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    exosnap::engine::DxgiOdCaptureSrc od_;

    // The hub retries a failed reopen on every pump tick (unbounded, no backoff
    // by design). A reopen here enumerates the topology and creates a D3D
    // device, so the producer paces itself: attempts inside the throttle window
    // fail fast without touching DXGI.
    std::chrono::steady_clock::time_point last_open_attempt_{};

    // Whether this open has delivered a real frame yet: the first acquire is
    // always copied even when metadata-only (a static desktop's only image).
    bool copied_since_open_ = false;

    // The duplication's frame is borrowed until ReleaseFrame, so the held frame
    // must be a copy the producer owns. Reallocated on size/format change.
    winrt::com_ptr<ID3D11Texture2D> copy_texture_;
    uint32_t copy_width_ = 0;
    uint32_t copy_height_ = 0;
    DXGI_FORMAT copy_format_ = DXGI_FORMAT_UNKNOWN;

    uint64_t generation_ = 0;
};

} // namespace exosnap
