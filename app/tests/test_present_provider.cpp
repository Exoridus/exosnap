// ADR 0033: unit tests for the present-diagnostics GATE.
//
// This file covers the pre-session decision only -- opt-in AND elevation -- and the
// graceful-degrade contract when no trace can be opened. The session's own lifecycle
// (consuming, attribution boundaries, process liveness, availability) lives in
// test_present_session.cpp, against the production session with a fake trace backend.
//
// Every provider here is built with a factory that yields NO backend. That is not a
// convenience: with the vendored consumer linked into this target, the default factory
// would open a real system-wide ETW session named "ExoSnapPresentMon" and call
// StopNamedSession on it first -- tearing the session out from under a running ExoSnap
// on the same machine. A unit test may not do that to a developer's desktop.

#include <memory>

#include <gtest/gtest.h>

#include "diagnostics/ElevationProvider.h"
#include "diagnostics/PresentMonProvider.h"
#include "diagnostics/PresentProvider.h"
#include "diagnostics/PresentTraceBackend.h"

namespace {

using exosnap::diagnostics::IElevationProvider;
using exosnap::diagnostics::IPresentTraceBackend;
using exosnap::diagnostics::PresentMode;
using exosnap::diagnostics::PresentMonProvider;
using exosnap::diagnostics::PresentSample;

// Stub elevation provider returning a fixed state (mirrors test_elevation.cpp).
class StubElevationProvider final : public IElevationProvider {
  public:
    explicit StubElevationProvider(bool elevated) : elevated_(elevated) {
    }
    [[nodiscard]] bool IsElevated() const override {
        return elevated_;
    }

  private:
    bool elevated_;
};

// No trace at all -- the same answer a build without the vendored consumer gives.
auto NoBackend() {
    return [] { return std::shared_ptr<IPresentTraceBackend>{}; };
}

// Pre-session gate: opt_in AND elevation, irrespective of the trace.
TEST(PresentProviderTest, GateOpenOnlyWhenElevatedAndOptIn) {
    const StubElevationProvider elevated(true);
    const StubElevationProvider not_elevated(false);

    EXPECT_TRUE(PresentMonProvider(elevated, /*opt_in=*/true, NoBackend()).GateOpen());
    EXPECT_FALSE(PresentMonProvider(elevated, /*opt_in=*/false, NoBackend()).GateOpen());
    EXPECT_FALSE(PresentMonProvider(not_elevated, /*opt_in=*/true, NoBackend()).GateOpen());
    EXPECT_FALSE(PresentMonProvider(not_elevated, /*opt_in=*/false, NoBackend()).GateOpen());
}

// With no trace backend the session never opens, so even an elevated+opt-in provider
// cannot reach IsAvailable() == true.
TEST(PresentProviderTest, UnavailableWhenNoTraceCanBeOpened) {
    const StubElevationProvider elevated(true);
    PresentMonProvider provider(elevated, /*opt_in=*/true, NoBackend());
    EXPECT_FALSE(provider.IsAvailable());
    EXPECT_FALSE(provider.Sample().available);
}

TEST(PresentProviderTest, SampleFieldsAreDefaultWhenUnavailable) {
    // Even when the gate is fully open, a session with no trace has no datum to
    // report — Sample() must degrade cleanly with all fields at their defaults.
    const StubElevationProvider elevated(true);
    const PresentMonProvider provider(elevated, /*opt_in=*/true, NoBackend());

    const PresentSample sample = provider.Sample();
    EXPECT_FALSE(sample.available);
    EXPECT_EQ(sample.mode, PresentMode::Unknown);
    EXPECT_FALSE(sample.tearing);
    EXPECT_DOUBLE_EQ(sample.present_interval_ms, 0.0);
}

TEST(PresentProviderTest, SampleUnavailableWhenGateClosed) {
    const StubElevationProvider not_elevated(false);
    const PresentMonProvider provider(not_elevated, /*opt_in=*/true, NoBackend());
    EXPECT_FALSE(provider.GateOpen());
    EXPECT_FALSE(provider.IsAvailable());
    EXPECT_FALSE(provider.Sample().available);
}

TEST(PresentProviderTest, SetOptInFlipsGate) {
    const StubElevationProvider elevated(true);
    PresentMonProvider provider(elevated, /*opt_in=*/false, NoBackend());
    EXPECT_FALSE(provider.GateOpen());
    provider.SetOptIn(true);
    EXPECT_TRUE(provider.GateOpen());
    provider.SetOptIn(false);
    EXPECT_FALSE(provider.GateOpen());
}

TEST(PresentProviderTest, ConsumedViaInterfacePointer) {
    const StubElevationProvider elevated(true);
    const PresentMonProvider concrete(elevated, /*opt_in=*/true, NoBackend());
    const exosnap::diagnostics::IPresentProvider& as_iface = concrete;
    // With no trace the session cannot open, so IsAvailable() remains false even when
    // the pre-session gate (GateOpen) is true.
    EXPECT_FALSE(as_iface.IsAvailable());
    EXPECT_FALSE(as_iface.Sample().available);
}

} // namespace
