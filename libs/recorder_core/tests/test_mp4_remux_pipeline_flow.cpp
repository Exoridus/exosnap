// test_mp4_remux_pipeline_flow.cpp — integration-logic tests for the MP4
// remux-on-stop pipeline (ADR-0014).
//
// These tests focus on the pure-logic layer that does NOT require a live
// encoder or display capture device:
//
//   1. DeriveTransientMkvPath — extension substitution (mp4 → mkv.tmp)
//   2. DeriveTransientMkvPath edge cases — nested extensions, no extension
//   3. OpusMp4GatingAfterMfRemoval — Opus+MP4 is still rejected (codec-gate)
//   4. Mp4ValidationAcceptsAacH264 — Mp4+H264+AAC is accepted
//   5. Mp4ValidationRejectsAv1     — Mp4+AV1 is rejected
//   6. Mp4ValidationRejectsWebMH264 — WebM+H264 remains rejected
//
// Note: RunRemuxJob() and the Saving-state transitions are exercised by the
// higher-level integration in test_mp4_remuxer.cpp (success/cancel/fail paths).
// The tests below only cover the pure helpers and gating rules that live in
// recorder_session / container validation logic.

#include <gtest/gtest.h>

#include <recorder_core/codec_types.h>
#include <recorder_core/recorder_session.h>

#include <filesystem>
#include <string>

namespace {

using recorder_core::AudioCodec;
using recorder_core::CaptureTarget;
using recorder_core::Container;
using recorder_core::DeriveTransientMkvPath;
using recorder_core::RecorderConfig;
using recorder_core::RecorderResult;
using recorder_core::RecorderSession;
using recorder_core::VideoCodec;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static RecorderConfig MakeMp4Config() {
    RecorderConfig cfg{};
    cfg.output_path = std::filesystem::temp_directory_path() / "exosnap_test.mp4";
    cfg.container = Container::Mp4;
    cfg.video_codec = VideoCodec::H264Nvenc;
    cfg.audio_codec = AudioCodec::AacMf;
    cfg.record_audio = false;
    cfg.target.kind = CaptureTarget::Kind::Window;
    cfg.target.native_id = 1;
    return cfg;
}

// ---------------------------------------------------------------------------
// DeriveTransientMkvPath tests
// ---------------------------------------------------------------------------

TEST(Mp4RemuxPipelineFlowTest, DeriveTransientMkvPath_BasicSubstitution) {
    // Standard case: "recording.mp4" → "recording.mkv.tmp"
    const std::filesystem::path input = L"C:\\Videos\\recording.mp4";
    const std::filesystem::path result = DeriveTransientMkvPath(input);

    EXPECT_EQ(result.extension().wstring(), L".tmp");
    EXPECT_NE(result, input);

    // Stem of result should be "recording.mkv" so that the full filename is
    // "recording.mkv.tmp".
    EXPECT_EQ(result.filename().wstring(), L"recording.mkv.tmp");
    EXPECT_EQ(result.parent_path().wstring(), input.parent_path().wstring());
}

TEST(Mp4RemuxPipelineFlowTest, DeriveTransientMkvPath_PreservesDirectory) {
    const std::filesystem::path input = L"D:\\Captures\\session 2025\\clip.mp4";
    const std::filesystem::path result = DeriveTransientMkvPath(input);

    EXPECT_EQ(result.parent_path().wstring(), input.parent_path().wstring());
    EXPECT_EQ(result.filename().wstring(), L"clip.mkv.tmp");
}

TEST(Mp4RemuxPipelineFlowTest, DeriveTransientMkvPath_NoExtension) {
    // Path without extension: still appends ".mkv.tmp" by replacing empty ext.
    const std::filesystem::path input = L"C:\\Videos\\noext";
    const std::filesystem::path result = DeriveTransientMkvPath(input);

    // replace_extension(".mkv.tmp") on a path with no extension appends the ext.
    EXPECT_EQ(result.filename().wstring(), L"noext.mkv.tmp");
}

TEST(Mp4RemuxPipelineFlowTest, DeriveTransientMkvPath_RelativePath) {
    // Works for relative paths as well (used in tests / portable scenarios).
    const std::filesystem::path input = L"recording.mp4";
    const std::filesystem::path result = DeriveTransientMkvPath(input);

    EXPECT_EQ(result.wstring(), L"recording.mkv.tmp");
}

TEST(Mp4RemuxPipelineFlowTest, DeriveTransientMkvPath_IsIdempotentOnStem) {
    // Two different MP4 output paths must not share the same transient path.
    const std::filesystem::path a = L"C:\\Videos\\clip_a.mp4";
    const std::filesystem::path b = L"C:\\Videos\\clip_b.mp4";

    EXPECT_NE(DeriveTransientMkvPath(a), DeriveTransientMkvPath(b));
}

// ---------------------------------------------------------------------------
// Container / codec gating tests (regression: Opus+MP4 must remain blocked
// even after the Media Foundation SinkWriter path was removed)
// ---------------------------------------------------------------------------

TEST(Mp4RemuxPipelineFlowTest, OpusMp4GatingAfterMfRemoval) {
    // ADR-0014 / gating rule: Opus is never valid with Container::Mp4.
    // This test guards against the MF path removal accidentally relaxing the
    // codec-container gate in Validate().
    RecorderSession session;
    auto cfg = MakeMp4Config();
    cfg.audio_codec = AudioCodec::Opus;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_FALSE(result.succeeded);
    EXPECT_FALSE(result.error_detail.empty()) << "Validate() must populate error_detail for blocked Opus+MP4";
}

TEST(Mp4RemuxPipelineFlowTest, Mp4ValidatesWithAacH264) {
    RecorderSession session;
    const auto cfg = MakeMp4Config(); // H264 + AacMf, audio off

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(Mp4RemuxPipelineFlowTest, Mp4RejectsAv1) {
    RecorderSession session;
    auto cfg = MakeMp4Config();
    cfg.video_codec = VideoCodec::Av1Nvenc;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_FALSE(result.succeeded);
}

TEST(Mp4RemuxPipelineFlowTest, WebMWithH264RemainsRejected) {
    RecorderSession session;
    auto cfg = MakeMp4Config();
    cfg.container = Container::WebM;
    cfg.video_codec = VideoCodec::H264Nvenc;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_FALSE(result.succeeded);
}

TEST(Mp4RemuxPipelineFlowTest, MatroskaWithH264AacIsValid) {
    // Sanity: Matroska is unaffected by ADR-0014 changes.
    RecorderSession session;
    auto cfg = MakeMp4Config();
    cfg.container = Container::Matroska;

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

} // namespace
