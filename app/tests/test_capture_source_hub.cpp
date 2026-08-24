// CaptureSourceHub drives StepCaptureHub. These tests exist to prove exactly
// that: every open, close, reopen and hold the hub performs is one the policy
// asked for. The policy's own rules are pinned in
// libs/engine/tests/test_capture_hub_policy.cpp and are not restated here.
//
// A FakeProducer stands in for the WGC session, so the loop runs with no GPU, no
// WinRT and no threads. Pump() is stepped by hand, which is the only reason the
// reopen and hold behaviour is observable at all -- the webcam's equivalent loop
// is untestable today for want of this seam.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "services/CaptureSourceHub.h"

namespace {

using namespace exosnap;
using exosnap::engine::HubFrameKind;

class FakeProducer : public HubSourceProducer {
  public:
    int open_calls = 0;
    int close_calls = 0;
    bool open_succeeds = true;
    bool is_open = false;

    // What the next PollFrame returns. Frame delivery bumps `generation`.
    ProducerPoll next_poll = ProducerPoll::NoFrame;
    uint64_t generation = 0;

    bool Open(std::string& err) override {
        ++open_calls;
        if (!open_succeeds) {
            err = "fake: refused";
            return false;
        }
        is_open = true;
        return true;
    }

    void Close() override {
        ++close_calls;
        is_open = false;
    }

    ProducerPoll PollFrame(HubFrame& out) override {
        if (next_poll == ProducerPoll::Frame) {
            out.width = 1920;
            out.height = 1080;
            out.generation = ++generation;
        }
        return next_poll;
    }
};

struct Fixture {
    FakeProducer* fake = nullptr;
    std::unique_ptr<CaptureSourceHub> hub;

    Fixture() {
        auto producer = std::make_unique<FakeProducer>();
        fake = producer.get();
        hub = std::make_unique<CaptureSourceHub>(std::move(producer));
    }

    // Subscribes a consumer that records every frame it is handed.
    uint64_t SubscribeRecording(std::vector<uint64_t>& seen) {
        return hub->Subscribe([&seen](const HubFrame& f, HubFrameKind) { seen.push_back(f.generation); });
    }

