// Pins the pure decision of whether a session's pre-encode surface is shared
// with the preview and which display transform the consumer must apply
// (preview_tap.h). The rule under test: every session taps except the
// already-PQ R10G10B10A2 native sub-path, and only a native HDR10 (FP16 scRGB)
// session needs a preview-side tone-map.

#include <exosnap/engine/preview_tap.h>

#include <gtest/gtest.h>

using namespace exosnap::engine;

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
    EXPECT_EQ(ResolveRawCaptureTapDesc(DXGI_FORMAT_B8G8R8A8_UNORM, false, 0.0f, 0.0f).transform,
              PreviewTapTransform::None);
    // A 10 bpc SDR desktop composites to R10G10B10A2 but is still an SDR image.
    EXPECT_EQ(ResolveRawCaptureTapDesc(DXGI_FORMAT_R10G10B10A2_UNORM, false, 0.0f, 0.0f).transform,
              PreviewTapTransform::None);
}

TEST(RawCaptureTapDesc, HdrDesktopTonemapsWithReportedPeak) {
    // 1000-nit HDR panel: peak scale = 1000 / 80 reference-white multiples.
    const PreviewTapDesc d = ResolveRawCaptureTapDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, true, 0.0f, 1000.0f);
    EXPECT_EQ(d.transform, PreviewTapTransform::ScrgbHdr);
    EXPECT_FLOAT_EQ(d.peak_scale, 12.5f);
}

TEST(RawCaptureTapDesc, SdrAdvancedColorDesktopGetsSrgbEncodeNotRollOff) {
    // FP16 but the display is NOT HDR-active: an SDR desktop under Auto Color
    // Management. Reference white must stay white — sRGB OETF, no roll-off.
    const PreviewTapDesc d = ResolveRawCaptureTapDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, false, 0.0f, 1499.0f);
    EXPECT_EQ(d.transform, PreviewTapTransform::ScrgbSdr);
}

TEST(RawCaptureTapDesc, UnknownPeakFallsBackGracefully) {
    // HDR-active but the panel reports no luminance: HdrPeakScale's documented
    // 1000-nit fallback keeps highlights compressed instead of clipped.
    const PreviewTapDesc d = ResolveRawCaptureTapDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, true, 0.0f, 0.0f);
    EXPECT_EQ(d.transform, PreviewTapTransform::ScrgbHdr);
    EXPECT_FLOAT_EQ(d.peak_scale, 12.5f);
}

// ---- Capture-hub republish decision (DxgiCaptureHubService::WorkerProc) ----

namespace {

// One device, so every case below changes exactly the property it is about.
const DeviceGeneration kDevA{1};
const DeviceGeneration kDevB{2};

// The state a 1440p FP16 SDR desktop publishes, as the baseline each case edits.
CaptureTapPublishState Published() {
    return CaptureTapPublishState{kDevA,
                                  /*shared_valid=*/true,
                                  2560,
                                  1440,
                                  DXGI_FORMAT_R16G16B16A16_FLOAT,
                                  /*hdr_active=*/false,
                                  /*max_luminance_nits=*/0.0f};
}

CaptureTapFrameState Frame() {
    return CaptureTapFrameState{kDevA, 2560, 1440, DXGI_FORMAT_R16G16B16A16_FLOAT, false, 0.0f};
}

} // namespace

TEST(ShouldRepublishCaptureTap, NoSharedTextureYetAlwaysRepublishes) {
    CaptureTapPublishState published = Published();
    published.shared_valid = false;
    EXPECT_TRUE(ShouldRepublishCaptureTap(published, Frame()));
}

TEST(ShouldRepublishCaptureTap, DimensionOrFormatChangeRepublishes) {
    CaptureTapPublishState smaller = Published();
    smaller.width = 1920;
    smaller.height = 1080;
    EXPECT_TRUE(ShouldRepublishCaptureTap(smaller, Frame()));

    CaptureTapPublishState otherFormat = Published();
    otherFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    EXPECT_TRUE(ShouldRepublishCaptureTap(otherFormat, Frame()));
}

TEST(ShouldRepublishCaptureTap, UnchangedFrameWithUnchangedFactsDoesNotRepublish) {
    CaptureTapPublishState published = Published();
    published.hdr_active = true;
    published.max_luminance_nits = 1000.0f;
    CaptureTapFrameState frame = Frame();
    frame.hdr_active = true;
    frame.max_luminance_nits = 1000.0f;
    EXPECT_FALSE(ShouldRepublishCaptureTap(published, frame));
}

TEST(ShouldRepublishCaptureTap, HdrActiveToggleWithUnchangedFormatStillRepublishes) {
    // An Advanced-Color desktop keeps delivering FP16 across a live Windows-HDR
    // (or Auto-HDR) toggle, so dimensions and format alone never notice the
    // display's HDR state flipped underneath the already-shared texture. Before
    // that fix the preview stayed tone-mapped with a stale peak and transform
    // until an unrelated resolution change forced a refresh.
    CaptureTapFrameState frame = Frame();
    frame.hdr_active = true;
    frame.max_luminance_nits = 1000.0f;
    EXPECT_TRUE(ShouldRepublishCaptureTap(Published(), frame));
}

