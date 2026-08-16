// The capture hub *services'* command channel and lease gate -- the layer above
// the registry, where DXGI and WGC turn posted commands into registry calls.
//
// Both services are unconstructible in a test (they own a pump thread that
// opens Output Duplication resp. a WGC frame pool), so what is pinned here is
// the pair of components they are thin drivers of: CaptureHubCommandQueue and
// StepCaptureHubGate. The harness below runs the identical drive loop both
// WorkerProcs run, against the real CaptureHubRegistry and a fake producer, so
// an interleaving that reopens a duplication shows up as a real Open() call.
//
// The races these tests exist for -- both reachable, because
// Subscribe/Unsubscribe are posted from the UI thread while
// RequestEngineLease() is posted from the recording coordinator's
// prepare/record worker:
//
//   1. the single-slot mailbox dropped whichever command was still unprocessed;
//   2. the acknowledgement was "processed serial >= mine", so the Subscribe that
//      overwrote a LeaseRequest also released its waiter -- and then opened a
//      second duplication of the output the engine was about to record.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "services/CaptureHubCommandQueue.h"
#include "services/CaptureHubGate.h"
#include "services/CaptureHubRegistry.h"

namespace {

using namespace exosnap;
using recorder_core::HubFrameKind;

const CaptureSourceKey kDisplay1{CaptureSourceKey::Kind::DxgiMonitor, 0, L"\\\\.\\DISPLAY1"};
const CaptureSourceKey kDisplay2{CaptureSourceKey::Kind::DxgiMonitor, 0, L"\\\\.\\DISPLAY2"};

// Counters outlive the producers deliberately: a hub is disposed with its last
// consumer, so a test that re-subscribes gets a *new* producer and must still
// be able to read what every producer for that key did.
struct ProducerCounters {
    int opens = 0;
    int closes = 0;
};

class FakeProducer : public HubSourceProducer {
  public:
    explicit FakeProducer(ProducerCounters* counters) : counters_(counters) {
    }

    bool Open(std::string&) override {
        ++counters_->opens;
        return true;
    }
    void Close() override {
        ++counters_->closes;
    }
    ProducerPoll PollFrame(HubFrame&) override {
        return ProducerPoll::NoFrame;
    }

  private:
    ProducerCounters* counters_;
};

// What a Subscribe carries. The services differ only in this payload (a device
// name vs. a WGC source key) and in what their sinks do with a frame; the
// command handling below is theirs verbatim.
struct Payload {
    CaptureSourceKey key;
};

// One pump-thread iteration of DxgiCaptureHubService::WorkerProc /
// WgcCaptureHubService::WorkerProc, minus the D3D publisher.
struct Harness {
    CaptureHubCommandQueue<Payload> commands;
    CaptureHubGateState gate;
    std::unordered_map<CaptureSourceKey, ProducerCounters, CaptureSourceKeyHash> counters;
    CaptureHubRegistry registry;
    CaptureSubscription subscription;
    CaptureSourceKey current_key;
    Payload desired;
    int publisher_resets = 0;
    std::vector<CaptureHubCommandQueue<Payload>::Entry> batch;

    Harness()
        : registry([this](const CaptureSourceKey& key) -> std::unique_ptr<HubSourceProducer> {
              return std::make_unique<FakeProducer>(&counters[key]);
          }) {
    }

    int OpenCalls(const CaptureSourceKey& key) {
        const auto it = counters.find(key);
        return it == counters.end() ? 0 : it->second.opens;
    }

    int CloseCalls(const CaptureSourceKey& key) {
        const auto it = counters.find(key);
        return it == counters.end() ? 0 : it->second.closes;
    }

    uint64_t Subscribe(const CaptureSourceKey& key) {
        return commands.Post(CaptureHubOp::Subscribe, Payload{key});
    }
    uint64_t Unsubscribe() {
        return commands.Post(CaptureHubOp::Unsubscribe);
    }
    uint64_t LeaseRequest() {
        return commands.Post(CaptureHubOp::LeaseRequest);
    }
    uint64_t LeaseReturn() {
        return commands.Post(CaptureHubOp::LeaseReturn);
    }

