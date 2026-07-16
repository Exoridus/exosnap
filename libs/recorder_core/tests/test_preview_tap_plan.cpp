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

// ---- Raw captured desktop frames (idle DXGI-hub source, no session policy) ----

TEST(RawCaptureTapDesc, SdrDesktopFormatsDrawAsIs) {
    EXPECT_EQ(ResolveRawCaptureTapDesc(DXGI_FORMAT_B8G8R8A8_UNORM, false, 0.0f).transform, PreviewTapTransform::None);
    // A 10 bpc SDR desktop composites to R10G10B10A2 but is still an SDR image.
    EXPECT_EQ(ResolveRawCaptureTapDesc(DXGI_FORMAT_R10G10B10A2_UNORM, false, 0.0f).transform,
              PreviewTapTransform::None);
}

TEST(RawCaptureTapDesc, HdrDesktopTonemapsWithReportedPeak) {
    // 1000-nit HDR panel: peak scale = 1000 / 80 reference-white multiples.
    const PreviewTapDesc d = ResolveRawCaptureTapDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, true, 1000.0f);
    EXPECT_EQ(d.transform, PreviewTapTransform::ScrgbHdr);
    EXPECT_FLOAT_EQ(d.peak_scale, 12.5f);
}

TEST(RawCaptureTapDesc, SdrAdvancedColorDesktopGetsSrgbEncodeNotRollOff) {
    // FP16 but the display is NOT HDR-active: an SDR desktop under Auto Color
    // Management. Reference white must stay white — sRGB OETF, no roll-off.
    const PreviewTapDesc d = ResolveRawCaptureTapDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, false, 1499.0f);
    EXPECT_EQ(d.transform, PreviewTapTransform::ScrgbSdr);
}

TEST(RawCaptureTapDesc, UnknownPeakFallsBackGracefully) {
    // HDR-active but the panel reports no luminance: HdrPeakScale's documented
    // 1000-nit fallback keeps highlights compressed instead of clipped.
    const PreviewTapDesc d = ResolveRawCaptureTapDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, true, 0.0f);
    EXPECT_EQ(d.transform, PreviewTapTransform::ScrgbHdr);
    EXPECT_FLOAT_EQ(d.peak_scale, 12.5f);
}

// ---- Capture-hub republish decision (DxgiCaptureHubService::WorkerProc) ----

TEST(ShouldRepublishCaptureTap, NoSharedTextureYetAlwaysRepublishes) {
    EXPECT_TRUE(ShouldRepublishCaptureTap(/*shared_valid=*/false, 0, 0, DXGI_FORMAT_UNKNOWN, 2560, 1440,
                                          DXGI_FORMAT_R16G16B16A16_FLOAT,
                                          /*last_hdr_active=*/false, /*last_max_luminance_nits=*/0.0f,
                                          /*current_hdr_active=*/false, /*current_max_luminance_nits=*/0.0f));
}

TEST(ShouldRepublishCaptureTap, DimensionOrFormatChangeRepublishes) {
    EXPECT_TRUE(ShouldRepublishCaptureTap(/*shared_valid=*/true, 1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT, 2560, 1440,
                                          DXGI_FORMAT_R16G16B16A16_FLOAT, false, 0.0f, false, 0.0f));
    EXPECT_TRUE(ShouldRepublishCaptureTap(/*shared_valid=*/true, 2560, 1440, DXGI_FORMAT_B8G8R8A8_UNORM, 2560, 1440,
                                          DXGI_FORMAT_R16G16B16A16_FLOAT, false, 0.0f, false, 0.0f));
}

TEST(ShouldRepublishCaptureTap, UnchangedFrameWithUnchangedFactsDoesNotRepublish) {
    EXPECT_FALSE(ShouldRepublishCaptureTap(/*shared_valid=*/true, 2560, 1440, DXGI_FORMAT_R16G16B16A16_FLOAT, 2560,
                                           1440, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                           /*last_hdr_active=*/true, /*last_max_luminance_nits=*/1000.0f,
                                           /*current_hdr_active=*/true, /*current_max_luminance_nits=*/1000.0f));
}

TEST(ShouldRepublishCaptureTap, HdrActiveToggleWithUnchangedFormatStillRepublishes) {
    // The regression this guards: an Advanced-Color desktop keeps delivering
    // FP16 across a live Windows-HDR (or Auto-HDR) toggle, so dimensions/format
    // alone never notice the display's HDR state flipped underneath the
    // already-shared texture. Before the fix this returned false, leaving the
    // preview tone-mapped with a stale peak/transform until an unrelated
    // resolution change forced a refresh.
    EXPECT_TRUE(ShouldRepublishCaptureTap(/*shared_valid=*/true, 2560, 1440, DXGI_FORMAT_R16G16B16A16_FLOAT, 2560, 1440,
                                          DXGI_FORMAT_R16G16B16A16_FLOAT,
                                          /*last_hdr_active=*/false, /*last_max_luminance_nits=*/0.0f,
                                          /*current_hdr_active=*/true, /*current_max_luminance_nits=*/1000.0f));
}

TEST(ShouldRepublishCaptureTap, MaxLuminanceChangeAloneRepublishes) {
    // The display's reported peak can change (e.g. a driver renegotiation) while
    // hdr_active stays true; the stale peak_scale must still be refreshed.
    EXPECT_TRUE(ShouldRepublishCaptureTap(/*shared_valid=*/true, 2560, 1440, DXGI_FORMAT_R16G16B16A16_FLOAT, 2560, 1440,
                                          DXGI_FORMAT_R16G16B16A16_FLOAT,
                                          /*last_hdr_active=*/true, /*last_max_luminance_nits=*/400.0f,
                                          /*current_hdr_active=*/true, /*current_max_luminance_nits=*/1000.0f));
}
