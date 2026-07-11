#include <gtest/gtest.h>

#include <capability/resolver.h>
#include <capability/translation.h>

// Characterization tests for the static output-format reconciliation the
// resolver owns (ReconcileOutputFormat). They pin the exact end behavior the
// app layer relied on before the ownership move: the same inputs must produce
// the same fully reconciled format the settings intake and the preset
// sanitizer used to compute with their own rule copies.

namespace exosnap::capability {
namespace {

// ---------------------------------------------------------------------------
// Rule 1 — container × codec compatibility (ADR 0010 registry)
// ---------------------------------------------------------------------------

TEST(ReconcileOutputFormatTest, DefaultProfileIsUntouched) {
    // MKV + AV1 + Opus + CFR — the shipped default profile must pass through
    // without a single rule firing.
    const OutputFormatReconciliation outcome = ReconcileOutputFormat({});

    EXPECT_EQ(outcome.resolved.container, Container::Matroska);
    EXPECT_EQ(outcome.resolved.video_codec, VideoCodec::Av1Nvenc);
    EXPECT_EQ(outcome.resolved.audio_codec, AudioCodec::Opus);
    EXPECT_EQ(outcome.resolved.bit_depth, BitDepth::Bit8);
    EXPECT_EQ(outcome.resolved.chroma, ChromaSubsampling::Cs420);
    EXPECT_TRUE(outcome.resolved.cfr);
    EXPECT_FALSE(outcome.codecs_adjusted);
    EXPECT_FALSE(outcome.bit_depth_demoted);
    EXPECT_FALSE(outcome.chroma_snapped);
    EXPECT_FALSE(outcome.cfr_forced);
}

TEST(ReconcileOutputFormatTest, Mp4ForcesH264AacFromAv1Opus) {
    OutputFormatRequest request;
    request.container = Container::Mp4;
    request.video_codec = VideoCodec::Av1Nvenc;
    request.audio_codec = AudioCodec::Opus;

    const OutputFormatReconciliation outcome = ReconcileOutputFormat(request);

    EXPECT_EQ(outcome.resolved.container, Container::Mp4);
    EXPECT_EQ(outcome.resolved.video_codec, VideoCodec::H264Nvenc);
    EXPECT_EQ(outcome.resolved.audio_codec, AudioCodec::AacMf);
    EXPECT_TRUE(outcome.codecs_adjusted);
}

TEST(ReconcileOutputFormatTest, Mp4KeepsHevcAndFixesOnlyAudio) {
    OutputFormatRequest request;
    request.container = Container::Mp4;
    request.video_codec = VideoCodec::HevcNvenc;
    request.audio_codec = AudioCodec::Opus;

    const OutputFormatReconciliation outcome = ReconcileOutputFormat(request);

    // MP4 + HEVC + AAC is a working combination — the video codec survives.
    EXPECT_EQ(outcome.resolved.video_codec, VideoCodec::HevcNvenc);
    EXPECT_EQ(outcome.resolved.audio_codec, AudioCodec::AacMf);
    EXPECT_TRUE(outcome.codecs_adjusted);
}

TEST(ReconcileOutputFormatTest, WebMForcesAv1OpusFromH264Aac) {
    OutputFormatRequest request;
    request.container = Container::WebM;
    request.video_codec = VideoCodec::H264Nvenc;
    request.audio_codec = AudioCodec::AacMf;

    const OutputFormatReconciliation outcome = ReconcileOutputFormat(request);

    EXPECT_EQ(outcome.resolved.video_codec, VideoCodec::Av1Nvenc);
    EXPECT_EQ(outcome.resolved.audio_codec, AudioCodec::Opus);
    EXPECT_TRUE(outcome.codecs_adjusted);
}

// ---------------------------------------------------------------------------
// Rule 2 — 10-bit demotion (HEVC/AV1 only, ADR 0032)
// ---------------------------------------------------------------------------

TEST(ReconcileOutputFormatTest, TenBitH264DemotesToEightBit) {
    OutputFormatRequest request;
    request.video_codec = VideoCodec::H264Nvenc;
    request.audio_codec = AudioCodec::AacMf;
    request.bit_depth = BitDepth::Bit10;

    const OutputFormatReconciliation outcome = ReconcileOutputFormat(request);

    EXPECT_EQ(outcome.resolved.bit_depth, BitDepth::Bit8);
    EXPECT_TRUE(outcome.bit_depth_demoted);
}

TEST(ReconcileOutputFormatTest, TenBitSurvivesForHevcAndAv1) {
    for (const VideoCodec codec : {VideoCodec::HevcNvenc, VideoCodec::Av1Nvenc}) {
        OutputFormatRequest request;
        request.video_codec = codec;
        request.audio_codec = AudioCodec::Opus;
        request.bit_depth = BitDepth::Bit10;

        const OutputFormatReconciliation outcome = ReconcileOutputFormat(request);

        EXPECT_EQ(outcome.resolved.bit_depth, BitDepth::Bit10);
        EXPECT_FALSE(outcome.bit_depth_demoted);
    }
}

TEST(ReconcileOutputFormatTest, ContainerForcedH264AlsoDemotesTenBit) {
    // A stored MKV + AV1 + 10-bit selection switched to MP4: the container
    // forces H.264 first, and the demotion must see the forced codec — the
    // rule order the preset sanitizer and the settings intake always used.
    OutputFormatRequest request;
    request.container = Container::Mp4;
    request.video_codec = VideoCodec::Av1Nvenc;
    request.audio_codec = AudioCodec::Opus;
    request.bit_depth = BitDepth::Bit10;

    const OutputFormatReconciliation outcome = ReconcileOutputFormat(request);

    EXPECT_EQ(outcome.resolved.video_codec, VideoCodec::H264Nvenc);
    EXPECT_EQ(outcome.resolved.bit_depth, BitDepth::Bit8);
    EXPECT_TRUE(outcome.bit_depth_demoted);
}

// ---------------------------------------------------------------------------
// Rule 3 — 4:4:4 chroma snap (8-bit H.264/HEVC expert path only)
// ---------------------------------------------------------------------------

TEST(ReconcileOutputFormatTest, Chroma444SnapsToCs420ForAv1) {
    OutputFormatRequest request;
    request.video_codec = VideoCodec::Av1Nvenc;
    request.chroma = ChromaSubsampling::Cs444;

    const OutputFormatReconciliation outcome = ReconcileOutputFormat(request);

    EXPECT_EQ(outcome.resolved.chroma, ChromaSubsampling::Cs420);
    EXPECT_TRUE(outcome.chroma_snapped);
}

TEST(ReconcileOutputFormatTest, Chroma444SurvivesForEightBitH264AndHevc) {
    for (const VideoCodec codec : {VideoCodec::H264Nvenc, VideoCodec::HevcNvenc}) {
        OutputFormatRequest request;
        request.video_codec = codec;
        request.audio_codec = AudioCodec::AacMf;
        request.bit_depth = BitDepth::Bit8;
        request.chroma = ChromaSubsampling::Cs444;

        const OutputFormatReconciliation outcome = ReconcileOutputFormat(request);

        EXPECT_EQ(outcome.resolved.chroma, ChromaSubsampling::Cs444);
        EXPECT_FALSE(outcome.chroma_snapped);
    }
}

TEST(ReconcileOutputFormatTest, Chroma444SnapsWhenTenBitSurvives) {
    // HEVC keeps 10-bit, and 4:4:4 is 8-bit only — the chroma gives way, not
    // the bit depth.
    OutputFormatRequest request;
    request.video_codec = VideoCodec::HevcNvenc;
    request.audio_codec = AudioCodec::AacMf;
    request.bit_depth = BitDepth::Bit10;
    request.chroma = ChromaSubsampling::Cs444;

    const OutputFormatReconciliation outcome = ReconcileOutputFormat(request);

    EXPECT_EQ(outcome.resolved.bit_depth, BitDepth::Bit10);
    EXPECT_EQ(outcome.resolved.chroma, ChromaSubsampling::Cs420);
    EXPECT_FALSE(outcome.bit_depth_demoted);
    EXPECT_TRUE(outcome.chroma_snapped);
}

TEST(ReconcileOutputFormatTest, Chroma444SurvivesTheTenBitDemotionOnH264) {
    // H.264 + 10-bit + 4:4:4: the bit depth demotes to 8-bit FIRST, and the
    // now-8-bit H.264 selection keeps its expert 4:4:4 — the deliberate rule
    // order both prior rule copies documented and applied.
    OutputFormatRequest request;
    request.video_codec = VideoCodec::H264Nvenc;
    request.audio_codec = AudioCodec::AacMf;
    request.bit_depth = BitDepth::Bit10;
    request.chroma = ChromaSubsampling::Cs444;

    const OutputFormatReconciliation outcome = ReconcileOutputFormat(request);

    EXPECT_EQ(outcome.resolved.bit_depth, BitDepth::Bit8);
    EXPECT_EQ(outcome.resolved.chroma, ChromaSubsampling::Cs444);
    EXPECT_TRUE(outcome.bit_depth_demoted);
    EXPECT_FALSE(outcome.chroma_snapped);
}

// ---------------------------------------------------------------------------
// Rule 4 — MP4 forces CFR timing
// ---------------------------------------------------------------------------

TEST(ReconcileOutputFormatTest, Mp4ForcesCfrFromVfr) {
    OutputFormatRequest request;
    request.container = Container::Mp4;
    request.video_codec = VideoCodec::H264Nvenc;
    request.audio_codec = AudioCodec::AacMf;
    request.cfr = false;

    const OutputFormatReconciliation outcome = ReconcileOutputFormat(request);

    EXPECT_TRUE(outcome.resolved.cfr);
    EXPECT_TRUE(outcome.cfr_forced);
}

TEST(ReconcileOutputFormatTest, Mp4WithCfrAlreadyOnReportsNoForce) {
    OutputFormatRequest request;
    request.container = Container::Mp4;
    request.video_codec = VideoCodec::H264Nvenc;
    request.audio_codec = AudioCodec::AacMf;
    request.cfr = true;

    const OutputFormatReconciliation outcome = ReconcileOutputFormat(request);

    EXPECT_TRUE(outcome.resolved.cfr);
    EXPECT_FALSE(outcome.cfr_forced);
}

TEST(ReconcileOutputFormatTest, MatroskaAndWebMKeepVfr) {
    for (const Container container : {Container::Matroska, Container::WebM}) {
        OutputFormatRequest request;
        request.container = container;
        request.cfr = false;

        const OutputFormatReconciliation outcome = ReconcileOutputFormat(request);

        EXPECT_FALSE(outcome.resolved.cfr);
        EXPECT_FALSE(outcome.cfr_forced);
    }
}

// ---------------------------------------------------------------------------
// Idempotency — a reconciled format passes through unchanged
// ---------------------------------------------------------------------------

TEST(ReconcileOutputFormatTest, ReconciliationIsIdempotent) {
    OutputFormatRequest request;
    request.container = Container::Mp4;
    request.video_codec = VideoCodec::Av1Nvenc;
    request.audio_codec = AudioCodec::Opus;
    request.bit_depth = BitDepth::Bit10;
    request.chroma = ChromaSubsampling::Cs444;
    request.cfr = false;

    const OutputFormatReconciliation first = ReconcileOutputFormat(request);
    const OutputFormatReconciliation second = ReconcileOutputFormat(first.resolved);

    EXPECT_EQ(second.resolved.container, first.resolved.container);
    EXPECT_EQ(second.resolved.video_codec, first.resolved.video_codec);
    EXPECT_EQ(second.resolved.audio_codec, first.resolved.audio_codec);
    EXPECT_EQ(second.resolved.bit_depth, first.resolved.bit_depth);
    EXPECT_EQ(second.resolved.chroma, first.resolved.chroma);
    EXPECT_EQ(second.resolved.cfr, first.resolved.cfr);
    EXPECT_FALSE(second.codecs_adjusted);
    EXPECT_FALSE(second.bit_depth_demoted);
    EXPECT_FALSE(second.chroma_snapped);
    EXPECT_FALSE(second.cfr_forced);
}

// ---------------------------------------------------------------------------
// ToHdrDisplayFacts — the probe-to-engine field mapping for native HDR10
// ---------------------------------------------------------------------------

TEST(ToHdrDisplayFactsTest, MapsEveryFieldTheNativeHdrPathReads) {
    DisplayHdrFacts probe;
    probe.name = "\\\\.\\DISPLAY2";
    probe.hdr_active = true;
    probe.red_primary_x = 0.68f;
    probe.red_primary_y = 0.32f;
    probe.green_primary_x = 0.265f;
    probe.green_primary_y = 0.69f;
    probe.blue_primary_x = 0.15f;
    probe.blue_primary_y = 0.06f;
    probe.white_point_x = 0.3127f;
    probe.white_point_y = 0.329f;
    probe.max_luminance_nits = 1015.0f;
    probe.min_luminance_nits = 0.05f;

    const recorder_core::HdrDisplayFacts facts = ToHdrDisplayFacts(probe);

    EXPECT_TRUE(facts.hdr_active);
    EXPECT_FLOAT_EQ(facts.red_primary_x, 0.68f);
    EXPECT_FLOAT_EQ(facts.red_primary_y, 0.32f);
    EXPECT_FLOAT_EQ(facts.green_primary_x, 0.265f);
    EXPECT_FLOAT_EQ(facts.green_primary_y, 0.69f);
    EXPECT_FLOAT_EQ(facts.blue_primary_x, 0.15f);
    EXPECT_FLOAT_EQ(facts.blue_primary_y, 0.06f);
    EXPECT_FLOAT_EQ(facts.white_point_x, 0.3127f);
    EXPECT_FLOAT_EQ(facts.white_point_y, 0.329f);
    EXPECT_FLOAT_EQ(facts.max_luminance_nits, 1015.0f);
    EXPECT_FLOAT_EQ(facts.min_luminance_nits, 0.05f);
    // The probe has no DISPLAYCONFIG_SDR_WHITE_LEVEL reading; the engine's
    // 0 = unknown default must be preserved, never invented.
    EXPECT_FLOAT_EQ(facts.sdr_white_level_nits, 0.0f);
}

} // namespace
} // namespace exosnap::capability