    // Drains and applies everything queued, then pumps -- exactly the shape of
    // both WorkerProc loop bodies.
    void Step() {
        commands.WaitAndDrain(std::chrono::milliseconds(0), batch);
        for (auto& command : batch) {
            const CaptureHubGateAction action = StepCaptureHubGate(gate, command.op);
            gate = action.next;

            if (command.op == CaptureHubOp::Subscribe)
                desired = std::move(command.payload);

            if (action.drop_subscription) {
                subscription.Reset();
                current_key = {};
            }
            if (action.release_lease)
                registry.RequestLease(current_key);
            if (action.reset_publisher)
                ++publisher_resets;
            if (action.return_lease)
                registry.ReturnLease(current_key);
            if (action.apply_subscription) {
                current_key = desired.key;
                subscription = registry.Subscribe(current_key, [](const HubFrame&, HubFrameKind) {});
            }
            if (action.acknowledge_release)
                commands.PublishLeaseRelease(command.serial);
        }
        registry.PumpAll();
    }
};

TEST(CaptureHubGate, ASubscriptionIsNeverAppliedWhileTheLeaseIsOut) {
    // Exhaustive over every gate state and command: the one invariant the whole
    // file exists for. A capture opened here is a second duplication of the
    // output the engine is recording, which DXGI refuses outright.
    for (int bits = 0; bits < 8; ++bits) {
        CaptureHubGateState state;
        state.leased = (bits & 1) != 0;
        state.subscribed = (bits & 2) != 0;
        state.deferred = (bits & 4) != 0;
        for (const CaptureHubOp op : {CaptureHubOp::Subscribe, CaptureHubOp::Unsubscribe, CaptureHubOp::LeaseRequest,
                                      CaptureHubOp::LeaseReturn}) {
            const CaptureHubGateAction action = StepCaptureHubGate(state, op);
            EXPECT_FALSE(action.apply_subscription && action.next.leased)
                << "bits=" << bits << " op=" << static_cast<int>(op);
        }
    }
}

TEST(CaptureHubGate, OnlyALeaseRequestAcknowledgesARelease) {
    for (int bits = 0; bits < 8; ++bits) {
        CaptureHubGateState state;
        state.leased = (bits & 1) != 0;
        state.subscribed = (bits & 2) != 0;
        state.deferred = (bits & 4) != 0;
        EXPECT_FALSE(StepCaptureHubGate(state, CaptureHubOp::Subscribe).acknowledge_release);
        EXPECT_FALSE(StepCaptureHubGate(state, CaptureHubOp::Unsubscribe).acknowledge_release);
        EXPECT_FALSE(StepCaptureHubGate(state, CaptureHubOp::LeaseReturn).acknowledge_release);
        EXPECT_TRUE(StepCaptureHubGate(state, CaptureHubOp::LeaseRequest).acknowledge_release);
    }
}

TEST(CaptureHubService, SubscribeThenLeaseClosesTheCaptureAndAcknowledges) {
    Harness h;
    h.Subscribe(kDisplay1);
    h.Step();
    ASSERT_EQ(h.OpenCalls(kDisplay1), 1);

    const uint64_t lease = h.LeaseRequest();
    EXPECT_FALSE(h.commands.WaitForLeaseRelease(lease, std::chrono::milliseconds(0)))
        << "nothing may be acknowledged before the pump has processed the request";
    h.Step();

    EXPECT_EQ(h.CloseCalls(kDisplay1), 1);
    EXPECT_TRUE(h.commands.WaitForLeaseRelease(lease, std::chrono::milliseconds(0)));
    EXPECT_EQ(h.commands.LeaseReleasedSerialForTest(), lease);
}

TEST(CaptureHubService, ALeaseQueuedBehindASubscribeLosesNeither) {
    // Both producers post before the pump wakes: the UI thread's Subscribe and
    // the record worker's LeaseRequest. The single-slot mailbox kept only one.
    Harness h;
    h.Subscribe(kDisplay1);
    const uint64_t lease = h.LeaseRequest();
    h.Step();

    EXPECT_EQ(h.OpenCalls(kDisplay1), 1) << "the subscribe must not be dropped";
    EXPECT_EQ(h.CloseCalls(kDisplay1), 1) << "the lease request must not be dropped";
    EXPECT_TRUE(h.commands.WaitForLeaseRelease(lease, std::chrono::milliseconds(0)));
}

TEST(CaptureHubService, ASubscribeQueuedBehindALeaseCannotReopenTheCapture) {
    // The P0 interleaving. Old behaviour: the Subscribe overwrote the pending
    // LeaseRequest, opened a fresh hub whose state said nobody owned the
    // output -- and released the engine's waiter anyway, because its serial was
    // higher.
    Harness h;
    h.Subscribe(kDisplay1);
    h.Step();

    const uint64_t lease = h.LeaseRequest();
    h.Subscribe(kDisplay1);
    h.Step();

    EXPECT_EQ(h.CloseCalls(kDisplay1), 1);
    EXPECT_EQ(h.OpenCalls(kDisplay1), 1) << "no second duplication while the engine holds the lease";
    EXPECT_TRUE(h.commands.WaitForLeaseRelease(lease, std::chrono::milliseconds(0)));
    EXPECT_EQ(h.commands.LeaseReleasedSerialForTest(), lease) << "the subscribe must not stand in for the release";

    // Pumping on does not sneak a reopen in either.
    for (int i = 0; i < 20; ++i)
        h.Step();
    EXPECT_EQ(h.OpenCalls(kDisplay1), 1);
}

TEST(CaptureHubService, ASubscribeDeferredByTheLeaseIsServedOnItsReturn) {
    Harness h;
    h.Subscribe(kDisplay1);
    h.Step();
    h.LeaseRequest();
    h.Subscribe(kDisplay2); // the user switched the preview target mid-prepare
    h.Step();
    ASSERT_EQ(h.OpenCalls(kDisplay2), 0);

    h.LeaseReturn();
    h.Step();
    EXPECT_EQ(h.OpenCalls(kDisplay2), 1) << "a deferred subscribe is served, not discarded";
    EXPECT_EQ(h.OpenCalls(kDisplay1), 1) << "the leased source is not reopened behind the engine";
}

TEST(CaptureHubService, AnUnsubscribeUnderTheLeaseRevokesTheDeferredSubscribe) {
    Harness h;
    h.LeaseRequest();
    h.Subscribe(kDisplay1);
    h.Unsubscribe();
    h.LeaseReturn();
    h.Step();

    EXPECT_EQ(h.OpenCalls(kDisplay1), 0) << "the caller withdrew the preview before the lease came back";
    EXPECT_FALSE(static_cast<bool>(h.subscription));
}

TEST(CaptureHubService, AConcurrentUnsubscribeDoesNotAcknowledgeTheLease) {
    Harness h;
    h.Subscribe(kDisplay1);
    h.Step();

    const uint64_t lease = h.LeaseRequest();
    const uint64_t unsubscribe = h.Unsubscribe();
    h.Step();

    EXPECT_TRUE(h.commands.WaitForLeaseRelease(lease, std::chrono::milliseconds(0)));
    EXPECT_EQ(h.commands.LeaseReleasedSerialForTest(), lease);
    EXPECT_FALSE(h.commands.WaitForLeaseRelease(unsubscribe, std::chrono::milliseconds(0)))
        << "a non-lease serial is never acknowledged";
}

TEST(CaptureHubService, TheLeaseStaysOutUntilItIsExplicitlyReturned) {
    Harness h;
    h.Subscribe(kDisplay1);
    h.Step();
    h.LeaseRequest();
    h.Step();

    for (int i = 0; i < 10; ++i) {
        h.Subscribe(kDisplay1);
        h.Unsubscribe();
        h.Step();
    }
    EXPECT_EQ(h.OpenCalls(kDisplay1), 1);

    h.Subscribe(kDisplay1);
    h.LeaseReturn();
    h.Step();
    EXPECT_EQ(h.OpenCalls(kDisplay1), 2);
}

TEST(CaptureHubService, ARapidBurstOfCommandsIsAppliedInOrderAndInFull) {
    Harness h;
    for (int i = 0; i < 40; ++i) {
        h.Subscribe(kDisplay1);
        h.Unsubscribe();
    }
    h.Step();

    // Every pair opened and closed a capture: the registry disposes the hub
    // with its last consumer, so a lost command would show up as a mismatch.
    EXPECT_EQ(h.OpenCalls(kDisplay1), 40);
    EXPECT_EQ(h.CloseCalls(kDisplay1), 40);
    EXPECT_EQ(h.registry.HubCountForTest(), 0u);
    EXPECT_EQ(h.publisher_resets, 80);
}

TEST(CaptureHubCommandQueueTest, ConcurrentProducersLoseNoCommand) {
    CaptureHubCommandQueue<Payload> queue;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;

    std::vector<std::thread> posters;
    posters.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        posters.emplace_back([&queue] {
            for (int i = 0; i < kPerThread; ++i)
                queue.Post(i % 2 == 0 ? CaptureHubOp::Subscribe : CaptureHubOp::Unsubscribe, Payload{});
        });
    }

