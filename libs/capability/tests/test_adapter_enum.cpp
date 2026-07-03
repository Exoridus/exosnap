#include <gtest/gtest.h>

#include <capability/adapter_enum.h>

namespace exosnap::capability {
namespace {

// ---------------------------------------------------------------------------
// ClassifyVendor — pure, deterministic, no DXGI/hardware needed.
// ---------------------------------------------------------------------------

TEST(ClassifyVendor, NvidiaPciId) {
    EXPECT_EQ(ClassifyVendor(0x10DEu), AdapterVendor::Nvidia);
}

TEST(ClassifyVendor, AmdPciId) {
    EXPECT_EQ(ClassifyVendor(0x1002u), AdapterVendor::Amd);
}

TEST(ClassifyVendor, AmdApuAlternatePciId) {
    EXPECT_EQ(ClassifyVendor(0x1022u), AdapterVendor::Amd);
}

TEST(ClassifyVendor, IntelPciId) {
    EXPECT_EQ(ClassifyVendor(0x8086u), AdapterVendor::Intel);
}

TEST(ClassifyVendor, UnknownPciIdIsOther) {
    EXPECT_EQ(ClassifyVendor(0xDEADu), AdapterVendor::Other);
}

// ---------------------------------------------------------------------------
// ClassifyKind — pure heuristic on the two DXGI memory counters.
// ---------------------------------------------------------------------------

TEST(ClassifyKind, LargeDedicatedVramIsDiscrete) {
    // RTX-class card: several GB dedicated, a modest shared aperture.
    const uint64_t dedicated = 8ull * 1024 * 1024 * 1024;
    const uint64_t shared = 256ull * 1024 * 1024;
    EXPECT_EQ(ClassifyKind(dedicated, shared), AdapterKind::Discrete);
}

TEST(ClassifyKind, SmallDedicatedVramIsIntegrated) {
    // Typical iGPU: a small dedicated aperture (or none), relies on shared system RAM.
    const uint64_t dedicated = 128ull * 1024 * 1024;
    const uint64_t shared = 16ull * 1024 * 1024 * 1024;
    EXPECT_EQ(ClassifyKind(dedicated, shared), AdapterKind::Integrated);
}

TEST(ClassifyKind, ZeroDedicatedVramIsIntegrated) {
    EXPECT_EQ(ClassifyKind(0, 8ull * 1024 * 1024 * 1024), AdapterKind::Integrated);
}

TEST(ClassifyKind, BothZeroIsUnknown) {
    EXPECT_EQ(ClassifyKind(0, 0), AdapterKind::Unknown);
}

TEST(ClassifyKind, ExactlyAtDiscreteFloorIsDiscrete) {
    const uint64_t floor_bytes = 512ull * 1024 * 1024;
    EXPECT_EQ(ClassifyKind(floor_bytes, 0), AdapterKind::Discrete);
}

TEST(ClassifyKind, JustBelowDiscreteFloorIsIntegrated) {
    const uint64_t just_below = 512ull * 1024 * 1024 - 1;
    EXPECT_EQ(ClassifyKind(just_below, 0), AdapterKind::Integrated);
}

// ---------------------------------------------------------------------------
// EnumerateAdapters — live DXGI call. Every Windows box (including headless
// CI runners) exposes at least the software/WARP adapter to DXGI, but that
// one is filtered out; a real desktop/CI runner still exposes at least one
// real display adapter. This is a smoke test, not a hardware-specific
// assertion (see AdapterEncoderCapability tests for the vendor-specific pure
// logic, which does not require live hardware).
// ---------------------------------------------------------------------------

TEST(EnumerateAdapters, ReturnsAtLeastOneRealAdapterWithNonEmptyName) {
    const auto adapters = EnumerateAdapters();
    // Non-fatal: nearly every Windows box (including headless CI VMs) exposes at least
    // one non-software DXGI adapter, but this is still live hardware/driver behavior, so
    // an empty result is reported rather than aborting the rest of this test binary.
    EXPECT_FALSE(adapters.empty()) << "Expected at least one non-software DXGI adapter on this system.";
    for (const auto& a : adapters) {
        EXPECT_FALSE(a.name.empty());
        // Every real adapter must classify to a concrete kind or Unknown — never crash/garbage.
        EXPECT_TRUE(a.kind == AdapterKind::Discrete || a.kind == AdapterKind::Integrated ||
                    a.kind == AdapterKind::Unknown);
    }
}

TEST(EnumerateAdapters, ExcludesMicrosoftBasicRenderDriver) {
    const auto adapters = EnumerateAdapters();
    for (const auto& a : adapters) {
        const bool is_warp = a.vendor_id == 0x1414u && a.device_id == 0x008cu;
        EXPECT_FALSE(is_warp) << "Software/WARP adapter must be filtered out of EnumerateAdapters().";
    }
}

} // namespace
} // namespace exosnap::capability
