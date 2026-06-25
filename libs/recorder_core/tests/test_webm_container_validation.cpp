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

// --- PCM (0.6.0 Audio v2): Matroska-only A_PCM/INT_LIT ---

TEST(WebMContainerValidationTest, Validate_AcceptsMatroskaAv1Pcm) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidMatroskaConfig();
    cfg.audio_codec = AudioCodec::Pcm;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_AcceptsMatroskaH264Pcm) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidMatroskaConfig();
    cfg.video_codec = VideoCodec::H264Nvenc;
    cfg.audio_codec = AudioCodec::Pcm;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_RejectsWebMPcm) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidWebMConfig();
    cfg.audio_codec = AudioCodec::Pcm;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_EQ(result.error_code, E_INVALIDARG);
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
    EXPECT_NE(result.error_detail.find("Pcm"), std::string::npos);
}

// ADR 0030 (narrowed): MP4 + H.264 + PCM is back to Experimental — Validate rejects it.
// libavformat emits the ipcm sample entry (ISO/IEC 23003-5) which has limited player
// support; deferred until a broadly-compatible sample-entry mapping is validated.
// Use MKV for PCM. FLAC and Opus also remain rejected for MP4.
TEST(WebMContainerValidationTest, Validate_RejectsMp4Pcm) {
    RecorderSession session;
    RecorderConfig cfg{};
    cfg.output_path = std::filesystem::current_path() / "test_mp4_pcm.mp4";
    cfg.target.kind = CaptureTarget::Kind::Monitor;
    cfg.target.native_id = 1;
    cfg.container = Container::Mp4;
    cfg.video_codec = VideoCodec::H264Nvenc;
    cfg.audio_codec = AudioCodec::Pcm;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_EQ(result.error_code, E_INVALIDARG);
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
}

// --- FLAC (0.6.0 Audio v2): Matroska-only A_FLAC ---

TEST(WebMContainerValidationTest, Validate_AcceptsMatroskaAv1Flac) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidMatroskaConfig();
    cfg.audio_codec = AudioCodec::Flac;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_AcceptsMatroskaH264Flac) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidMatroskaConfig();
    cfg.video_codec = VideoCodec::H264Nvenc;
    cfg.audio_codec = AudioCodec::Flac;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_RejectsWebMFlac) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidWebMConfig();
    cfg.audio_codec = AudioCodec::Flac;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_EQ(result.error_code, E_INVALIDARG);
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
    EXPECT_NE(result.error_detail.find("Flac"), std::string::npos);
}

TEST(WebMContainerValidationTest, Validate_RejectsMp4Flac) {
    RecorderSession session;
    RecorderConfig cfg{};
    cfg.output_path = std::filesystem::current_path() / "test_mp4_flac.mp4";
    cfg.target.kind = CaptureTarget::Kind::Monitor;
    cfg.target.native_id = 1;
    cfg.container = Container::Mp4;
    cfg.video_codec = VideoCodec::H264Nvenc;
    cfg.audio_codec = AudioCodec::Flac;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_EQ(result.error_code, E_INVALIDARG);
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
}

// --- HEVC (0.7.0): Matroska V_MPEGH/ISO/HEVC + MP4 hvc1 ---
// The container compat registry lists MKV + HEVC + {Opus, AAC, PCM, FLAC} as
// Allowed; Validate must accept these end-to-end. MP4 + HEVC + AAC is also Allowed
// (transient MKV remuxed to MP4 with the 'hvc1' FourCC); MP4 audio is AAC-only, so
// MP4 + HEVC + {Opus, PCM, FLAC} must be rejected, and WebM + HEVC stays rejected.

TEST(WebMContainerValidationTest, Validate_AcceptsMatroskaHevcOpus) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidMatroskaConfig();
    cfg.video_codec = VideoCodec::HevcNvenc;
    cfg.audio_codec = AudioCodec::Opus;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_AcceptsMatroskaHevcAac) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidMatroskaConfig();
    cfg.video_codec = VideoCodec::HevcNvenc;
    cfg.audio_codec = AudioCodec::AacMf;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_AcceptsMatroskaHevcPcm) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidMatroskaConfig();
    cfg.video_codec = VideoCodec::HevcNvenc;
    cfg.audio_codec = AudioCodec::Pcm;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_AcceptsMatroskaHevcFlac) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidMatroskaConfig();
    cfg.video_codec = VideoCodec::HevcNvenc;
    cfg.audio_codec = AudioCodec::Flac;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

