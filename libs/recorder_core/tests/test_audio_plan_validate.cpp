#include <gtest/gtest.h>

#include <recorder_core/audio_track_model.h>
#include <recorder_core/recorder_session.h>

#include <windows.h>

#include <filesystem>
#include <initializer_list>

namespace {

using recorder_core::AudioSourceKind;
using recorder_core::CaptureTarget;
using recorder_core::ErrorPhase;
using recorder_core::RecorderConfig;
using recorder_core::RecorderResult;
using recorder_core::RecorderSession;
using recorder_core::ResolvedAudioTrack;

RecorderConfig MakeValidBaseConfig() {
    RecorderConfig cfg{};
    cfg.output_path = std::filesystem::current_path() / "validate_audio_plan_phase5a.mkv";
    cfg.target.kind = CaptureTarget::Kind::Monitor;
    cfg.target.native_id = 1;
    return cfg;
}

ResolvedAudioTrack MakeTrack(uint32_t track_index, std::initializer_list<AudioSourceKind> sources) {
    ResolvedAudioTrack track{};
    track.track_index = track_index;
    track.sources.assign(sources.begin(), sources.end());
    return track;
}

TEST(AudioPlanValidateTest, Validate_AcceptsMergedAudioTrack_TwoSources) {
    // Merged tracks (1-3 sources) are now valid.
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.audio_track_plan.tracks.push_back(MakeTrack(0, {AudioSourceKind::App, AudioSourceKind::Mic}));
    cfg.audio_target_process_id = 1234u;

    RecorderResult validation{};
    EXPECT_TRUE(session.Validate(cfg, &validation));
    EXPECT_TRUE(validation.succeeded);
}

TEST(AudioPlanValidateTest, Validate_RejectsTrackWithFourSources) {
    // Tracks with more than 3 sources remain invalid.
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.audio_track_plan.tracks.push_back(MakeTrack(
        0, {AudioSourceKind::App, AudioSourceKind::Mic, AudioSourceKind::Sys, AudioSourceKind::SystemOutput}));
    cfg.audio_target_process_id = 1234u;

    RecorderResult validation{};
    EXPECT_FALSE(session.Validate(cfg, &validation));
    EXPECT_EQ(validation.error_code, E_NOTIMPL);
    EXPECT_EQ(validation.error_phase, ErrorPhase::Prepare);
}

TEST(AudioPlanValidateTest, Validate_RejectsAppSourceWithoutPid) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.audio_track_plan.tracks.push_back(MakeTrack(0, {AudioSourceKind::App}));

    RecorderResult validation{};
    EXPECT_FALSE(session.Validate(cfg, &validation));
    EXPECT_EQ(validation.error_code, E_INVALIDARG);
    EXPECT_EQ(validation.error_phase, ErrorPhase::Prepare);
}

TEST(AudioPlanValidateTest, Validate_RejectsSysSourceWithoutPid) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.audio_track_plan.tracks.push_back(MakeTrack(0, {AudioSourceKind::Sys}));

    RecorderResult validation{};
    EXPECT_FALSE(session.Validate(cfg, &validation));
    EXPECT_EQ(validation.error_code, E_INVALIDARG);
    EXPECT_EQ(validation.error_phase, ErrorPhase::Prepare);
}

TEST(AudioPlanValidateTest, Validate_AcceptsAppSourceWithPid) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.audio_track_plan.tracks.push_back(MakeTrack(0, {AudioSourceKind::App}));
    cfg.audio_target_process_id = 1234u;

    RecorderResult validation{};
    EXPECT_TRUE(session.Validate(cfg, &validation));
    EXPECT_TRUE(validation.succeeded);
}

TEST(AudioPlanValidateTest, Validate_AcceptsSysSourceWithPid) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.audio_track_plan.tracks.push_back(MakeTrack(0, {AudioSourceKind::Sys}));
    cfg.audio_target_process_id = 1234u;

    RecorderResult validation{};
    EXPECT_TRUE(session.Validate(cfg, &validation));
    EXPECT_TRUE(validation.succeeded);
}

