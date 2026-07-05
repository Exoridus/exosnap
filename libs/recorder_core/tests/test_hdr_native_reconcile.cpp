#include <gtest/gtest.h>

#include <recorder_core/codec_types.h>
#include <recorder_core/hdr_native.h>
#include <recorder_core/recorder_session.h>

#include <filesystem>

// Coordinator-seam tests for the native HDR10 encode reconcile
// (ApplyHdr10NativeEncode), the pure function the RecordingCoordinator applies
// once IsHdr10NativeEffective() is true. The regression it guards: the UI allows
// HEVC + 8-bit + native-HDR10 + 4:4:4 together; the HDR10 path then pins 10-bit,
// and 4:4:4 (AYUV) is an 8-bit-only path — so without a chroma snap the resulting
// Cs444 + Bit10 config is rejected by RecorderSession::Validate() ("Cs444 is 8-bit
// only") and recording start fails on any HDR-active display.

namespace {

using recorder_core::AudioCodec;
using recorder_core::BitDepth;
using recorder_core::CaptureTarget;
using recorder_core::ChromaSubsampling;
using recorder_core::Container;
using recorder_core::HdrDisplayFacts;
using recorder_core::HdrMode;
using recorder_core::RecorderConfig;
using recorder_core::RecorderResult;
using recorder_core::RecorderSession;
using recorder_core::VideoCodec;

// A valid HEVC + MKV base config mirroring what the UI/translation produces for
// the "HEVC + 8-bit + 4:4:4 + native HDR10" selection before the coordinator's
// HDR10 reconcile runs.
RecorderConfig MakeHevc444Hdr10Config() {
    RecorderConfig cfg{};
    cfg.output_path = std::filesystem::temp_directory_path() / "exosnap_hdr_reconcile_test.mkv";
    cfg.container = Container::Matroska;
    cfg.video_codec = VideoCodec::HevcNvenc;
    cfg.audio_codec = AudioCodec::Opus;
    cfg.audio_sample_rate = 48000;
    cfg.audio_channels = 2;
    cfg.chroma = ChromaSubsampling::Cs444;
    cfg.bit_depth = BitDepth::Bit8;
    cfg.hdr_mode = HdrMode::Hdr10;
    cfg.target.kind = CaptureTarget::Kind::Monitor;
    cfg.target.native_id = 1;
    return cfg;
}

// HDR-active display facts with real chromaticity/luminance readings.
HdrDisplayFacts MakeActiveHdrFacts() {
    HdrDisplayFacts facts;
    facts.hdr_active = true;
    facts.red_primary_x = 0.680f;
    facts.red_primary_y = 0.320f;
    facts.green_primary_x = 0.265f;
    facts.green_primary_y = 0.690f;
    facts.blue_primary_x = 0.150f;
    facts.blue_primary_y = 0.060f;
    facts.white_point_x = 0.3127f;
    facts.white_point_y = 0.3290f;
    facts.max_luminance_nits = 1000.0f;
    facts.min_luminance_nits = 0.005f;
    return facts;
}

// The regression under test: HDR10-effective + Cs444 reconciles to Cs420 + Bit10
// and the session config then passes Validate() (recording start no longer fails).
TEST(HdrNativeReconcileTest, Hdr10With444SnapsToCs420AndValidates) {
    // Gate the coordinator checks before applying the reconcile.
    ASSERT_TRUE(
        recorder_core::IsHdr10NativeEffective(HdrMode::Hdr10, /*display_hdr_active=*/true, VideoCodec::HevcNvenc));

    RecorderConfig cfg = MakeHevc444Hdr10Config();
    const bool chroma_snapped = recorder_core::ApplyHdr10NativeEncode(cfg, MakeActiveHdrFacts());

    EXPECT_TRUE(chroma_snapped);
    EXPECT_EQ(cfg.chroma, ChromaSubsampling::Cs420);
    EXPECT_EQ(cfg.bit_depth, BitDepth::Bit10);
    EXPECT_TRUE(cfg.color.hdr);

    RecorderSession session;
    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result)) << result.error_detail;
    EXPECT_TRUE(result.succeeded);
}

// Guard/documentation: the un-reconciled combination (Cs444 + Bit10) is exactly
// what Validate() rejects — this is why the snap is required.
TEST(HdrNativeReconcileTest, Cs444With10BitIsRejectedWithoutSnap) {
    RecorderConfig cfg = MakeHevc444Hdr10Config();
    cfg.bit_depth = BitDepth::Bit10; // pin 10-bit but skip the chroma snap

    RecorderSession session;
    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_FALSE(result.succeeded);
    EXPECT_NE(result.error_detail.find("Cs444"), std::string::npos);
}

// A Cs420 session already at 4:2:0 is untouched by the reconcile (no spurious
// snap reported), and still pins 10-bit HDR10 metadata.
TEST(HdrNativeReconcileTest, Cs420SessionReportsNoSnap) {
    RecorderConfig cfg = MakeHevc444Hdr10Config();
    cfg.chroma = ChromaSubsampling::Cs420;

    const bool chroma_snapped = recorder_core::ApplyHdr10NativeEncode(cfg, MakeActiveHdrFacts());

    EXPECT_FALSE(chroma_snapped);
    EXPECT_EQ(cfg.chroma, ChromaSubsampling::Cs420);
    EXPECT_EQ(cfg.bit_depth, BitDepth::Bit10);
}

// Frame-snapshot fail-fast: RequestFrameSnapshot rejects a 4:4:4 session at
// request time (FrameSnapshotSupported == false) instead of parking the request
// until stop, so the CaptureFrame hotkey / notification layer reacts immediately.
TEST(HdrNativeReconcileTest, FrameSnapshotUnavailableFor444) {
    EXPECT_FALSE(recorder_core::FrameSnapshotSupported(ChromaSubsampling::Cs444));
    EXPECT_TRUE(recorder_core::FrameSnapshotSupported(ChromaSubsampling::Cs420));
}

} // namespace
