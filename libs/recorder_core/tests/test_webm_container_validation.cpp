#include <gtest/gtest.h>

#include <recorder_core/recorder_session.h>

#include <windows.h>

#include <filesystem>

namespace {

using recorder_core::AudioCodec;
using recorder_core::CaptureTarget;
using recorder_core::Container;
using recorder_core::ErrorPhase;
using recorder_core::RecorderConfig;
using recorder_core::RecorderResult;
using recorder_core::RecorderSession;
using recorder_core::VideoCodec;

RecorderConfig MakeValidWebMConfig() {
    RecorderConfig cfg{};
    cfg.output_path = std::filesystem::current_path() / "test_webm_container.webm";
    cfg.target.kind = CaptureTarget::Kind::Monitor;
    cfg.target.native_id = 1;
    cfg.container = Container::WebM;
    cfg.audio_codec = AudioCodec::Opus;
    return cfg;
}

RecorderConfig MakeValidMatroskaConfig() {
    RecorderConfig cfg{};
    cfg.output_path = std::filesystem::current_path() / "test_matroska_container.mkv";
    cfg.target.kind = CaptureTarget::Kind::Monitor;
    cfg.target.native_id = 1;
    cfg.container = Container::Matroska;
    cfg.audio_codec = AudioCodec::AacMf;
    return cfg;
}

TEST(WebMContainerValidationTest, Validate_AcceptsWebMAv1Opus) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidWebMConfig();

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_AcceptsMatroskaAv1Aac) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidMatroskaConfig();

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_AcceptsMatroskaAv1Opus) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidMatroskaConfig();
    cfg.audio_codec = AudioCodec::Opus;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_RejectsWebMAv1Aac) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidWebMConfig();
    cfg.audio_codec = AudioCodec::AacMf;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_EQ(result.error_code, E_INVALIDARG);
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
    EXPECT_NE(result.error_detail.find("WebM"), std::string::npos);
}

TEST(WebMContainerValidationTest, Validate_DefaultConfigIsWebMOpus) {
    const RecorderConfig cfg{};
    EXPECT_EQ(cfg.container, Container::WebM);
    EXPECT_EQ(cfg.audio_codec, AudioCodec::Opus);
    EXPECT_EQ(cfg.video_codec, VideoCodec::Av1Nvenc);
}

TEST(WebMContainerValidationTest, Validate_AcceptsMatroskaH264Aac) {
    RecorderSession session;
    RecorderConfig cfg{};
    cfg.output_path = std::filesystem::current_path() / "test_mkv_h264_aac.mkv";
    cfg.target.kind = CaptureTarget::Kind::Monitor;
    cfg.target.native_id = 1;
    cfg.container = Container::Matroska;
    cfg.video_codec = VideoCodec::H264Nvenc;
    cfg.audio_codec = AudioCodec::AacMf;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_RejectsWebMH264) {
    RecorderSession session;
    RecorderConfig cfg{};
    cfg.output_path = std::filesystem::current_path() / "test_webm_h264.webm";
    cfg.target.kind = CaptureTarget::Kind::Monitor;
    cfg.target.native_id = 1;
    cfg.container = Container::WebM;
    cfg.video_codec = VideoCodec::H264Nvenc;
    cfg.audio_codec = AudioCodec::Opus;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
}

} // namespace
