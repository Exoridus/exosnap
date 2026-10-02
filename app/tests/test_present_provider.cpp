// Unit tests for the present-diagnostics provider projection.
//
// The gate is the session-scoped opt-in and nothing else. Elevation is not a
// proxy for trace rights: a standard token may hold them (Performance Log Users),
// and an elevated process may still meet a session conflict. The provider reports
// what the controlled ETW open actually answered. The session's own lifecycle
// lives in test_present_session.cpp.
//
// Every provider here is built with a factory that yields either no backend or a
// fake one. With the vendored consumer linked into this target, the default
// factory would open a real system-wide ETW session on the developer's desktop;
// a unit test may not do that.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "diagnostics/PresentMonProvider.h"
#include "diagnostics/PresentProvider.h"
#include "diagnostics/PresentTraceBackend.h"

namespace {

// The consumer thread starts asynchronously; this bounds the wait rather than
// guessing a sleep.
bool WaitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

using exosnap::diagnostics::IPresentTraceBackend;
using exosnap::diagnostics::PresentMode;
using exosnap::diagnostics::PresentMonProvider;
using exosnap::diagnostics::PresentProviderState;
using exosnap::diagnostics::PresentSample;
using exosnap::diagnostics::PresentTraceOpenResult;
using exosnap::diagnostics::TracePresentEvent;

// No trace at all -- the same answer a build without the vendored consumer gives.
auto NoBackend() {
    return [] { return std::shared_ptr<IPresentTraceBackend>{}; };
}

class FakeTraceBackend final : public IPresentTraceBackend {
  public:
    PresentTraceOpenResult Open() override {
        return open_result;
    }

    void Consume() override {
        std::unique_lock lk(mutex_);
        closed_cv_.wait(lk, [this] { return closed_; });
    }

    void Close() override {
        {
            std::lock_guard lk(mutex_);
            closed_ = true;
        }
        closed_cv_.notify_all();
    }

    int64_t TimestampFrequency() const override {
        return 10'000'000;
    }

    std::vector<TracePresentEvent> Drain() override {
        std::lock_guard lk(mutex_);
        std::vector<TracePresentEvent> out;
        out.swap(queued_);
        return out;
    }

    void Publish(const TracePresentEvent& event) {
        std::lock_guard lk(mutex_);
        queued_.push_back(event);
    }

    PresentTraceOpenResult open_result = PresentTraceOpenResult::Opened;

  private:
    mutable std::mutex mutex_;
    std::condition_variable closed_cv_;
    bool closed_ = false;
    std::vector<TracePresentEvent> queued_;
};

std::function<std::shared_ptr<IPresentTraceBackend>()> FactoryFor(const std::shared_ptr<FakeTraceBackend>& backend) {
    return [backend] { return std::static_pointer_cast<IPresentTraceBackend>(backend); };
}

TracePresentEvent Present(unsigned long pid, uint64_t qpc, int mode_code) {
    TracePresentEvent event;
    event.process_id = pid;
    event.present_qpc = qpc;
    event.present_mode_code = mode_code;
    event.sync_interval = 1;
    return event;
}

TEST(PresentProviderTest, GateIsTheOptInAlone) {
    EXPECT_TRUE(PresentMonProvider(/*opt_in=*/true, NoBackend()).GateOpen());
    EXPECT_FALSE(PresentMonProvider(/*opt_in=*/false, NoBackend()).GateOpen());

    PresentMonProvider provider(/*opt_in=*/false, NoBackend());
    provider.SetOptIn(true);
    EXPECT_TRUE(provider.GateOpen());
    provider.SetOptIn(false);
    EXPECT_FALSE(provider.GateOpen());
}

TEST(PresentProviderTest, WithoutABackendTheStateIsNotBuiltAndNothingIsMeasured) {
    PresentMonProvider provider(/*opt_in=*/true, NoBackend());
    EXPECT_FALSE(provider.IsAvailable());
    EXPECT_EQ(provider.state(), PresentProviderState::NotBuilt);
    EXPECT_EQ(provider.openResult(), PresentTraceOpenResult::NotBuilt);
    EXPECT_FALSE(provider.Sample().available);
}

TEST(PresentProviderTest, AccessDeniedIsNamedAndDoesNotFabricateData) {
    auto backend = std::make_shared<FakeTraceBackend>();
    backend->open_result = PresentTraceOpenResult::AccessDenied;
    PresentMonProvider provider(/*opt_in=*/true, FactoryFor(backend));

    EXPECT_FALSE(provider.IsAvailable());
    EXPECT_EQ(provider.state(), PresentProviderState::AccessDenied);
    EXPECT_EQ(provider.openResult(), PresentTraceOpenResult::AccessDenied);

    const PresentSample sample = provider.Sample();
    EXPECT_FALSE(sample.available);
    EXPECT_EQ(sample.mode, PresentMode::Unknown);
    EXPECT_FALSE(sample.tearing);
    EXPECT_DOUBLE_EQ(sample.present_interval_ms, 0.0);
}

TEST(PresentProviderTest, ASessionConflictIsNamedAndTheOtherSessionIsLeftAlone) {
    auto backend = std::make_shared<FakeTraceBackend>();
    backend->open_result = PresentTraceOpenResult::SessionConflict;
    PresentMonProvider provider(/*opt_in=*/true, FactoryFor(backend));

    EXPECT_FALSE(provider.IsAvailable());
    EXPECT_EQ(provider.state(), PresentProviderState::SessionConflict);
    EXPECT_FALSE(provider.Sample().available);
}

TEST(PresentProviderTest, NotSupportedIsItsOwnReason) {
    auto backend = std::make_shared<FakeTraceBackend>();
    backend->open_result = PresentTraceOpenResult::NotSupported;
    PresentMonProvider provider(/*opt_in=*/true, FactoryFor(backend));
    EXPECT_EQ(provider.state(), PresentProviderState::NotSupported);
}

TEST(PresentProviderTest, AnOpenTraceWithoutDataIsNotAZeroMeasurement) {
    auto backend = std::make_shared<FakeTraceBackend>();
    PresentMonProvider provider(/*opt_in=*/true, FactoryFor(backend));
    ASSERT_TRUE(WaitUntil([&] { return provider.IsAvailable(); }));

    EXPECT_EQ(provider.state(), PresentProviderState::OpenNoData);
    EXPECT_FALSE(provider.Sample().available);

    backend->Publish(Present(0, 1'000'000, 0));
    EXPECT_TRUE(WaitUntil([&] { return provider.state() == PresentProviderState::Measuring; }));
    EXPECT_TRUE(provider.Sample().available);
}

TEST(PresentProviderTest, ConsumedViaInterfacePointer) {
    auto backend = std::make_shared<FakeTraceBackend>();
    backend->open_result = PresentTraceOpenResult::AccessDenied;
    const PresentMonProvider concrete(/*opt_in=*/true, FactoryFor(backend));
    const exosnap::diagnostics::IPresentProvider& as_iface = concrete;
    // The request is on, the OS said no: IsAvailable() is false and the sample
    // reports nothing, but the reason is recoverable from the provider state.
    EXPECT_FALSE(as_iface.IsAvailable());
    EXPECT_FALSE(as_iface.Sample().available);
    EXPECT_EQ(concrete.state(), PresentProviderState::AccessDenied);
}

} // namespace
