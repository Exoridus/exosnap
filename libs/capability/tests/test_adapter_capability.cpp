#include <gtest/gtest.h>

#include <capability/adapter_capability.h>
#include <capability/adapter_enum.h>

namespace exosnap::capability {
namespace {

// ---------------------------------------------------------------------------
// EncoderBackendLabelForVendor — pure vendor -> label mapping, no probing.
// ---------------------------------------------------------------------------

TEST(EncoderBackendLabelForVendor, NvidiaMapsToNvenc) {
    EXPECT_EQ(EncoderBackendLabelForVendor(AdapterVendor::Nvidia), "NVENC");
}

TEST(EncoderBackendLabelForVendor, AmdHasNoWiredBackend) {
    EXPECT_EQ(EncoderBackendLabelForVendor(AdapterVendor::Amd), "");
}

TEST(EncoderBackendLabelForVendor, IntelHasNoWiredBackend) {
    EXPECT_EQ(EncoderBackendLabelForVendor(AdapterVendor::Intel), "");
}

TEST(EncoderBackendLabelForVendor, OtherHasNoWiredBackend) {
    EXPECT_EQ(EncoderBackendLabelForVendor(AdapterVendor::Other), "");
}

// ---------------------------------------------------------------------------
// ProbeAdapterEncoderCapability — the AMD/Intel/Other path is pure (no probe
// is ever attempted for these vendors), so it is fully deterministic without
// any hardware. This is the MVP-discipline guarantee from suite-device.jsx:
// roadmap backends never fabricate a probed result.
// ---------------------------------------------------------------------------

TEST(ProbeAdapterEncoderCapability, AmdAdapterIsHonestlyUnprobed) {
    AdapterInfo adapter;
    adapter.name = "Synthetic Radeon RX 9999";
    adapter.vendor = AdapterVendor::Amd;
    adapter.vendor_id = 0x1002u;

    const auto cap = ProbeAdapterEncoderCapability(adapter);
    EXPECT_FALSE(cap.probed);
    EXPECT_TRUE(cap.backend_label.empty());
    EXPECT_FALSE(cap.provenance.empty());
    EXPECT_FALSE(cap.h264);
    EXPECT_FALSE(cap.hevc);
    EXPECT_FALSE(cap.av1);
}

TEST(ProbeAdapterEncoderCapability, IntelAdapterIsHonestlyUnprobed) {
    AdapterInfo adapter;
    adapter.name = "Synthetic UHD Graphics 999";
    adapter.vendor = AdapterVendor::Intel;
    adapter.vendor_id = 0x8086u;

    const auto cap = ProbeAdapterEncoderCapability(adapter);
    EXPECT_FALSE(cap.probed);
    EXPECT_TRUE(cap.backend_label.empty());
    EXPECT_FALSE(cap.provenance.empty());
}

TEST(ProbeAdapterEncoderCapability, OtherVendorAdapterIsHonestlyUnprobed) {
    AdapterInfo adapter;
    adapter.name = "Synthetic Unknown Adapter";
    adapter.vendor = AdapterVendor::Other;

    const auto cap = ProbeAdapterEncoderCapability(adapter);
    EXPECT_FALSE(cap.probed);
    EXPECT_TRUE(cap.backend_label.empty());
}

// NVIDIA path touches real hardware/driver state (DXGI re-enumeration + an
// actual NVENC session open) and cannot be made deterministic without a
// physical NVIDIA GPU. This is a smoke test only: it must never crash, and
// probed=false must imply the per-codec flags stay at their honest default
// (false), never fabricated. On a headless CI runner (no NVIDIA GPU) this
// exercises the "no NVENC DLL" / "no matching adapter" early-return paths.
TEST(ProbeAdapterEncoderCapability, NvidiaAdapterNeverFabricatesResultWhenUnprobed) {
    AdapterInfo adapter;
    adapter.name = "Synthetic GeForce (no real LUID)";
    adapter.vendor = AdapterVendor::Nvidia;
    adapter.vendor_id = 0x10DEu;
    adapter.luid = 0; // does not correspond to any real DXGI adapter

    const auto cap = ProbeAdapterEncoderCapability(adapter);
    EXPECT_EQ(cap.backend_label, "NVENC"); // this IS an NVIDIA adapter, even if unprobed
    EXPECT_FALSE(cap.provenance.empty());
    if (!cap.probed) {
        EXPECT_FALSE(cap.h264);
        EXPECT_FALSE(cap.hevc);
        EXPECT_FALSE(cap.av1);
    }
}

} // namespace
} // namespace exosnap::capability
