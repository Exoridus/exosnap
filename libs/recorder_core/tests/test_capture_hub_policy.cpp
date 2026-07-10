// Pure arbitration for a capture hub: who owns the capture, when it opens and
// closes, what a consumer sees while the source is not producing, and the order
// in which a display is handed to the recording engine.
//
// No D3D, no WGC, no threads. Every rule the hubs enforce at runtime is decided
// by StepCaptureHub, and every one of them is pinned here. The hub classes are
// thin drivers of this function -- if they ever decide something themselves,
// these tests stop meaning anything.

#include <gtest/gtest.h>

#include <recorder_core/capture_hub_policy.h>

namespace {

using namespace recorder_core;

// Drives a sequence of events through the policy, returning the final state.
// Mirrors how the hubs call it: apply the decision, perform the actions.
CaptureHubState Drive(CaptureHubState state, std::initializer_list<CaptureHubEvent> events) {
    for (CaptureHubEvent e : events)
        state = StepCaptureHub(state, e).next;
    return state;
}

CaptureHubState WithLiveConsumer() {
    return Drive({}, {CaptureHubEvent::ConsumerAdded, CaptureHubEvent::FrameReceived});
}

// --- consumer counting -----------------------------------------------------

TEST(CaptureHubPolicy, FirstConsumerOpensTheCapture) {
    const CaptureHubDecision d = StepCaptureHub({}, CaptureHubEvent::ConsumerAdded);

    EXPECT_TRUE(d.open_capture);
    EXPECT_TRUE(d.next.capture_open);
    EXPECT_EQ(d.next.consumers, 1);
}

TEST(CaptureHubPolicy, SecondConsumerDoesNotOpenASecondCapture) {
    const CaptureHubState one = Drive({}, {CaptureHubEvent::ConsumerAdded});
    const CaptureHubDecision d = StepCaptureHub(one, CaptureHubEvent::ConsumerAdded);

    EXPECT_FALSE(d.open_capture);
    EXPECT_EQ(d.next.consumers, 2);
    EXPECT_TRUE(d.next.capture_open);
}

TEST(CaptureHubPolicy, CaptureClosesOnlyWhenTheLastConsumerLeaves) {
    const CaptureHubState two = Drive({}, {CaptureHubEvent::ConsumerAdded, CaptureHubEvent::ConsumerAdded});

    const CaptureHubDecision first_leaves = StepCaptureHub(two, CaptureHubEvent::ConsumerRemoved);
    EXPECT_FALSE(first_leaves.close_capture);
    EXPECT_TRUE(first_leaves.next.capture_open);

    const CaptureHubDecision last_leaves = StepCaptureHub(first_leaves.next, CaptureHubEvent::ConsumerRemoved);
    EXPECT_TRUE(last_leaves.close_capture);
    EXPECT_FALSE(last_leaves.next.capture_open);
    EXPECT_EQ(last_leaves.next.consumers, 0);
}

// With the preview switched off there is no consumer, so no duplication exists.
// This is the whole mitigation for the idle-duplication risk -- pinned, not hoped.
TEST(CaptureHubPolicy, NoConsumersMeansNoCaptureAndNoHeldFrame) {
    const CaptureHubState after = Drive(WithLiveConsumer(), {CaptureHubEvent::ConsumerRemoved});

    EXPECT_FALSE(after.capture_open);
    EXPECT_FALSE(after.has_frame);
    EXPECT_EQ(ResolveHubFrame(after), HubFrameKind::None);
}

// --- the hold --------------------------------------------------------------

TEST(CaptureHubPolicy, BeforeTheFirstFrameThereIsNothingToHold) {
    const CaptureHubState opened = Drive({}, {CaptureHubEvent::ConsumerAdded});
    EXPECT_EQ(ResolveHubFrame(opened), HubFrameKind::None);

    // A source that dies before ever producing still has nothing to hand out.
    const CaptureHubState lost = Drive(opened, {CaptureHubEvent::SourceLost});
    EXPECT_EQ(ResolveHubFrame(lost), HubFrameKind::None);
}

TEST(CaptureHubPolicy, AConsumerSeesLiveFramesWhileTheSourceProduces) {
    EXPECT_EQ(ResolveHubFrame(WithLiveConsumer()), HubFrameKind::Live);
}

// The Snipping Tool takes the WGC surface; the monitor is unplugged. Same shape.
TEST(CaptureHubPolicy, ALostSourceYieldsTheHeldFrameNeverAnEmptyOne) {
    const CaptureHubDecision d = StepCaptureHub(WithLiveConsumer(), CaptureHubEvent::SourceLost);

    EXPECT_TRUE(d.schedule_reopen);
    EXPECT_TRUE(d.next.holding);
    EXPECT_TRUE(d.next.has_frame) << "loss must never clear the held frame";
    EXPECT_EQ(ResolveHubFrame(d.next), HubFrameKind::Held);
}

TEST(CaptureHubPolicy, TheHeldFrameSurvivesUnboundedFailedReopens) {
    CaptureHubState s = Drive(WithLiveConsumer(), {CaptureHubEvent::SourceLost});

    for (int i = 0; i < 1000; ++i) {
        const CaptureHubDecision d = StepCaptureHub(s, CaptureHubEvent::ReopenFailed);
        EXPECT_TRUE(d.schedule_reopen) << "retry is unbounded; only a consumer leaving stops it";
        s = d.next;
    }

    EXPECT_TRUE(s.holding);
    EXPECT_EQ(ResolveHubFrame(s), HubFrameKind::Held);
}

// Reopening the duplication is not the same as producing again. Until a frame
// actually arrives the consumer keeps the held image -- otherwise the preview
// would flash empty in the window between reopen and first frame.
TEST(CaptureHubPolicy, ReopenAloneDoesNotEndTheHold) {
    const CaptureHubState reopened =
        Drive(WithLiveConsumer(), {CaptureHubEvent::SourceLost, CaptureHubEvent::ReopenSucceeded});

    EXPECT_TRUE(reopened.holding);
    EXPECT_EQ(ResolveHubFrame(reopened), HubFrameKind::Held);

    const CaptureHubState producing = Drive(reopened, {CaptureHubEvent::FrameReceived});
    EXPECT_FALSE(producing.holding);
    EXPECT_EQ(ResolveHubFrame(producing), HubFrameKind::Live);
}

TEST(CaptureHubPolicy, GivingUpOnTheSourceStillHoldsTheLastFrame) {
    const CaptureHubDecision d =
        StepCaptureHub(Drive(WithLiveConsumer(), {CaptureHubEvent::SourceLost}), CaptureHubEvent::SourceUnrecoverable);

    EXPECT_TRUE(d.close_capture) << "no retry loop against a dead GPU";
    EXPECT_FALSE(d.schedule_reopen);
    EXPECT_TRUE(d.next.lost);
    EXPECT_EQ(ResolveHubFrame(d.next), HubFrameKind::Held) << "lost is still not blank";
}

// --- handing a display to the engine ---------------------------------------

// The one rule the measured constraint forces: the hub's duplication is gone
// before the engine's is opened. Both facts in one decision, so no interleaving
// can put them in the wrong order.
TEST(CaptureHubPolicy, TheHubReleasesBeforeTheEngineIsAllowedToOpen) {
    const CaptureHubDecision d = StepCaptureHub(WithLiveConsumer(), CaptureHubEvent::LeaseRequested);

    EXPECT_TRUE(d.close_capture);
    EXPECT_TRUE(d.grant_lease);
    EXPECT_FALSE(d.open_capture);
    EXPECT_FALSE(d.next.capture_open) << "the engine may not open a second duplication";
    EXPECT_TRUE(d.next.lease_out);
}

TEST(CaptureHubPolicy, ConsumersKeepTheHeldFrameAcrossTheHandover) {
    const CaptureHubState leased = Drive(WithLiveConsumer(), {CaptureHubEvent::LeaseRequested});

    EXPECT_TRUE(leased.has_frame);
    EXPECT_EQ(ResolveHubFrame(leased), HubFrameKind::Held);
}

// Step 4 of the handover: the engine publishes its composited frame through the
// existing tap, the hub forwards it. Consumers go live again without the hub
// owning a capture.
TEST(CaptureHubPolicy, ForwardedEngineFramesMakeConsumersLiveWhileLeased) {
    const CaptureHubState forwarding =
        Drive(WithLiveConsumer(), {CaptureHubEvent::LeaseRequested, CaptureHubEvent::FrameReceived});

    EXPECT_TRUE(forwarding.lease_out);
    EXPECT_FALSE(forwarding.capture_open);
    EXPECT_EQ(ResolveHubFrame(forwarding), HubFrameKind::Live);
}

// Native HDR10 has no tap, so no frame is ever forwarded. The hub holds for the
// whole recording rather than blanking.
TEST(CaptureHubPolicy, ALeaseWithNoForwardedFramesHoldsForItsWholeDuration) {
    const CaptureHubState leased = Drive(WithLiveConsumer(), {CaptureHubEvent::LeaseRequested});
    EXPECT_EQ(ResolveHubFrame(leased), HubFrameKind::Held);
}

TEST(CaptureHubPolicy, ReclaimAfterStopRestoresProduction) {
    const CaptureHubState leased = Drive(WithLiveConsumer(), {CaptureHubEvent::LeaseRequested});
    const CaptureHubDecision d = StepCaptureHub(leased, CaptureHubEvent::LeaseReturned);

    EXPECT_TRUE(d.open_capture);
    EXPECT_TRUE(d.next.capture_open);
    EXPECT_FALSE(d.next.lease_out);
    EXPECT_EQ(ResolveHubFrame(d.next), HubFrameKind::Held) << "held until the reopened source produces";

    EXPECT_EQ(ResolveHubFrame(Drive(d.next, {CaptureHubEvent::FrameReceived})), HubFrameKind::Live);
}

// The user closed the preview during the recording. Nothing to reclaim for.
TEST(CaptureHubPolicy, ReclaimWithNoConsumersLeftDoesNotReopen) {
    const CaptureHubState leased_then_abandoned =
        Drive(WithLiveConsumer(), {CaptureHubEvent::LeaseRequested, CaptureHubEvent::ConsumerRemoved});
    const CaptureHubDecision d = StepCaptureHub(leased_then_abandoned, CaptureHubEvent::LeaseReturned);

    EXPECT_FALSE(d.open_capture);
    EXPECT_FALSE(d.next.capture_open);
}

// Recording starts while the display is renegotiating. The hub owns a dead
// duplication; it must drop it and grant anyway -- the engine's own open and
// recovery take over from there.
TEST(CaptureHubPolicy, ALeaseRequestedWhileHoldingDropsTheDeadDuplicationAndGrants) {
    const CaptureHubState holding = Drive(WithLiveConsumer(), {CaptureHubEvent::SourceLost});
    const CaptureHubDecision d = StepCaptureHub(holding, CaptureHubEvent::LeaseRequested);

    EXPECT_TRUE(d.close_capture);
    EXPECT_TRUE(d.grant_lease);
    EXPECT_FALSE(d.schedule_reopen) << "the hub stops retrying a source it no longer owns";
    EXPECT_TRUE(d.next.lease_out);
    EXPECT_EQ(ResolveHubFrame(d.next), HubFrameKind::Held);
}

// A consumer leaving mid-recording must not close a capture the hub does not own.
TEST(CaptureHubPolicy, TheLastConsumerLeavingWhileLeasedClosesNothing) {
    const CaptureHubState leased = Drive(WithLiveConsumer(), {CaptureHubEvent::LeaseRequested});
    const CaptureHubDecision d = StepCaptureHub(leased, CaptureHubEvent::ConsumerRemoved);

    EXPECT_FALSE(d.close_capture) << "the engine owns the duplication, not the hub";
    EXPECT_TRUE(d.next.lease_out);
    EXPECT_EQ(d.next.consumers, 0);
}

// A hub that never had a consumer is asked for a lease: nothing to release.
TEST(CaptureHubPolicy, ALeaseOnAClosedHubGrantsWithoutClosing) {
    const CaptureHubDecision d = StepCaptureHub({}, CaptureHubEvent::LeaseRequested);

    EXPECT_FALSE(d.close_capture);
    EXPECT_TRUE(d.grant_lease);
    EXPECT_TRUE(d.next.lease_out);
}

// A consumer subscribing during a recording joins the forwarded stream; it must
// not open a duplication behind the engine's back.
TEST(CaptureHubPolicy, AConsumerArrivingDuringALeaseDoesNotOpenACapture) {
    const CaptureHubState leased = Drive(WithLiveConsumer(), {CaptureHubEvent::LeaseRequested});
    const CaptureHubDecision d = StepCaptureHub(leased, CaptureHubEvent::ConsumerAdded);

    EXPECT_FALSE(d.open_capture);
    EXPECT_FALSE(d.next.capture_open);
    EXPECT_EQ(d.next.consumers, 2);
}

} // namespace
