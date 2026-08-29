// test_mp4_remux_pipeline_flow.cpp — integration-logic tests for the MP4
// remux-on-stop pipeline (ADR-0014).
//
// These tests focus on the pure-logic layer that does NOT require a live
// encoder or display capture device:
//
//   1. Valuable live-artifact derivation
//   2. Path derivation edge cases
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

#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/recorder_session.h>

#include <filesystem>
#include <string>

namespace {

using exosnap::engine::AudioCodec;
using exosnap::engine::CaptureTarget;
using exosnap::engine::Container;
using exosnap::engine::DeriveValuablePartialPath;
using exosnap::engine::RecorderConfig;
using exosnap::engine::RecorderResult;
using exosnap::engine::RecorderSession;
using exosnap::engine::VideoCodec;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static RecorderConfig MakeMp4Config() {
    RecorderConfig cfg{};
    cfg.output_path = std::filesystem::temp_directory_path() / "exosnap_test.mp4";
    cfg.container = Container::Mp4;
    cfg.video_codec = VideoCodec::H264;
    cfg.audio_codec = AudioCodec::Aac;
    cfg.record_audio = false;
    cfg.target.kind = CaptureTarget::Kind::Window;
    cfg.target.native_id = 1;
    return cfg;
}

// ---------------------------------------------------------------------------
// Valuable live-artifact path tests
// ---------------------------------------------------------------------------

TEST(Mp4RemuxPipelineFlowTest, DeriveValuablePartialPath_AppendsToCompleteFinalName) {
    const std::filesystem::path input = L"C:\\Videos\\recording.mp4";
    const std::filesystem::path result = DeriveValuablePartialPath(input);

    EXPECT_EQ(result.extension().wstring(), L".partial");
    EXPECT_NE(result, input);
    EXPECT_EQ(result.filename().wstring(), L"recording.mp4.partial");
    EXPECT_EQ(result.parent_path().wstring(), input.parent_path().wstring());
}

TEST(Mp4RemuxPipelineFlowTest, DeriveValuablePartialPath_PreservesDirectory) {
    const std::filesystem::path input = L"D:\\Captures\\session 2025\\clip.mp4";
    const std::filesystem::path result = DeriveValuablePartialPath(input);

    EXPECT_EQ(result.parent_path().wstring(), input.parent_path().wstring());
    EXPECT_EQ(result.filename().wstring(), L"clip.mp4.partial");
}

TEST(Mp4RemuxPipelineFlowTest, DeriveValuablePartialPath_NoExtension) {
    const std::filesystem::path input = L"C:\\Videos\\noext";
    const std::filesystem::path result = DeriveValuablePartialPath(input);

    EXPECT_EQ(result.filename().wstring(), L"noext.partial");
}

TEST(Mp4RemuxPipelineFlowTest, DeriveValuablePartialPath_RelativePath) {
    // Works for relative paths as well (used in tests / portable scenarios).
    const std::filesystem::path input = L"recording.mp4";
    const std::filesystem::path result = DeriveValuablePartialPath(input);

    EXPECT_EQ(result.wstring(), L"recording.mp4.partial");
}

TEST(Mp4RemuxPipelineFlowTest, DeriveValuablePartialPath_DistinguishesFinalNames) {
    // Two different MP4 output paths must not share the same transient path.
    const std::filesystem::path a = L"C:\\Videos\\clip_a.mp4";
    const std::filesystem::path b = L"C:\\Videos\\clip_b.mp4";

    EXPECT_NE(DeriveValuablePartialPath(a), DeriveValuablePartialPath(b));
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
    const auto cfg = MakeMp4Config(); // H264 + Aac, audio off

    RecorderResult result{};
    EXPECT_TRUE(session.Validate(cfg, &result));
    EXPECT_TRUE(result.succeeded);
}

TEST(Mp4RemuxPipelineFlowTest, Mp4RejectsAv1) {
    RecorderSession session;
    auto cfg = MakeMp4Config();
    cfg.video_codec = VideoCodec::Av1;

    RecorderResult result{};
    EXPECT_FALSE(session.Validate(cfg, &result));
    EXPECT_FALSE(result.succeeded);
}

TEST(Mp4RemuxPipelineFlowTest, WebMWithH264RemainsRejected) {
    RecorderSession session;
    auto cfg = MakeMp4Config();
    cfg.container = Container::WebM;
    cfg.video_codec = VideoCodec::H264;

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