// Helper: a minimal MP4 + HEVC config (audio codec set by the caller).
static RecorderConfig MakeMp4HevcConfig(AudioCodec audio) {
    RecorderConfig cfg{};
    cfg.output_path = std::filesystem::current_path() / "test_mp4_hevc.mp4";
    cfg.target.kind = CaptureTarget::Kind::Monitor;
    cfg.target.native_id = 1;
    cfg.container = Container::Mp4;
    cfg.video_codec = VideoCodec::HevcNvenc;
    cfg.audio_codec = audio;
    return cfg;
}

TEST(WebMContainerValidationTest, Validate_AcceptsMp4HevcAac) {
    // 0.7.0 hvc1-in-MP4: HEVC recorded to a transient MKV, remuxed to MP4 with the
    // 'hvc1' sample-entry FourCC. AAC is the only MP4 audio codec, so this is the
    // sole valid MP4 + HEVC combination.
    RecorderSession session;
    RecorderConfig cfg = MakeMp4HevcConfig(AudioCodec::AacMf);

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_RejectsMp4HevcOpus) {
    // Opus-in-MP4 is Prohibited regardless of the video codec.
    RecorderSession session;
    RecorderConfig cfg = MakeMp4HevcConfig(AudioCodec::Opus);

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
}

TEST(WebMContainerValidationTest, Validate_RejectsMp4HevcPcm) {
    // MP4 audio is AAC-only; PCM (ipcm) stays Experimental.
    RecorderSession session;
    RecorderConfig cfg = MakeMp4HevcConfig(AudioCodec::Pcm);

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
}

TEST(WebMContainerValidationTest, Validate_RejectsMp4HevcFlac) {
    // MP4 audio is AAC-only; FLAC-in-MP4 stays Experimental.
    RecorderSession session;
    RecorderConfig cfg = MakeMp4HevcConfig(AudioCodec::Flac);

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
}

TEST(WebMContainerValidationTest, Validate_RejectsWebMHevc) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidWebMConfig();
    cfg.video_codec = VideoCodec::HevcNvenc;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
}

// --- 10-bit (0.7.0 S5): HEVC Main10 / AV1 10-bit via P010, SDR BT.709 ---
// 10-bit is valid for the same containers as the 8-bit codec (HEVC: MKV/MP4;
// AV1: MKV/WebM). H.264 is 8-bit only and must be rejected with E_NOTIMPL.

using recorder_core::BitDepth;

TEST(WebMContainerValidationTest, Validate_AcceptsMatroskaHevc10Bit) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidMatroskaConfig();
    cfg.video_codec = VideoCodec::HevcNvenc;
    cfg.audio_codec = AudioCodec::AacMf;
    cfg.bit_depth = BitDepth::Bit10;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_AcceptsMp4Hevc10Bit) {
    RecorderSession session;
    RecorderConfig cfg = MakeMp4HevcConfig(AudioCodec::AacMf);
    cfg.bit_depth = BitDepth::Bit10;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_AcceptsWebMAv110Bit) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidWebMConfig(); // WebM + AV1 + Opus
    cfg.bit_depth = BitDepth::Bit10;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_AcceptsMatroskaAv110Bit) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidMatroskaConfig(); // MKV + AV1 + AAC
    cfg.bit_depth = BitDepth::Bit10;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(WebMContainerValidationTest, Validate_RejectsMkvH26410Bit) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidMatroskaConfig();
    cfg.video_codec = VideoCodec::H264Nvenc;
    cfg.audio_codec = AudioCodec::AacMf;
    cfg.bit_depth = BitDepth::Bit10;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_EQ(result.error_code, E_NOTIMPL);
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
    EXPECT_NE(result.error_detail.find("Bit10"), std::string::npos);
}

TEST(WebMContainerValidationTest, Validate_RejectsMp4H26410Bit) {
    RecorderSession session;
    RecorderConfig cfg{};
    cfg.output_path = std::filesystem::current_path() / "test_mp4_h264_10bit.mp4";
    cfg.target.kind = CaptureTarget::Kind::Monitor;
    cfg.target.native_id = 1;
    cfg.container = Container::Mp4;
    cfg.video_codec = VideoCodec::H264Nvenc;
    cfg.audio_codec = AudioCodec::AacMf;
    cfg.bit_depth = BitDepth::Bit10;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_EQ(result.error_code, E_NOTIMPL);
    EXPECT_EQ(result.error_phase, ErrorPhase::Prepare);
}

} // namespace