    void DeliverFrame() {
        fake->next_poll = ProducerPoll::Frame;
        hub->Pump();
        fake->next_poll = ProducerPoll::NoFrame;
    }
};

// --- the capture follows the consumers -------------------------------------

TEST(CaptureSourceHub, TheFirstConsumerOpensTheProducerAndTheLastClosesIt) {
    Fixture f;
    EXPECT_EQ(f.fake->open_calls, 0) << "an idle hub owns no capture";

    const uint64_t a = f.hub->Subscribe([](const HubFrame&, HubFrameKind) {});
    EXPECT_EQ(f.fake->open_calls, 1);
    EXPECT_TRUE(f.fake->is_open);

    const uint64_t b = f.hub->Subscribe([](const HubFrame&, HubFrameKind) {});
    EXPECT_EQ(f.fake->open_calls, 1) << "the source is captured once, not once per consumer";

    f.hub->Unsubscribe(a);
    EXPECT_TRUE(f.fake->is_open);

    f.hub->Unsubscribe(b);
    EXPECT_EQ(f.fake->close_calls, 1);
    EXPECT_FALSE(f.fake->is_open);
}

// Turn the preview off and nothing is captured. For the DXGI hub this is what
// keeps an idle duplication from existing at all.
TEST(CaptureSourceHub, PumpingWithNoConsumersNeverTouchesTheProducer) {
    Fixture f;
    for (int i = 0; i < 10; ++i)
        f.hub->Pump();

    EXPECT_EQ(f.fake->open_calls, 0);
}

TEST(CaptureSourceHub, AProducerThatFailsToOpenIsRetriedUntilItSucceeds) {
    Fixture f;
    f.fake->open_succeeds = false;

    (void)f.hub->Subscribe([](const HubFrame&, HubFrameKind) {});
    EXPECT_EQ(f.fake->open_calls, 1);

    for (int i = 0; i < 5; ++i)
        f.hub->Pump();
    EXPECT_EQ(f.fake->open_calls, 6) << "a source that will not open is retried, not abandoned";
    EXPECT_EQ(f.hub->Frame(), HubFrameKind::None) << "nothing was ever captured";

    f.fake->open_succeeds = true;
    f.hub->Pump();
    EXPECT_TRUE(f.fake->is_open);
}

// --- frames and the hold ---------------------------------------------------

TEST(CaptureSourceHub, ConsumersReceiveFramesAndSeeThemLive) {
    Fixture f;
    std::vector<uint64_t> seen;
    f.SubscribeRecording(seen);

    f.DeliverFrame();
    f.DeliverFrame();

    EXPECT_EQ(seen, (std::vector<uint64_t>{1, 2}));
    EXPECT_EQ(f.hub->Frame(), HubFrameKind::Live);
    EXPECT_EQ(f.hub->HeldFrame().generation, 2u);
}

TEST(CaptureSourceHub, PollingNothingIsNotLoss) {
    Fixture f;
    std::vector<uint64_t> seen;
    f.SubscribeRecording(seen);
    f.DeliverFrame();

    f.fake->next_poll = ProducerPoll::NoFrame;
    for (int i = 0; i < 5; ++i)
        f.hub->Pump();

    EXPECT_EQ(seen.size(), 1u) << "a quiet source delivers no callbacks";
    EXPECT_EQ(f.hub->Frame(), HubFrameKind::Live) << "and is still live";
    EXPECT_EQ(f.fake->close_calls, 0);
}

// The Snipping Tool takes the WGC surface. The consumer keeps the last frame.
TEST(CaptureSourceHub, ALostSourceHoldsTheLastFrameAndIsReopenedUnderneath) {
    Fixture f;
    std::vector<uint64_t> seen;
    f.SubscribeRecording(seen);
    f.DeliverFrame();

    f.fake->next_poll = ProducerPoll::Lost;
    f.hub->Pump();

    EXPECT_EQ(f.hub->Frame(), HubFrameKind::Held);
    EXPECT_EQ(f.hub->HeldFrame().generation, 1u) << "the held frame is the last good one";
    EXPECT_EQ(f.fake->close_calls, 1) << "the dead session is dropped";

    // Reopen fails for a while; the hold persists and retries never stop.
    f.fake->open_succeeds = false;
    f.fake->next_poll = ProducerPoll::NoFrame;
    const int before = f.fake->open_calls;
    for (int i = 0; i < 50; ++i)
        f.hub->Pump();
    EXPECT_EQ(f.fake->open_calls, before + 50) << "retry is unbounded";
    EXPECT_EQ(f.hub->Frame(), HubFrameKind::Held);
    EXPECT_EQ(seen.size(), 1u) << "a held frame is not re-delivered as if it were new";

    // Reopening is not producing: still held until a frame actually arrives.
    f.fake->open_succeeds = true;
    f.hub->Pump();
    EXPECT_EQ(f.hub->Frame(), HubFrameKind::Held);

    f.DeliverFrame();
    EXPECT_EQ(f.hub->Frame(), HubFrameKind::Live);
    EXPECT_EQ(seen, (std::vector<uint64_t>{1, 2}));
}

TEST(CaptureSourceHub, AnUnrecoverableSourceStopsRetryingButStillHolds) {
    Fixture f;
    (void)f.hub->Subscribe([](const HubFrame&, HubFrameKind) {});
    f.DeliverFrame();

    f.fake->next_poll = ProducerPoll::Fatal;
    f.hub->Pump();

    const int opens = f.fake->open_calls;
    for (int i = 0; i < 10; ++i)
        f.hub->Pump();

    EXPECT_EQ(f.fake->open_calls, opens) << "no retry loop against a dead GPU";
    EXPECT_EQ(f.hub->Frame(), HubFrameKind::Held) << "lost is still not blank";
}

TEST(CaptureSourceHub, LosingTheSourceBeforeAnyFrameHasNothingToHold) {
    Fixture f;
    (void)f.hub->Subscribe([](const HubFrame&, HubFrameKind) {});

    f.fake->next_poll = ProducerPoll::Lost;
    f.hub->Pump();

    EXPECT_EQ(f.hub->Frame(), HubFrameKind::None);
}

TEST(CaptureSourceHub, TheLastConsumerLeavingDropsTheHeldFrame) {
    Fixture f;
    const uint64_t token = f.hub->Subscribe([](const HubFrame&, HubFrameKind) {});
    f.DeliverFrame();
    ASSERT_EQ(f.hub->Frame(), HubFrameKind::Live);

    f.hub->Unsubscribe(token);
    EXPECT_EQ(f.hub->Frame(), HubFrameKind::None);
}

// --- subscription lifetime -------------------------------------------------

// The reason this hub hands out tokens rather than storing a raw callback: a
// consumer that has unsubscribed must never be called again. WebcamService's
// callback captures a raw `this` and outlives its page; a hub must not.
TEST(CaptureSourceHub, AnUnsubscribedConsumerIsNeverCalledAgain) {
    Fixture f;
    std::vector<uint64_t> stays;
    std::vector<uint64_t> leaves;

    f.SubscribeRecording(stays);
    const uint64_t going = f.SubscribeRecording(leaves);

    f.DeliverFrame();
    f.hub->Unsubscribe(going);
    f.DeliverFrame();

    EXPECT_EQ(stays, (std::vector<uint64_t>{1, 2}));
    EXPECT_EQ(leaves, (std::vector<uint64_t>{1})) << "no frame after unsubscribe";
}

TEST(CaptureSourceHub, UnsubscribingAnUnknownTokenIsHarmless) {
    Fixture f;
    const uint64_t token = f.hub->Subscribe([](const HubFrame&, HubFrameKind) {});

    f.hub->Unsubscribe(token);
    f.hub->Unsubscribe(token); // twice
    f.hub->Unsubscribe(9999);

    EXPECT_EQ(f.fake->close_calls, 1) << "the capture is not closed twice";
}

// --- handing the source to the recording engine ----------------------------

TEST(CaptureSourceHub, TakingTheLeaseClosesTheHubsCaptureAndHolds) {
    Fixture f;
    (void)f.hub->Subscribe([](const HubFrame&, HubFrameKind) {});
    f.DeliverFrame();

    EXPECT_TRUE(f.hub->RequestLease()) << "the engine may open its own capture now";
    EXPECT_FALSE(f.fake->is_open) << "released before the engine opens";
    EXPECT_EQ(f.fake->close_calls, 1);
    EXPECT_EQ(f.hub->Frame(), HubFrameKind::Held);
}

TEST(CaptureSourceHub, PumpingWhileLeasedNeverTouchesTheProducer) {
    Fixture f;
    (void)f.hub->Subscribe([](const HubFrame&, HubFrameKind) {});
    f.DeliverFrame();
    f.hub->RequestLease();

    const int opens = f.fake->open_calls;
    for (int i = 0; i < 10; ++i)
        f.hub->Pump();

    EXPECT_EQ(f.fake->open_calls, opens) << "the engine owns the source; the hub must not duplicate it";
}

// The engine publishes its composited frame through the existing tap; the hub
// forwards it, so the preview shows cursor and webcam rather than a still.
TEST(CaptureSourceHub, ForwardedEngineFramesReachConsumersAndAreLive) {
    Fixture f;
    std::vector<uint64_t> seen;
    f.SubscribeRecording(seen);
    f.DeliverFrame();
    f.hub->RequestLease();
    ASSERT_EQ(f.hub->Frame(), HubFrameKind::Held);

    HubFrame engine_frame;
    engine_frame.generation = 77;
    f.hub->ForwardFrame(engine_frame);

    EXPECT_EQ(f.hub->Frame(), HubFrameKind::Live);
    EXPECT_EQ(seen, (std::vector<uint64_t>{1, 77}));
}

// Native HDR10 has no tap: nothing is ever forwarded, so the hold simply lasts.
TEST(CaptureSourceHub, ALeaseThatForwardsNothingHoldsThroughout) {
    Fixture f;
    (void)f.hub->Subscribe([](const HubFrame&, HubFrameKind) {});
    f.DeliverFrame();
    f.hub->RequestLease();

    for (int i = 0; i < 10; ++i)
        f.hub->Pump();

    EXPECT_EQ(f.hub->Frame(), HubFrameKind::Held);
}

TEST(CaptureSourceHub, ReturningTheLeaseReopensAndProducesAgain) {
    Fixture f;
    std::vector<uint64_t> seen;
    f.SubscribeRecording(seen);
    f.DeliverFrame();
    f.hub->RequestLease();

    f.hub->ReturnLease();
    EXPECT_TRUE(f.fake->is_open);
    EXPECT_EQ(f.hub->Frame(), HubFrameKind::Held) << "held until the reopened source produces";

    f.DeliverFrame();
    EXPECT_EQ(f.hub->Frame(), HubFrameKind::Live);
}

TEST(CaptureSourceHub, ReturningTheLeaseWithNoConsumersLeftReopensNothing) {
    Fixture f;
    const uint64_t token = f.hub->Subscribe([](const HubFrame&, HubFrameKind) {});
    f.DeliverFrame();
    f.hub->RequestLease();

    f.hub->Unsubscribe(token); // the user closed the preview mid-recording
    const int opens = f.fake->open_calls;

    f.hub->ReturnLease();
    EXPECT_EQ(f.fake->open_calls, opens);
    EXPECT_FALSE(f.fake->is_open);
}

// Recording starts while the display is renegotiating: the hub is holding a dead
// session. It must drop it and grant anyway.
TEST(CaptureSourceHub, ALeaseTakenWhileHoldingDropsTheDeadSession) {
    Fixture f;
    (void)f.hub->Subscribe([](const HubFrame&, HubFrameKind) {});
    f.DeliverFrame();
    f.fake->next_poll = ProducerPoll::Lost;
    f.hub->Pump();
    ASSERT_EQ(f.hub->Frame(), HubFrameKind::Held);

    EXPECT_TRUE(f.hub->RequestLease());

    const int opens = f.fake->open_calls;
    for (int i = 0; i < 10; ++i)
        f.hub->Pump();
    EXPECT_EQ(f.fake->open_calls, opens) << "the hub stops retrying a source it no longer owns";
}

// A consumer arriving mid-recording joins the forwarded stream. It must not open
// a capture behind the engine's back -- that is the E_INVALIDARG case.
TEST(CaptureSourceHub, AConsumerSubscribingDuringALeaseOpensNothing) {
    Fixture f;
    (void)f.hub->Subscribe([](const HubFrame&, HubFrameKind) {});
    f.DeliverFrame();
    f.hub->RequestLease();
    const int opens = f.fake->open_calls;

    (void)f.hub->Subscribe([](const HubFrame&, HubFrameKind) {});

    EXPECT_EQ(f.fake->open_calls, opens);
    EXPECT_FALSE(f.fake->is_open);
}

} // namespace