    std::vector<uint64_t> serials;
    std::vector<CaptureHubCommandQueue<Payload>::Entry> batch;
    // No sleep and no deadline guess: the drain loop simply runs until it has
    // seen every serial the producers will ever emit.
    while (serials.size() < static_cast<size_t>(kThreads * kPerThread)) {
        queue.WaitAndDrain(std::chrono::milliseconds(5), batch);
        for (const auto& entry : batch)
            serials.push_back(entry.serial);
    }
    for (std::thread& poster : posters)
        poster.join();

    ASSERT_EQ(serials.size(), static_cast<size_t>(kThreads * kPerThread));
    EXPECT_TRUE(std::is_sorted(serials.begin(), serials.end())) << "drain order is post order";
    for (size_t i = 0; i < serials.size(); ++i)
        ASSERT_EQ(serials[i], static_cast<uint64_t>(i + 1)) << "no serial may be skipped";
}

TEST(CaptureHubCommandQueueTest, AWaiterIsReleasedByItsOwnLeaseAndNotByAnEarlierOne) {
    CaptureHubCommandQueue<Payload> queue;
    const uint64_t first = queue.Post(CaptureHubOp::LeaseRequest);
    const uint64_t second = queue.Post(CaptureHubOp::LeaseRequest);

    queue.PublishLeaseRelease(first);
    EXPECT_TRUE(queue.WaitForLeaseRelease(first, std::chrono::milliseconds(0)));
    EXPECT_FALSE(queue.WaitForLeaseRelease(second, std::chrono::milliseconds(0)));

    queue.PublishLeaseRelease(second);
    EXPECT_TRUE(queue.WaitForLeaseRelease(second, std::chrono::milliseconds(0)));
}