TEST(ShouldRepublishCaptureTap, MaxLuminanceChangeAloneRepublishes) {
    // The display's reported peak can change (e.g. a driver renegotiation) while
    // hdr_active stays true; the stale peak_scale must still be refreshed.
    CaptureTapPublishState published = Published();
    published.hdr_active = true;
    published.max_luminance_nits = 400.0f;
    CaptureTapFrameState frame = Frame();
    frame.hdr_active = true;
    frame.max_luminance_nits = 1000.0f;
    EXPECT_TRUE(ShouldRepublishCaptureTap(published, frame));
}

TEST(ShouldRepublishCaptureTap, AReplacedDeviceRepublishesThoughNothingElseChanged) {
    // The defect. After a DEVICE_REMOVED or an adapter-matched reopen the desktop
    // is the same size, the same format, in the same HDR state -- and the shared
    // texture belongs to a device that no longer exists. Every other comparison
    // in this function says "carry on", so the generation is the only thing that
    // can say otherwise, and the consumer was handed a handle into a dead device.
    CaptureTapFrameState frame = Frame();
    frame.device_generation = kDevB;
    EXPECT_TRUE(ShouldRepublishCaptureTap(Published(), frame));
}

TEST(ShouldRepublishCaptureTap, NoDeviceIsNeverCurrent) {
    // Between Close() and the next Open() the producer has no generation. A
    // dependent must not treat "no device on either side" as "unchanged".
    CaptureTapPublishState published = Published();
    published.device_generation = DeviceGeneration{};
    CaptureTapFrameState frame = Frame();
    frame.device_generation = DeviceGeneration{};
    EXPECT_TRUE(ShouldRepublishCaptureTap(published, frame));

    // And a texture published before any device existed is never current either.
    EXPECT_TRUE(ShouldRepublishCaptureTap(published, Frame()));
}

// ---- DeviceResourceIsCurrent, the rule all three hubs share ----

TEST(DeviceGenerationRule, TheSameGenerationIsCurrent) {
    EXPECT_TRUE(DeviceResourceIsCurrent(kDevA, kDevA));
}

TEST(DeviceGenerationRule, ADifferentGenerationIsNot) {
    EXPECT_FALSE(DeviceResourceIsCurrent(kDevA, kDevB));
    EXPECT_FALSE(DeviceResourceIsCurrent(kDevB, kDevA));
}

TEST(DeviceGenerationRule, NothingBuiltIsNotTheSameAsStillGood) {
    const DeviceGeneration none{};
    EXPECT_FALSE(DeviceResourceIsCurrent(none, kDevA)) << "a dependent built without a device must be rebuilt";
    EXPECT_FALSE(DeviceResourceIsCurrent(kDevA, none)) << "a resource cannot be current against no device";
    EXPECT_FALSE(DeviceResourceIsCurrent(none, none)) << "two absences are not a match";
}

TEST(DeviceGenerationRule, GenerationsAreUniqueAndMonotonic) {
    // Process-wide: two producers must never mint the same number for different
    // devices, or a dependent following a source change between them sees no
    // difference where there is one.
    const DeviceGeneration first = NextDeviceGeneration();
    const DeviceGeneration second = NextDeviceGeneration();
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(second.Valid());
    EXPECT_NE(first, second);
    EXPECT_GT(second.value(), first.value());
    EXPECT_NE(first.value(), DeviceGeneration::kNoDevice);
}
// The SDR reference white travels with the tap so the consumer applies the same
// normalisation the engine does. It is meaningful for the HDR transform only:
// an SDR Advanced-Color desktop is display-referred, where 1.0 already IS the
// display's reference white and dividing would darken a correct picture.
TEST(PreviewTapPlanTest, PaperWhiteScaleIsCarriedForHdrAndNeutralOtherwise) {
    const PreviewTapDesc hdr = ResolveRawCaptureTapDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, true, 280.0f, 1000.0f);
    EXPECT_EQ(hdr.transform, PreviewTapTransform::ScrgbHdr);
    EXPECT_NEAR(hdr.paper_white_scale, 280.0f / 80.0f, 1e-5f);

    const PreviewTapDesc sdr_scrgb = ResolveRawCaptureTapDesc(DXGI_FORMAT_R16G16B16A16_FLOAT, false, 280.0f, 0.0f);
    EXPECT_EQ(sdr_scrgb.transform, PreviewTapTransform::ScrgbSdr);
    EXPECT_FLOAT_EQ(sdr_scrgb.paper_white_scale, 1.0f);

    const PreviewTapDesc plain = ResolveRawCaptureTapDesc(DXGI_FORMAT_B8G8R8A8_UNORM, true, 280.0f, 1000.0f);
    EXPECT_EQ(plain.transform, PreviewTapTransform::None);
    EXPECT_FLOAT_EQ(plain.paper_white_scale, 1.0f);
}