TEST(AudioPlanValidateTest, Validate_AcceptsAppSysMicPlanWithPid) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.audio_track_plan.tracks.push_back(MakeTrack(0, {AudioSourceKind::App}));
    cfg.audio_track_plan.tracks.push_back(MakeTrack(1, {AudioSourceKind::Sys}));
    cfg.audio_track_plan.tracks.push_back(MakeTrack(2, {AudioSourceKind::Mic}));
    cfg.audio_target_process_id = 1234u;

    RecorderResult validation{};
    EXPECT_TRUE(session.Validate(cfg, &validation));
    EXPECT_TRUE(validation.succeeded);
}

TEST(AudioPlanValidateTest, Validate_AcceptsEmptyPlanWithoutPid) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();

    RecorderResult validation{};
    EXPECT_TRUE(session.Validate(cfg, &validation));
    EXPECT_TRUE(validation.succeeded);
}

TEST(AudioPlanValidateTest, Validate_AcceptsEmptyPlanWithPid) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.audio_target_process_id = 1234u;

    RecorderResult validation{};
    EXPECT_TRUE(session.Validate(cfg, &validation));
    EXPECT_TRUE(validation.succeeded);
}

// ---------------------------------------------------------------------------
// audio_pcm_float: Pcm-codec-only, requires audio_bit_depth == 32.
// ---------------------------------------------------------------------------

TEST(AudioPlanValidateTest, Validate_AcceptsPcmFloatWithBitDepth32) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.container = recorder_core::Container::Matroska; // Pcm is MKV-only
    cfg.audio_codec = recorder_core::AudioCodec::Pcm;
    cfg.audio_bit_depth = 32;
    cfg.audio_pcm_float = true;

    RecorderResult validation{};
    EXPECT_TRUE(session.Validate(cfg, &validation));
    EXPECT_TRUE(validation.succeeded);
}

TEST(AudioPlanValidateTest, Validate_RejectsPcmFloatWithNonPcmCodec_Aac) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.container = recorder_core::Container::Matroska; // valid for Aac, isolates the pcm_float check
    cfg.audio_codec = recorder_core::AudioCodec::Aac;
    cfg.audio_bit_depth = 32;
    cfg.audio_pcm_float = true;

    RecorderResult validation{};
    EXPECT_FALSE(session.Validate(cfg, &validation));
    EXPECT_EQ(validation.error_code, E_INVALIDARG);
    EXPECT_EQ(validation.error_phase, ErrorPhase::Prepare);
}

TEST(AudioPlanValidateTest, Validate_RejectsPcmFloatWithNonPcmCodec_Opus) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.audio_codec = recorder_core::AudioCodec::Opus; // Opus is valid on the default WebM container
    cfg.audio_sample_rate = 48000;                     // Opus requires 48000
    cfg.audio_bit_depth = 32;
    cfg.audio_pcm_float = true;

    RecorderResult validation{};
    EXPECT_FALSE(session.Validate(cfg, &validation));
    EXPECT_EQ(validation.error_code, E_INVALIDARG);
    EXPECT_EQ(validation.error_phase, ErrorPhase::Prepare);
}

TEST(AudioPlanValidateTest, Validate_RejectsPcmFloatWithNonPcmCodec_Flac) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.container = recorder_core::Container::Matroska; // valid for Flac, isolates the pcm_float check
    cfg.audio_codec = recorder_core::AudioCodec::Flac;
    cfg.audio_bit_depth = 24;
    cfg.audio_pcm_float = true;

    RecorderResult validation{};
    EXPECT_FALSE(session.Validate(cfg, &validation));
    EXPECT_EQ(validation.error_code, E_INVALIDARG);
    EXPECT_EQ(validation.error_phase, ErrorPhase::Prepare);
}

TEST(AudioPlanValidateTest, Validate_RejectsPcmFloatWithBitDepth16) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.container = recorder_core::Container::Matroska; // Pcm is MKV-only
    cfg.audio_codec = recorder_core::AudioCodec::Pcm;
    cfg.audio_bit_depth = 16;
    cfg.audio_pcm_float = true;

    RecorderResult validation{};
    EXPECT_FALSE(session.Validate(cfg, &validation));
    EXPECT_EQ(validation.error_code, E_INVALIDARG);
    EXPECT_EQ(validation.error_phase, ErrorPhase::Prepare);
}

} // namespace