TEST(CaptureHubCommandQueueTest, ShutdownReleasesALeaseWaiterWithAFailure) {
    CaptureHubCommandQueue<Payload> queue;
    const uint64_t serial = queue.Post(CaptureHubOp::LeaseRequest);

    std::atomic<bool> released{false};
    std::atomic<bool> result{true};
    std::thread waiter([&] {
        // A generous timeout on purpose: the test proves Shutdown() ends the
        // wait, so reaching the timeout would be the failure, not the pass.
        result = queue.WaitForLeaseRelease(serial, std::chrono::seconds(10));
        released = true;
    });

    queue.Shutdown();
    waiter.join();

    EXPECT_TRUE(released.load());
    EXPECT_FALSE(result.load()) << "a service that is shutting down never released the capture";
    EXPECT_TRUE(queue.Stopping());
}

TEST(CaptureHubCommandQueueTest, APumpThreadDrainingConcurrentlyStillHonoursTheAck) {
    // The real threading shape: one poster thread standing in for the UI thread,
    // one for the record worker, and a pump draining both. The lease waiter must
    // observe its release, and it must observe it only after the pump has
    // processed that specific command.
    CaptureHubCommandQueue<Payload> queue;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> processed_lease{0};

    std::thread pump([&] {
        std::vector<CaptureHubCommandQueue<Payload>::Entry> batch;
        while (!stop.load(std::memory_order_acquire)) {
            queue.WaitAndDrain(std::chrono::milliseconds(1), batch);
            for (const auto& entry : batch) {
                if (entry.op == CaptureHubOp::LeaseRequest) {
                    processed_lease.store(entry.serial, std::memory_order_release);
                    queue.PublishLeaseRelease(entry.serial);
                }
            }
        }
    });

    std::thread ui([&] {
        for (int i = 0; i < 500; ++i)
            queue.Post(i % 2 == 0 ? CaptureHubOp::Subscribe : CaptureHubOp::Unsubscribe, Payload{});
    });

    for (int i = 0; i < 50; ++i) {
        const uint64_t serial = queue.Post(CaptureHubOp::LeaseRequest);
        ASSERT_TRUE(queue.WaitForLeaseRelease(serial, std::chrono::seconds(10))) << "iteration " << i;
        ASSERT_GE(processed_lease.load(std::memory_order_acquire), serial);
        queue.Post(CaptureHubOp::LeaseReturn);
    }

    ui.join();
    stop.store(true, std::memory_order_release);
    queue.Shutdown();
    pump.join();
}

} // namespace
