// The registry's job is sharing and disposal: one hub per key, born with the
// first consumer, gone with the last. What a hub does once it exists is pinned
// by test_capture_source_hub.cpp and by the policy's own suite; it is not
// restated here.
//
// The one behavioural test that does belong here is the hold, because it is the
// reason the picker's tiles go behind a hub at all: a source that stops
// producing must keep handing out its last good frame rather than nothing.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "services/CaptureHubRegistry.h"

namespace {

using namespace exosnap;
using recorder_core::HubFrameKind;

// Not constexpr: the key carries the DXGI hub's device-name string now.
const CaptureSourceKey kWindowA{CaptureSourceKey::Kind::Window, 0x1000};
const CaptureSourceKey kWindowB{CaptureSourceKey::Kind::Window, 0x2000};

// A monitor and a window may share a native id value without being the same
// source; the key's kind is what tells them apart.
const CaptureSourceKey kMonitorA{CaptureSourceKey::Kind::Monitor, 0x1000};

// DXGI keys ignore native_id and are told apart by device name alone.
const CaptureSourceKey kDxgiDisplay1{CaptureSourceKey::Kind::DxgiMonitor, 0, L"\\\\.\\DISPLAY1"};
const CaptureSourceKey kDxgiDisplay2{CaptureSourceKey::Kind::DxgiMonitor, 0, L"\\\\.\\DISPLAY2"};

class FakeProducer : public HubSourceProducer {
  public:
    ProducerPoll next_poll = ProducerPoll::NoFrame;
    int open_calls = 0;
    int close_calls = 0;
    bool open_succeeds = true;
    uint64_t generation = 0;

    bool Open(std::string& err) override {
        ++open_calls;
        if (!open_succeeds) {
            err = "fake: refused";
            return false;
        }
        return true;
    }

    void Close() override {
        ++close_calls;
    }

    ProducerPoll PollFrame(HubFrame& out) override {
        if (next_poll == ProducerPoll::Frame) {
            out.width = 640;
            out.height = 480;
            out.generation = ++generation;
        }
        return next_poll;
    }
};

// Hands out fakes and keeps a pointer to each, so a test can steer the source
// behind a key after the registry has built the hub.
struct Fixture {
    std::vector<CaptureSourceKey> built;
    std::unordered_map<CaptureSourceKey, FakeProducer*, CaptureSourceKeyHash> producers;
    CaptureHubRegistry registry;

    Fixture()
        : registry([this](const CaptureSourceKey& key) -> std::unique_ptr<HubSourceProducer> {
              built.push_back(key);
              auto p = std::make_unique<FakeProducer>();
              producers[key] = p.get();
              return p;
          }) {
    }

    FakeProducer& Producer(const CaptureSourceKey& key) {
        return *producers.at(key);
    }

    void DeliverFrame(const CaptureSourceKey& key) {
        Producer(key).next_poll = ProducerPoll::Frame;
        registry.PumpAll();
        Producer(key).next_poll = ProducerPoll::NoFrame;
    }

