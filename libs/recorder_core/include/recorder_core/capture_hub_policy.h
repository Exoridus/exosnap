#pragma once

// Pure arbitration for a capture hub. No D3D, no WGC, no Qt, no threads.
//
// A hub owns exactly one capture per source key and fans it out to N consumers.
// Sharing is the mechanism; frame control is the point. Because the hub owns the
// only capture, it is the only party that can tell "the source produced nothing"
// apart from "someone took the source away" -- and therefore the only party that
// can hold the last good frame instead of handing out an empty surface.
//
// Two rules justify this file's existence:
//
//   Loss is held, not blanked. A consumer sees a still image, never an empty
//   one. Reopen retries are unbounded; only the last consumer leaving stops them.
//
//   A display may be duplicated once per process (measured; DXGI answers
//   E_INVALIDARG for the second attempt, on any device). So before the recording
//   engine opens its duplication, the hub's must be gone. StepCaptureHub emits
//   close_capture and grant_lease from a single decision, so no interleaving can
//   put them in the wrong order.
//
// The hub classes are thin drivers of StepCaptureHub: apply `next`, then perform
// the flagged actions. They decide nothing themselves. That binding is what makes
// test_capture_hub_policy.cpp a pin on real behaviour rather than on a model of it.

namespace recorder_core {

// What a consumer is handed when it asks for the current frame.
enum class HubFrameKind {
    None, // nothing has ever been captured; there is no frame to hold
    Held, // the last good frame; the source is not producing right now
    Live, // the source is producing
};

struct CaptureHubState {
    int consumers = 0;

    // The hub owns a capture right now. Mutually exclusive with lease_out: the
    // one-duplication-per-process constraint permits no overlap.
    bool capture_open = false;

    // The recording engine owns the capture. The hub forwards the engine's
    // composited frames through the existing NT-handle tap, where one exists.
    bool lease_out = false;

    // The hub is not producing live frames: the source died and is being
    // reopened, or the capture was handed to the engine, or it was just
    // reopened and has not delivered a frame yet. Consumers see the held frame.
    bool holding = false;

    // Given up on the source (DEVICE_REMOVED and friends). No retry loop runs
    // against a dead GPU. The held frame is still served -- lost is not blank.
    bool lost = false;

    // A frame has been captured at least once, so a hold is possible. Cleared
    // only when the last consumer leaves, never on loss.
    bool has_frame = false;
};

enum class CaptureHubEvent {
    ConsumerAdded,
    ConsumerRemoved,

    // The source delivered a frame -- captured by the hub, or forwarded from the
    // engine's tap while leased. Both end a hold.
    FrameReceived,

    // Recoverable: the window was minimised, WGC blanked, the display is
    // renegotiating, the monitor was unplugged. Classified upstream by
    // ClassifyOdAcquireFailure (Recover) and the webcam's equivalent.
    SourceLost,

    ReopenSucceeded, // the capture is open again, but has produced nothing yet
    ReopenFailed,    // retry again; the budget is unbounded

    // Unrecoverable: DEVICE_REMOVED / HUNG / RESET, i.e. ClassifyOdAcquireFailure
    // reporting Fail. Stop retrying.
    SourceUnrecoverable,

    LeaseRequested, // the engine is about to record this source
    LeaseReturned,  // the engine has stopped and released its capture
};

// What the driver must do after applying `next`. Perform close_capture before
// grant_lease; the rest are mutually exclusive by construction.
struct CaptureHubDecision {
    CaptureHubState next;

    bool open_capture = false;
    bool close_capture = false;
    bool schedule_reopen = false;
    bool grant_lease = false;
};

CaptureHubDecision StepCaptureHub(CaptureHubState state, CaptureHubEvent event) noexcept;

HubFrameKind ResolveHubFrame(const CaptureHubState& state) noexcept;

} // namespace recorder_core
