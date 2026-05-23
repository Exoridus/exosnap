#include <gtest/gtest.h>

#include <recorder_core/codec_types.h>
#include <recorder_core/recorder_session.h>

#include <filesystem>

namespace {

using recorder_core::AudioCodec;
using recorder_core::CaptureTarget;
using recorder_core::Container;
using recorder_core::RecorderConfig;
using recorder_core::RecorderResult;
using recorder_core::RecorderSession;
using recorder_core::VideoCodec;

static RecorderConfig MakeMp4Config() {
    RecorderConfig cfg{};
    auto tmp = std::filesystem::temp_directory_path() / "exosnap_test.mp4";
    cfg.output_path = tmp;
    cfg.container = Container::Mp4;
    cfg.video_codec = VideoCodec::H264Nvenc;
    cfg.audio_codec = AudioCodec::AacMf;
    cfg.record_audio = false;
    cfg.target.kind = CaptureTarget::Kind::Window;
    cfg.target.native_id = 1;
    return cfg;
}

TEST(Mp4ValidationTest, AcceptsMp4H264Aac) {
    RecorderSession session;
    RecorderResult result{};
    EXPECT_TRUE(session.Validate(MakeMp4Config(), &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(Mp4ValidationTest, RejectsMp4WithOpus) {
    RecorderSession session;
    auto cfg = MakeMp4Config();
    cfg.audio_codec = AudioCodec::Opus;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.error_detail.empty());
}

TEST(Mp4ValidationTest, RejectsMp4WithAv1) {
    RecorderSession session;
    auto cfg = MakeMp4Config();
    cfg.video_codec = VideoCodec::Av1Nvenc;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_FALSE(result.succeeded);
}

TEST(Mp4ValidationTest, RejectsWebMWithH264) {
    RecorderSession session;
    auto cfg = MakeMp4Config();
    cfg.container = Container::WebM;
    cfg.video_codec = VideoCodec::H264Nvenc;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_FALSE(result.succeeded);
}

TEST(Mp4ValidationTest, AcceptsMatroskaWithH264) {
    RecorderSession session;
    auto cfg = MakeMp4Config();
    cfg.container = Container::Matroska;
    cfg.video_codec = VideoCodec::H264Nvenc;
    cfg.audio_codec = AudioCodec::AacMf;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(Mp4ValidationTest, DefaultConfigIsWebMNotMp4) {
    RecorderConfig cfg{};
    EXPECT_EQ(cfg.container, Container::WebM);
    EXPECT_EQ(cfg.video_codec, VideoCodec::Av1Nvenc);
    EXPECT_EQ(cfg.audio_codec, AudioCodec::Opus);
}

} // namespace
