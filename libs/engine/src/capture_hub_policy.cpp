#include <exosnap/engine/capture_hub_policy.h>

namespace exosnap::engine {

CaptureHubDecision StepCaptureHub(CaptureHubState state, CaptureHubEvent event) noexcept {
    CaptureHubDecision d{};
    d.next = state;

    switch (event) {
    case CaptureHubEvent::ConsumerAdded:
        d.next.consumers += 1;
        // Only the first consumer opens, and only if nobody else owns the
        // capture: while the engine holds the lease the hub must not duplicate
        // behind its back, and a source we have given up on is not retried.
        if (d.next.consumers == 1 && !state.capture_open && !state.lease_out && !state.lost) {
            d.open_capture = true;
            d.next.capture_open = true;
            d.next.holding = true; // opened, nothing captured yet
        }
        break;

    case CaptureHubEvent::ConsumerRemoved:
        if (d.next.consumers > 0)
            d.next.consumers -= 1;
        if (d.next.consumers == 0) {
            // The engine's duplication is not ours to close.
            if (state.capture_open) {
                d.close_capture = true;
                d.next.capture_open = false;
            }
            if (!d.next.lease_out) {
                d.next.holding = false;
                d.next.lost = false;
                d.next.has_frame = false; // nobody left to hold it for
            }
        }
        break;

    case CaptureHubEvent::FrameReceived:
        d.next.has_frame = true;
        d.next.holding = false;
        d.next.lost = false;
        break;

    case CaptureHubEvent::SourceLost:
        // The held frame is deliberately untouched: it is what consumers see
        // until the source produces again, however long that takes.
        if (state.capture_open) {
            d.next.holding = true;
            d.schedule_reopen = true;
        }
        break;

    case CaptureHubEvent::ReopenSucceeded:
        // Owning a capture again is not the same as producing. Stay held until a
        // frame actually arrives, or the preview flashes empty in between.
        break;

    case CaptureHubEvent::ReopenFailed:
        if (state.capture_open)
            d.schedule_reopen = true; // unbounded; only the last consumer leaving stops it
        break;

    case CaptureHubEvent::SourceUnrecoverable:
        if (state.capture_open) {
            d.close_capture = true;
            d.next.capture_open = false;
        }
        d.next.holding = true;
        d.next.lost = true;
        break;

    case CaptureHubEvent::LeaseRequested:
        // Release before the engine opens -- both facts in one decision, so no
        // interleaving can duplicate the output twice. A dead duplication we were
        // still retrying is simply dropped; the engine's own recovery takes over.
        if (state.capture_open) {
            d.close_capture = true;
            d.next.capture_open = false;
        }
        d.next.lease_out = true;
        d.next.holding = true; // until the engine's tap forwards a frame
        d.next.lost = false;
        d.grant_lease = true;
        break;

    case CaptureHubEvent::LeaseReturned:
        d.next.lease_out = false;
        if (d.next.consumers > 0) {
            d.open_capture = true;
            d.next.capture_open = true;
            d.next.holding = true; // reopened; held until it produces
        } else {
            d.next.holding = false;
            d.next.has_frame = false;
        }
        break;
    }

    return d;
}

HubFrameKind ResolveHubFrame(const CaptureHubState& state) noexcept {
    if (!state.has_frame)
        return HubFrameKind::None;
    if (state.holding || state.lost)
        return HubFrameKind::Held;
    if (!state.capture_open && !state.lease_out)
        return HubFrameKind::Held;
    return HubFrameKind::Live;
}

} // namespace exosnap::engine
