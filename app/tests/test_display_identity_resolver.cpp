#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "services/DisplayIdentityResolver.h"

namespace exosnap {
namespace {

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

StableDisplayId MakeId(std::string device_path, std::string vendor, uint32_t product, std::string serial,
                       std::string friendly, std::string gdi) {
    StableDisplayId id;
    id.device_path = std::move(device_path);
    id.edid_vendor = std::move(vendor);
    id.edid_product = product;
    id.serial = std::move(serial);
    id.friendly_name = std::move(friendly);
    id.gdi_name = std::move(gdi);
    return id;
}

EnumeratedDisplayIdentity MakeEnum(StableDisplayId id, uintptr_t hmon, PhysicalRect rc) {
    EnumeratedDisplayIdentity e;
    e.id = std::move(id);
    e.hmonitor = hmon;
    e.rc_monitor_physical = rc;
    return e;
}

// ===========================================================================
// Ranked matcher
// ===========================================================================

TEST(DisplayIdentityResolver, EmptySavedIdIsNoPreference) {
    std::vector<EnumeratedDisplayIdentity> list{
        MakeEnum(MakeId("\\\\?\\A", "GSM", 100, "", "LG A", "\\\\.\\DISPLAY1"), 1, {0, 0, 1920, 1080})};
    EXPECT_FALSE(ResolveStableDisplay(StableDisplayId{}, list).has_value());
}

TEST(DisplayIdentityResolver, Stage1DevicePathExactWins) {
    std::vector<EnumeratedDisplayIdentity> list{
        MakeEnum(MakeId("\\\\?\\PATH-A", "GSM", 100, "S1", "LG A", "\\\\.\\DISPLAY1"), 1, {0, 0, 1920, 1080}),
        MakeEnum(MakeId("\\\\?\\PATH-B", "GSM", 100, "S2", "LG A", "\\\\.\\DISPLAY2"), 2, {1920, 0, 3840, 1080})};
    // Saved carries PATH-B but its GDI name drifted to DISPLAY1: device_path must win.
    StableDisplayId saved = MakeId("\\\\?\\PATH-B", "GSM", 100, "S2", "LG A", "\\\\.\\DISPLAY1");
    auto m = ResolveStableDisplay(saved, list);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->index, 1u);
    EXPECT_EQ(m->confidence, DisplayMatchConfidence::DevicePath);
}

TEST(DisplayIdentityResolver, Stage2SamePanelBySerialAtNewPort) {
    // Panel moved to another connector: device_path changed, serial follows it.
    std::vector<EnumeratedDisplayIdentity> list{
        MakeEnum(MakeId("\\\\?\\OTHER", "DEL", 200, "OTHER", "Dell", "\\\\.\\DISPLAY1"), 1, {0, 0, 1920, 1080}),
        MakeEnum(MakeId("\\\\?\\NEWPORT", "GSM", 100, "PANEL-SERIAL", "LG A", "\\\\.\\DISPLAY2"), 2,
                 {1920, 0, 3840, 1080})};
    StableDisplayId saved = MakeId("\\\\?\\OLDPORT", "GSM", 100, "PANEL-SERIAL", "LG A", "\\\\.\\DISPLAY3");
    auto m = ResolveStableDisplay(saved, list);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->index, 1u);
    EXPECT_EQ(m->confidence, DisplayMatchConfidence::Serial);
}

TEST(DisplayIdentityResolver, Stage3UniqueModelSingleMonitor) {
    std::vector<EnumeratedDisplayIdentity> list{
        MakeEnum(MakeId("\\\\?\\PORT", "GSM", 100, "", "LG 4K", "\\\\.\\DISPLAY1"), 1, {0, 0, 1920, 1080})};
    // device_path changed, no serial, but it's the only monitor of this model.
    StableDisplayId saved = MakeId("\\\\?\\OLD", "GSM", 100, "", "LG 4K", "\\\\.\\DISPLAY9");
    auto m = ResolveStableDisplay(saved, list);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->index, 0u);
    EXPECT_EQ(m->confidence, DisplayMatchConfidence::FriendlyName);
}

TEST(DisplayIdentityResolver, TwinsWithSerialsDisambiguate) {
    std::vector<EnumeratedDisplayIdentity> list{
        MakeEnum(MakeId("\\\\?\\P1", "GSM", 100, "SER-A", "LG", "\\\\.\\DISPLAY1"), 1, {0, 0, 1920, 1080}),
        MakeEnum(MakeId("\\\\?\\P2", "GSM", 100, "SER-B", "LG", "\\\\.\\DISPLAY2"), 2, {1920, 0, 3840, 1080})};
    StableDisplayId saved = MakeId("\\\\?\\GONE", "GSM", 100, "SER-B", "LG", "\\\\.\\DISPLAY7");
    auto m = ResolveStableDisplay(saved, list);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->index, 1u);
    EXPECT_EQ(m->confidence, DisplayMatchConfidence::Serial);
}

TEST(DisplayIdentityResolver, TwinsWithoutSerialAfterCableSwapAreUnresolved) {
    // Two identical monitors, no serials, and the saved device_path matches
    // neither current connector (cables swapped). Must NOT guess.
    std::vector<EnumeratedDisplayIdentity> list{
        MakeEnum(MakeId("\\\\?\\P1", "GSM", 100, "", "LG", "\\\\.\\DISPLAY1"), 1, {0, 0, 1920, 1080}),
        MakeEnum(MakeId("\\\\?\\P2", "GSM", 100, "", "LG", "\\\\.\\DISPLAY2"), 2, {1920, 0, 3840, 1080})};
    StableDisplayId saved = MakeId("\\\\?\\GONE", "GSM", 100, "", "LG", "\\\\.\\DISPLAY1");
    EXPECT_FALSE(ResolveStableDisplay(saved, list).has_value());
}

