#pragma once

// A capture hub: one capture per source, fanned out to N consumers, holding the
// last good frame whenever the source stops producing.
//
// The hub decides nothing. Every open, close, reopen and hold comes out of
// recorder_core::StepCaptureHub; this class performs the actions that decision
// asks for and drives the producer accordingly. Keep it that way -- the policy's
// tests are only a pin on real behaviour for as long as it holds.
//
// The producer is injected, so the loop runs without WGC, DXGI or a GPU. That
// seam is what makes the reconnect and hold behaviour testable at all; the
// webcam's identical loop has no such seam and is untested to this day.
//
// Threading: not thread-safe by itself beyond the subscriber list. One thread
// calls Pump(); Subscribe/Unsubscribe may come from another. Callbacks run on
// the pumping thread while the subscriber lock is held, so a callback must be
// short and must not re-enter the hub.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <d3d11.h>

#include <winrt/base.h>

#include <recorder_core/capture_hub_policy.h>

namespace exosnap {

// A frame as the hub passes it around. The texture may be null in tests and on
// the paths that only care about liveness; `generation` always advances.
struct HubFrame {
    winrt::com_ptr<ID3D11Texture2D> texture;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t generation = 0;
};

enum class ProducerPoll {
    Frame,   // a new frame is in `out`
    NoFrame, // the source is healthy and simply had nothing new (not loss)
    Lost,    // recoverable: WGC blanked, the display is renegotiating, unplugged
    Fatal,   // unrecoverable: DEVICE_REMOVED and friends. Stop retrying.
};

// The capture underneath a hub. Implemented over WGC, over DXGI Output
// Duplication, and by a fake in tests.
class HubSourceProducer {
  public:
    virtual ~HubSourceProducer() = default;

    virtual bool Open(std::string& err) = 0;
    virtual void Close() = 0;

    // Never blocks for long: the hub pumps this on a cadence.
    virtual ProducerPoll PollFrame(HubFrame& out) = 0;
};

class CaptureSourceHub {
  public:
    // Called with each new frame. Never called for a held frame -- a hold is not
    // a new frame; consumers that need it ask HeldFrame().
    using FrameCallback = std::function<void(const HubFrame&, recorder_core::HubFrameKind)>;

    explicit CaptureSourceHub(std::unique_ptr<HubSourceProducer> producer);
    ~CaptureSourceHub();

    CaptureSourceHub(const CaptureSourceHub&) = delete;
    CaptureSourceHub& operator=(const CaptureSourceHub&) = delete;

    // Opens the capture if this is the first consumer. The returned token
    // revokes the subscription; after Unsubscribe returns, the callback is
    // never invoked again.
    [[nodiscard]] uint64_t Subscribe(FrameCallback cb);
    void Unsubscribe(uint64_t token);

    // One step of the capture loop: poll, or retry a reopen. Cheap and
    // non-blocking when there is nothing to own.
    void Pump();

    [[nodiscard]] recorder_core::HubFrameKind Frame() const;
    [[nodiscard]] HubFrame HeldFrame() const;

    // Zero means nobody is watching: the capture is closed and the hub may be
    // discarded. The registry's disposal rule reads this.
    [[nodiscard]] int ConsumerCount() const {
        return state_.consumers;
    }

    // The recording engine is about to capture this source. Returns true once
    // the hub's own capture is released and the engine may open its own. A
    // display may only be duplicated once per process, so this ordering is the
    // whole point.
    bool RequestLease();
    void ReturnLease();

    // While leased, the engine's composited frames arrive here and are fanned
    // out, so consumers see the recorded image rather than a still.
    void ForwardFrame(const HubFrame& frame);

    [[nodiscard]] recorder_core::CaptureHubState StateForTest() const {
        return state_;
    }

  private:
    // Steps the policy, adopts the new state and performs the actions it asked
    // for. Returns the decision so callers can read grant_lease.
    recorder_core::CaptureHubDecision Apply(recorder_core::CaptureHubEvent event);
    void OpenProducer();
    void CloseProducer();
    void Deliver(const HubFrame& frame);

    std::unique_ptr<HubSourceProducer> producer_;
    bool producer_open_ = false;

    recorder_core::CaptureHubState state_{};
    HubFrame held_{};

    mutable std::mutex subscribers_mutex_;
    struct Subscriber {
        uint64_t token;
        FrameCallback cb;
    };
    std::vector<Subscriber> subscribers_;
    uint64_t next_token_ = 1;
};

} // namespace exosnap
