#include <gtest/gtest.h>

#include "diagnostics/ElevationProvider.h"

namespace {

using exosnap::diagnostics::IElevationProvider;

// Stub provider returning a fixed elevation state (the injectable seam tests and
// future providers rely on, mirroring the DiskSpaceProvider stub pattern).
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

TEST(ElevationProviderTest, StubReportsElevated) {
    const StubElevationProvider provider(true);
    EXPECT_TRUE(provider.IsElevated());
}

TEST(ElevationProviderTest, StubReportsNonElevated) {
    const StubElevationProvider provider(false);
    EXPECT_FALSE(provider.IsElevated());
}

TEST(ElevationProviderTest, ConsumedViaInterfacePointer) {
    const StubElevationProvider concrete(true);
    const IElevationProvider& as_iface = concrete;
    EXPECT_TRUE(as_iface.IsElevated());
}

} // namespace
