#pragma once

// Pure arbitration for a capture hub *service* -- the layer above
// CaptureSourceHub, where the pump thread turns posted commands into registry
// calls. No D3D, no WGC, no Qt, no threads.
//
// Why this exists as its own state machine: the one-duplication-per-process
// rule is enforced per hub instance by capture_hub_policy.h, but a service's
// Subscribe handler drops the old subscription first, which disposes the hub
// and takes its lease_out flag with it. A Subscribe processed while the engine
// holds the lease therefore built a *fresh* hub whose state said "nobody owns
// this output" and reopened the duplication behind the engine's back.
//
// The gate holds the lease at service level, where no hub disposal can erase
// it, and answers one question per command: what may the pump thread do now.
// The single invariant it exists to make provable:
//
//   `apply_subscription` is never emitted while `next.leased` is true.
//
// Both hub services drive this same resolver, so DXGI and WGC cannot drift
// apart semantically.

#include <cstdint>

namespace exosnap {

enum class CaptureHubOp {
    Subscribe,
    Unsubscribe,
    LeaseRequest,
    LeaseReturn,
};

struct CaptureHubGateState {
    // The recording engine holds the lease: this service owns no capture and
    // must open none until the lease comes back.
    bool leased = false;

    // A registry subscription is live right now.
    bool subscribed = false;

    // A Subscribe arrived while leased. Its payload waits for the lease return
    // rather than being dropped (the old single-slot mailbox lost it) or
    // applied immediately (which is what reopened the duplication).
    bool deferred = false;
};

struct CaptureHubGateAction {
    CaptureHubGateState next;

    // Drop the live registry subscription and the publisher state that belongs
    // to it. Always paired with whatever replaces it, never on its own.
    bool drop_subscription = false;

    // Create the registry subscription from the pump thread's desired payload.
    bool apply_subscription = false;

    bool release_lease = false; // registry.RequestLease(current key)
    bool return_lease = false;  // registry.ReturnLease(current key)

    // Publish this command's serial as the lease acknowledgement. Emitted only
    // for LeaseRequest, and only once this service's capture is provably gone.
    bool acknowledge_release = false;

    // Reset the shared-texture publisher: the producer's device does not
    // survive a close, so the tap must be rebuilt on the next frame.
    bool reset_publisher = false;
};

CaptureHubGateAction StepCaptureHubGate(CaptureHubGateState state, CaptureHubOp op) noexcept;

} // namespace exosnap
