// ADR 0033: unit tests for the present-diagnostics provider shell.
//
// This slice ships the verifiable gating shell only — the stub PresentMonProvider
// degrades to Unavailable (no ETW consumer yet). These tests pin the gate
// (opt_in && elevation) and the always-Unavailable Sample() contract so the later
// Vendor-Slice cannot silently regress the foundation behaviour.

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

TEST(PresentProviderTest, AvailableOnlyWhenElevatedAndOptIn) {
    const StubElevationProvider elevated(true);
    const StubElevationProvider not_elevated(false);

    EXPECT_TRUE(PresentMonProvider(elevated, /*opt_in=*/true).IsAvailable());
    EXPECT_FALSE(PresentMonProvider(elevated, /*opt_in=*/false).IsAvailable());
    EXPECT_FALSE(PresentMonProvider(not_elevated, /*opt_in=*/true).IsAvailable());
    EXPECT_FALSE(PresentMonProvider(not_elevated, /*opt_in=*/false).IsAvailable());
}

TEST(PresentProviderTest, SampleIsAlwaysUnavailableInThisSlice) {
    // Even when the gate is fully open, the stub has no ETW datum to report.
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
    EXPECT_FALSE(provider.IsAvailable());
    EXPECT_FALSE(provider.Sample().available);
}

TEST(PresentProviderTest, SetOptInFlipsGate) {
    const StubElevationProvider elevated(true);
    PresentMonProvider provider(elevated, /*opt_in=*/false);
    EXPECT_FALSE(provider.IsAvailable());
    provider.SetOptIn(true);
    EXPECT_TRUE(provider.IsAvailable());
    provider.SetOptIn(false);
    EXPECT_FALSE(provider.IsAvailable());
}

TEST(PresentProviderTest, ConsumedViaInterfacePointer) {
    const StubElevationProvider elevated(true);
    const PresentMonProvider concrete(elevated, /*opt_in=*/true);
    const exosnap::diagnostics::IPresentProvider& as_iface = concrete;
    EXPECT_TRUE(as_iface.IsAvailable());
    EXPECT_FALSE(as_iface.Sample().available);
}

} // namespace
