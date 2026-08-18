#include "services/CaptureHubGate.h"

namespace exosnap {

CaptureHubGateAction StepCaptureHubGate(CaptureHubGateState state, CaptureHubOp op) noexcept {
    CaptureHubGateAction action{};
    action.next = state;

    switch (op) {
    case CaptureHubOp::Subscribe:
        // The old subscription always goes first: one duplication per output,
        // ever, so nothing new may open while the previous one is still held.
        action.drop_subscription = true;
        action.reset_publisher = true;
        if (state.leased) {
            // The engine owns the capture. Remember the request instead of
            // serving it -- dropping it silently is what the single-slot
            // mailbox did, and serving it is what reopened the duplication.
            action.next.subscribed = false;
            action.next.deferred = true;
        } else {
            action.apply_subscription = true;
            action.next.subscribed = true;
            action.next.deferred = false;
        }
        break;

    case CaptureHubOp::Unsubscribe:
        // An explicit unsubscribe also revokes a deferred one: the caller no
        // longer wants this preview, and the lease return must not resurrect it.
        action.drop_subscription = true;
        action.reset_publisher = true;
        action.next.subscribed = false;
        action.next.deferred = false;
        break;

    case CaptureHubOp::LeaseRequest:
        // The subscription survives the lease: the hub keeps handing consumers
        // its held frame while the engine records. Only the capture goes.
        action.release_lease = state.subscribed;
        action.reset_publisher = true;
        action.next.leased = true;
        // Acknowledged unconditionally, including when nothing was subscribed:
        // "this service holds no duplication of that output" is true either
        // way, and that is the whole claim the waiting engine thread needs.
        action.acknowledge_release = true;
        break;

    case CaptureHubOp::LeaseReturn:
        action.next.leased = false;
        if (state.subscribed) {
            action.return_lease = true;
        } else if (state.deferred) {
            // The Subscribe that arrived under the lease, served now that the
            // engine's capture is gone.
            action.apply_subscription = true;
            action.next.subscribed = true;
            action.next.deferred = false;
        }
        break;
    }

    return action;
}

} // namespace exosnap
