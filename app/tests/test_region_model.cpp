#include <gtest/gtest.h>

#include <filesystem>
#include <recorder_core/recorder_session.h>

namespace {

using recorder_core::CaptureRegion;
using recorder_core::CaptureTarget;
using recorder_core::ErrorPhase;
using recorder_core::RecorderConfig;
using recorder_core::RecorderSession;

// ---------------------------------------------------------------------------
// CaptureRegion::IsValid
// ---------------------------------------------------------------------------

TEST(CaptureRegion, ValidAcceptsMinimumDimensions) {
    CaptureRegion r;
    r.width = CaptureRegion::kMinDimension;
    r.height = CaptureRegion::kMinDimension;
    EXPECT_TRUE(r.IsValid());
}

TEST(CaptureRegion, ValidAcceptsLargeDimensions) {
    CaptureRegion r;
    r.x = 100;
    r.y = 200;
    r.width = 1920;
    r.height = 1080;
    EXPECT_TRUE(r.IsValid());
}

TEST(CaptureRegion, InvalidWhenWidthTooSmall) {
    CaptureRegion r;
    r.width = CaptureRegion::kMinDimension - 1;
    r.height = CaptureRegion::kMinDimension;
    EXPECT_FALSE(r.IsValid());
}

TEST(CaptureRegion, InvalidWhenHeightTooSmall) {
    CaptureRegion r;
    r.width = CaptureRegion::kMinDimension;
    r.height = CaptureRegion::kMinDimension - 1;
    EXPECT_FALSE(r.IsValid());
}

TEST(CaptureRegion, InvalidWhenBothDimensionsZero) {
    CaptureRegion r;
    EXPECT_FALSE(r.IsValid());
}

TEST(CaptureRegion, InvalidWhenNegativeWidth) {
    CaptureRegion r;
    r.width = -100;
    r.height = CaptureRegion::kMinDimension;
    EXPECT_FALSE(r.IsValid());
}

TEST(CaptureRegion, InvalidWhenNegativeHeight) {
    CaptureRegion r;
    r.width = CaptureRegion::kMinDimension;
    r.height = -1;
    EXPECT_FALSE(r.IsValid());
}

// ---------------------------------------------------------------------------
// RecorderSession::Validate — crop_region rules
// ---------------------------------------------------------------------------

// Helper: build a minimal valid config (no audio, no target PID).
static RecorderConfig MakeBaseConfig() {
    RecorderConfig cfg;
    cfg.output_path = std::filesystem::temp_directory_path() / "region_test_output.mkv";
    cfg.target.kind = CaptureTarget::Kind::Monitor;
    cfg.target.native_id = 1; // non-zero sentinel
    cfg.container = recorder_core::Container::Matroska;
    cfg.video_codec = recorder_core::VideoCodec::H264Nvenc;
    cfg.audio_codec = recorder_core::AudioCodec::AacMf;
    cfg.frame_rate_num = 60;
    cfg.frame_rate_den = 1;
    cfg.record_audio = false;
    return cfg;
}

TEST(CaptureRegionValidation, ValidCropOnMonitorPasses) {
    RecorderSession session;
    RecorderConfig cfg = MakeBaseConfig();
    CaptureRegion region;
    region.x = 0;
    region.y = 0;
    region.width = 640;
    region.height = 480;
    cfg.crop_region = region;

    recorder_core::RecorderResult result;
    EXPECT_TRUE(session.Validate(cfg, &result));
}

TEST(CaptureRegionValidation, NoCropOnMonitorPasses) {
    RecorderSession session;
    RecorderConfig cfg = MakeBaseConfig();
    // crop_region not set

    recorder_core::RecorderResult result;
    EXPECT_TRUE(session.Validate(cfg, &result));
}

TEST(CaptureRegionValidation, CropOnWindowTargetFails) {
    RecorderSession session;
    RecorderConfig cfg = MakeBaseConfig();
    cfg.target.kind = CaptureTarget::Kind::Window;

    CaptureRegion region;
    region.width = 640;
    region.height = 480;
    cfg.crop_region = region;

    recorder_core::RecorderResult result;
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
}

TEST(CaptureRegionValidation, TooSmallCropFails) {
    RecorderSession session;
    RecorderConfig cfg = MakeBaseConfig();

    CaptureRegion region;
    region.width = CaptureRegion::kMinDimension - 1;
    region.height = CaptureRegion::kMinDimension;
    cfg.crop_region = region;

    recorder_core::RecorderResult result;
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
}

TEST(CaptureRegionValidation, BothDimensionsAtMinimumPasses) {
    RecorderSession session;
    RecorderConfig cfg = MakeBaseConfig();

    CaptureRegion region;
    region.width = CaptureRegion::kMinDimension;
    region.height = CaptureRegion::kMinDimension;
    cfg.crop_region = region;

    recorder_core::RecorderResult result;
    EXPECT_TRUE(session.Validate(cfg, &result));
}

} // namespace
