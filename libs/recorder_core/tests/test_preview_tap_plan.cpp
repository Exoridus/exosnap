// Pins the pure decision of whether a session's pre-encode surface is shared
// with the preview and which display transform the consumer must apply
// (preview_tap.h). The rule under test: every session taps except the
// already-PQ R10G10B10A2 native sub-path, and only a native HDR10 (FP16 scRGB)
// session needs a preview-side tone-map.

#include <recorder_core/preview_tap.h>

#include <gtest/gtest.h>

using namespace recorder_core;

TEST(PreviewTapPlan, SdrSessionTapsWithoutTransform) {
    const PreviewTapPlan plan = ResolvePreviewTapPlan(false, false, 1.0f);
    EXPECT_TRUE(plan.tap_enabled);
    EXPECT_EQ(plan.desc.transform, PreviewTapTransform::None);
    EXPECT_FLOAT_EQ(plan.desc.peak_scale, 1.0f);
}

TEST(PreviewTapPlan, ToneMappedSessionTapsWithoutTransform) {
    // A tone-mapped HDR session already shares an SDR surface; its peak scale
    // was consumed engine-side and must not leak into the consumer transform.
    const PreviewTapPlan plan = ResolvePreviewTapPlan(false, false, 12.5f);
    EXPECT_TRUE(plan.tap_enabled);
    EXPECT_EQ(plan.desc.transform, PreviewTapTransform::None);
    EXPECT_FLOAT_EQ(plan.desc.peak_scale, 1.0f);
}

TEST(PreviewTapPlan, NativeHdrTapsWithScrgbToneMapAndSessionPeak) {
    const PreviewTapPlan plan = ResolvePreviewTapPlan(true, false, 12.5f);
    EXPECT_TRUE(plan.tap_enabled);
    EXPECT_EQ(plan.desc.transform, PreviewTapTransform::ScrgbHdr);
    EXPECT_FLOAT_EQ(plan.desc.peak_scale, 12.5f);
}

TEST(PreviewTapPlan, AlreadyPqNativeSubPathDoesNotTap) {
    // R10G10B10A2 PQ desktop: non-linear surface, no linear intermediate to
    // share — the preview keeps its own WGC capture (see the design doc).
    const PreviewTapPlan plan = ResolvePreviewTapPlan(true, true, 12.5f);
    EXPECT_FALSE(plan.tap_enabled);
    EXPECT_EQ(plan.desc.transform, PreviewTapTransform::None);
}

TEST(PreviewTapPlan, PqFlagWithoutNativeIsIgnored) {
    // pq_input_is_pq is only ever set for native sessions; if it leaks in for a
    // non-native one the tap must still behave like plain SDR.
    const PreviewTapPlan plan = ResolvePreviewTapPlan(false, true, 1.0f);
    EXPECT_TRUE(plan.tap_enabled);
    EXPECT_EQ(plan.desc.transform, PreviewTapTransform::None);
}
