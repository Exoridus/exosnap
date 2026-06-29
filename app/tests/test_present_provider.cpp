// ADR 0033: unit tests for the present-diagnostics provider.
//
// The test target does NOT define EXOSNAP_HAS_PRESENTMON, so
// PresentMonEtwSession::Start() always returns false and IsOpen() always
// returns false. This keeps the test Qt-only (no ETW dep) while exercising
// the gating logic (GateOpen) and the graceful-degrade contract for the
// session-less path (IsAvailable / Sample).

#include <gtest/gtest.h>

#include "diagnostics/ElevationProvider.h"
#include "diagnostics/PresentMonProvider.h"
#include "diagnostics/PresentProvider.h"

namespace {

using exosnap::diagnostics::IElevationProvider;
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

// Pre-session gate: opt_in AND elevation, irrespective of the ETW session.
TEST(PresentProviderTest, GateOpenOnlyWhenElevatedAndOptIn) {
    const StubElevationProvider elevated(true);
    const StubElevationProvider not_elevated(false);

    EXPECT_TRUE(PresentMonProvider(elevated, /*opt_in=*/true).GateOpen());
    EXPECT_FALSE(PresentMonProvider(elevated, /*opt_in=*/false).GateOpen());
    EXPECT_FALSE(PresentMonProvider(not_elevated, /*opt_in=*/true).GateOpen());
    EXPECT_FALSE(PresentMonProvider(not_elevated, /*opt_in=*/false).GateOpen());
}

// Without EXOSNAP_HAS_PRESENTMON the no-op session never opens, so even an
// elevated+opt-in provider cannot reach IsAvailable() == true.
TEST(PresentProviderTest, UnavailableWhenEtwConsumerNotCompiledIn) {
    const StubElevationProvider elevated(true);
    PresentMonProvider provider(elevated, /*opt_in=*/true);
    // No EXOSNAP_HAS_PRESENTMON for this test target -> session can't open.
    EXPECT_FALSE(provider.IsAvailable());
    EXPECT_FALSE(provider.Sample().available);
}

TEST(PresentProviderTest, SampleFieldsAreDefaultWhenUnavailable) {
    // Even when the gate is fully open, the no-op session has no ETW datum to
    // report — Sample() must degrade cleanly with all fields at their defaults.
    const StubElevationProvider elevated(true);
    const PresentMonProvider provider(elevated, /*opt_in=*/true);

    const PresentSample sample = provider.Sample();
    EXPECT_FALSE(sample.available);
    EXPECT_EQ(sample.mode, PresentMode::Unknown);
    EXPECT_FALSE(sample.tearing);
    EXPECT_DOUBLE_EQ(sample.present_interval_ms, 0.0);
}

TEST(PresentProviderTest, SampleUnavailableWhenGateClosed) {
    const StubElevationProvider not_elevated(false);
    const PresentMonProvider provider(not_elevated, /*opt_in=*/true);
    EXPECT_FALSE(provider.GateOpen());
    EXPECT_FALSE(provider.IsAvailable());
    EXPECT_FALSE(provider.Sample().available);
}

TEST(PresentProviderTest, SetOptInFlipsGate) {
    const StubElevationProvider elevated(true);
    PresentMonProvider provider(elevated, /*opt_in=*/false);
    EXPECT_FALSE(provider.GateOpen());
    provider.SetOptIn(true);
    EXPECT_TRUE(provider.GateOpen());
    provider.SetOptIn(false);
    EXPECT_FALSE(provider.GateOpen());
}

TEST(PresentProviderTest, ConsumedViaInterfacePointer) {
    const StubElevationProvider elevated(true);
    const PresentMonProvider concrete(elevated, /*opt_in=*/true);
    const exosnap::diagnostics::IPresentProvider& as_iface = concrete;
    // Without EXOSNAP_HAS_PRESENTMON the session cannot open, so IsAvailable()
    // remains false even when the pre-session gate (GateOpen) is true.
    EXPECT_FALSE(as_iface.IsAvailable());
    EXPECT_FALSE(as_iface.Sample().available);
}

} // namespace