    CaptureSubscription SubscribeIgnoring(const CaptureSourceKey& key) {
        return registry.Subscribe(key, [](const HubFrame&, HubFrameKind) {});
    }
};

TEST(CaptureHubRegistry, TwoConsumersOfOneSourceShareOneCapture) {
    Fixture f;
    auto a = f.SubscribeIgnoring(kWindowA);
    auto b = f.SubscribeIgnoring(kWindowA);

    EXPECT_EQ(f.registry.HubCountForTest(), 1u);
    EXPECT_EQ(f.built.size(), 1u);
    EXPECT_EQ(f.Producer(kWindowA).open_calls, 1);
}

TEST(CaptureHubRegistry, DistinctSourcesGetDistinctHubs) {
    Fixture f;
    auto a = f.SubscribeIgnoring(kWindowA);
    auto b = f.SubscribeIgnoring(kWindowB);

    EXPECT_EQ(f.registry.HubCountForTest(), 2u);
    EXPECT_EQ(f.built.size(), 2u);
}

TEST(CaptureHubRegistry, KindDisambiguatesAnIdenticalNativeId) {
    Fixture f;
    auto window = f.SubscribeIgnoring(kWindowA);
    auto monitor = f.SubscribeIgnoring(kMonitorA);

    EXPECT_EQ(f.registry.HubCountForTest(), 2u);
}

TEST(CaptureHubRegistry, DxgiKeysAreToldApartByDeviceNameAlone) {
    Fixture f;
    auto one = f.SubscribeIgnoring(kDxgiDisplay1);
    auto two = f.SubscribeIgnoring(kDxgiDisplay2);
    auto again = f.SubscribeIgnoring(kDxgiDisplay1);

    // Same name shares a hub; a different name gets its own, native_id unused.
    EXPECT_EQ(f.registry.HubCountForTest(), 2u);
    EXPECT_EQ(f.built.size(), 2u);
    EXPECT_EQ(f.Producer(kDxgiDisplay1).open_calls, 1);
}

TEST(CaptureHubRegistry, TheLastConsumerLeavingClosesAndDiscardsTheHub) {
    Fixture f;
    {
        auto a = f.SubscribeIgnoring(kWindowA);
        auto b = f.SubscribeIgnoring(kWindowA);
        EXPECT_EQ(f.registry.HubCountForTest(), 1u);

        a.Reset();
        // One consumer left: the capture stays open. Discarding here would blank
        // the surviving tile.
        EXPECT_EQ(f.registry.HubCountForTest(), 1u);
        EXPECT_EQ(f.Producer(kWindowA).close_calls, 0);
    }
    EXPECT_EQ(f.registry.HubCountForTest(), 0u);
}

TEST(CaptureHubRegistry, ResubscribingAfterDisposalBuildsAFreshCapture) {
    Fixture f;
    f.SubscribeIgnoring(kWindowA).Reset();
    auto again = f.SubscribeIgnoring(kWindowA);

    EXPECT_EQ(f.built.size(), 2u);
    EXPECT_EQ(f.registry.HubCountForTest(), 1u);
}

TEST(CaptureHubRegistry, PumpAllFansOutToEverySourcesConsumers) {
    Fixture f;
    std::vector<uint64_t> seen_a;
    std::vector<uint64_t> seen_b;
    auto a = f.registry.Subscribe(kWindowA, [&](const HubFrame& fr, HubFrameKind) { seen_a.push_back(fr.generation); });
    auto b = f.registry.Subscribe(kWindowB, [&](const HubFrame& fr, HubFrameKind) { seen_b.push_back(fr.generation); });

    f.Producer(kWindowA).next_poll = ProducerPoll::Frame;
    f.Producer(kWindowB).next_poll = ProducerPoll::Frame;
    f.registry.PumpAll();

    EXPECT_EQ(seen_a, std::vector<uint64_t>{1});
    EXPECT_EQ(seen_b, std::vector<uint64_t>{1});
}

TEST(CaptureHubRegistry, NoFrameYetIsDistinguishableFromAHeldFrame) {
    Fixture f;
    auto a = f.SubscribeIgnoring(kWindowA);
    EXPECT_EQ(a.Frame(), HubFrameKind::None);
    EXPECT_EQ(a.HeldFrame().width, 0u);

    f.DeliverFrame(kWindowA);
    EXPECT_EQ(a.Frame(), HubFrameKind::Live);
    EXPECT_EQ(a.HeldFrame().width, 640u);
}

// The reason the picker's tiles are hub consumers. A Snipping Tool session takes
// the surface and WGC stops producing; the tile must freeze, not empty.
TEST(CaptureHubRegistry, ASourceThatStopsProducingKeepsServingItsLastFrame) {
    Fixture f;
    auto a = f.SubscribeIgnoring(kWindowA);
    f.DeliverFrame(kWindowA);

    for (int i = 0; i < 50; ++i)
        f.registry.PumpAll(); // quiet source: NoFrame, over and over

    EXPECT_EQ(a.HeldFrame().generation, 1u);
    EXPECT_EQ(a.HeldFrame().width, 640u);
}

// Loss is held too, and reopened without a deadline. The held frame is never
// cleared on the way.
TEST(CaptureHubRegistry, LossHoldsTheFrameAndRetriesTheReopenForever) {
    Fixture f;
    auto a = f.SubscribeIgnoring(kWindowA);
    f.DeliverFrame(kWindowA);

    FakeProducer& producer = f.Producer(kWindowA);
    producer.next_poll = ProducerPoll::Lost;
    producer.open_succeeds = false;
    f.registry.PumpAll();

    EXPECT_EQ(a.Frame(), HubFrameKind::Held);

    const int opens_before = producer.open_calls;
    for (int i = 0; i < 20; ++i)
        f.registry.PumpAll();
    EXPECT_GT(producer.open_calls, opens_before) << "the reopen budget is unbounded";
    EXPECT_EQ(a.HeldFrame().generation, 1u) << "a hold must never be cleared by loss";

    producer.open_succeeds = true;
    producer.next_poll = ProducerPoll::NoFrame;
    f.registry.PumpAll(); // reopens, but has produced nothing yet
    EXPECT_EQ(a.Frame(), HubFrameKind::Held);

    f.DeliverFrame(kWindowA);
    EXPECT_EQ(a.Frame(), HubFrameKind::Live);
    EXPECT_EQ(a.HeldFrame().generation, 2u);
}

TEST(CaptureHubRegistry, AConsumerIsNeverCalledAfterItUnsubscribes) {
    Fixture f;
    int calls = 0;
    auto a = f.registry.Subscribe(kWindowA, [&](const HubFrame&, HubFrameKind) { ++calls; });
    auto keep_alive = f.SubscribeIgnoring(kWindowA); // so the hub outlives `a`

    f.DeliverFrame(kWindowA);
    EXPECT_EQ(calls, 1);

    a.Reset();
    f.DeliverFrame(kWindowA);
    EXPECT_EQ(calls, 1);
}

TEST(CaptureHubRegistry, MovingASubscriptionDoesNotUnsubscribeTwice) {
    Fixture f;
    auto a = f.SubscribeIgnoring(kWindowA);
    auto b = f.SubscribeIgnoring(kWindowA);
    {
        CaptureSubscription moved = std::move(a);
        EXPECT_FALSE(static_cast<bool>(a));
        EXPECT_TRUE(static_cast<bool>(moved));
    }
    // `moved` dropped one consumer; `b` is still watching. A double-unsubscribe
    // would have taken `b`'s capture down with it.
    EXPECT_EQ(f.registry.HubCountForTest(), 1u);
    EXPECT_EQ(f.Producer(kWindowA).close_calls, 0);
}

TEST(CaptureHubRegistry, AnEmptySubscriptionReportsNoFrameRatherThanCrashing) {
    CaptureSubscription empty;
    EXPECT_FALSE(static_cast<bool>(empty));
    EXPECT_EQ(empty.Frame(), HubFrameKind::None);
    EXPECT_EQ(empty.HeldFrame().width, 0u);
}

} // namespace