TEST(DisplayIdentityResolver, RichIdentityMissDoesNotFallBackToGdi) {
    // Saved carries a device_path (rich identity). No enumerated device_path,
    // serial, or unique model matches -> UNRESOLVED, even though a GDI name
    // collides (that would be the silent mismatch this feature removes).
    std::vector<EnumeratedDisplayIdentity> list{
        MakeEnum(MakeId("\\\\?\\OTHER", "DEL", 55, "z", "Dell", "\\\\.\\DISPLAY1"), 1, {0, 0, 1920, 1080})};
    StableDisplayId saved = MakeId("\\\\?\\MISSING", "GSM", 100, "sx", "LG", "\\\\.\\DISPLAY1");
    EXPECT_FALSE(ResolveStableDisplay(saved, list).has_value());
}

TEST(DisplayIdentityResolver, DegradedIdentityFallsBackToGdiName) {
    // DisplayConfig failed at save time: only gdi_name was captured. Fall back
    // to the historical GDI-name match — never worse than today.
    std::vector<EnumeratedDisplayIdentity> list{
        MakeEnum(MakeId("\\\\?\\PATH", "GSM", 100, "", "LG", "\\\\.\\DISPLAY2"), 2, {0, 0, 1920, 1080})};
    StableDisplayId saved;
    saved.gdi_name = "\\\\.\\DISPLAY2";
    auto m = ResolveStableDisplay(saved, list);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->index, 0u);
    EXPECT_EQ(m->confidence, DisplayMatchConfidence::GdiName);
}

// ===========================================================================
// Region math
// ===========================================================================

TEST(DisplayIdentityResolver, RegionRoundTripSimple) {
    PhysicalRect anchor{0, 0, 1920, 1080};
    AbsoluteRegion region{192, 108, 960, 540};
    NormalizedRegion norm = AbsoluteRegionToAnchorRelative(region, anchor);
    EXPECT_FLOAT_EQ(norm.x, 0.1f);
    EXPECT_FLOAT_EQ(norm.y, 0.1f);
    EXPECT_FLOAT_EQ(norm.w, 0.5f);
    EXPECT_FLOAT_EQ(norm.h, 0.5f);
    AbsoluteRegion back = AnchorRelativeRegionToAbsolute(norm, anchor);
    EXPECT_EQ(back, region);
}

TEST(DisplayIdentityResolver, RegionProportionalAfterResolutionChange) {
    PhysicalRect anchor_1080{0, 0, 1920, 1080};
    AbsoluteRegion region{192, 108, 960, 540}; // 10%,10%,50%,50%
    NormalizedRegion norm = AbsoluteRegionToAnchorRelative(region, anchor_1080);

    // Same monitor, now at 4K: rectangle scales proportionally.
    PhysicalRect anchor_2160{0, 0, 3840, 2160};
    AbsoluteRegion scaled = AnchorRelativeRegionToAbsolute(norm, anchor_2160);
    EXPECT_EQ(scaled, (AbsoluteRegion{384, 216, 1920, 1080}));
}

TEST(DisplayIdentityResolver, RegionRoundTripOnScaledOffsetAnchorPhysicalPixels) {
    // A 4K monitor at 150% scaling and offset to the right: its PHYSICAL
    // rcMonitor is {1920,0,3840,2160} even though its logical Qt geometry would
    // be ~1280 wide. Region math must use these physical pixels; the round-trip
    // proves we did not normalize against a logical geometry.
    PhysicalRect anchor{1920, 0, 3840, 2160};
    AbsoluteRegion region{2112, 108, 960, 1080}; // origin at physical 2112,108
    NormalizedRegion norm = AbsoluteRegionToAnchorRelative(region, anchor);
    EXPECT_FLOAT_EQ(norm.x, 0.1f);
    EXPECT_FLOAT_EQ(norm.y, 0.05f);
    EXPECT_FLOAT_EQ(norm.w, 0.5f);
    EXPECT_FLOAT_EQ(norm.h, 0.5f);
    AbsoluteRegion back = AnchorRelativeRegionToAbsolute(norm, anchor);
    EXPECT_EQ(back, region);
}

TEST(DisplayIdentityResolver, RegionClampedInsideAnchor) {
    PhysicalRect anchor{0, 0, 1000, 1000};
    // A normalized region that would extend past the edges is clamped.
    NormalizedRegion norm{0.8f, 0.8f, 0.5f, 0.5f};
    AbsoluteRegion abs = AnchorRelativeRegionToAbsolute(norm, anchor);
    EXPECT_GE(abs.x, anchor.left);
    EXPECT_GE(abs.y, anchor.top);
    EXPECT_LE(abs.x + abs.width, anchor.right);
    EXPECT_LE(abs.y + abs.height, anchor.bottom);
}

TEST(DisplayIdentityResolver, DegenerateAnchorYieldsEmpty) {
    PhysicalRect bad{0, 0, 0, 0};
    EXPECT_EQ(AbsoluteRegionToAnchorRelative(AbsoluteRegion{1, 2, 3, 4}, bad), NormalizedRegion{});
    EXPECT_EQ(AnchorRelativeRegionToAbsolute(NormalizedRegion{0.1f, 0.1f, 0.5f, 0.5f}, bad), AbsoluteRegion{});
}

} // namespace
} // namespace exosnap
