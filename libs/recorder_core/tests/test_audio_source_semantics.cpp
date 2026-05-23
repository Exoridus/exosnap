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
    cfg.output_path = std::filesystem::current_path() / "validate_audio_source_semantics.mkv";
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

TEST(AudioSourceSemanticsTest, Validate_AcceptsSystemOutputTrackWithoutPid) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.record_audio = true;
    cfg.audio_track_plan.tracks.push_back(MakeTrack(0, {AudioSourceKind::SystemOutput}));

    RecorderResult validation{};
    EXPECT_TRUE(session.Validate(cfg, &validation));
    EXPECT_TRUE(validation.succeeded);
}

TEST(AudioSourceSemanticsTest, Validate_AcceptsSystemOutputAndMicPlan) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.record_audio = true;
    cfg.audio_track_plan.tracks.push_back(MakeTrack(0, {AudioSourceKind::SystemOutput}));
    cfg.audio_track_plan.tracks.push_back(MakeTrack(1, {AudioSourceKind::Mic}));

    RecorderResult validation{};
    EXPECT_TRUE(session.Validate(cfg, &validation));
    EXPECT_TRUE(validation.succeeded);
}

TEST(AudioSourceSemanticsTest, Validate_AcceptsRecordAudioFalse_EmptyPlan) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.record_audio = false;

    RecorderResult validation{};
    EXPECT_TRUE(session.Validate(cfg, &validation));
    EXPECT_TRUE(validation.succeeded);
}

TEST(AudioSourceSemanticsTest, Validate_AcceptsRecordAudioFalse_WithExplicitInvalidPlan) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.record_audio = false;
    cfg.audio_track_plan.tracks.push_back(MakeTrack(0, {AudioSourceKind::App}));
    cfg.audio_track_plan.tracks.push_back(MakeTrack(1, {AudioSourceKind::App}));
    cfg.audio_track_plan.tracks.push_back(MakeTrack(2, {AudioSourceKind::App}));
    cfg.audio_track_plan.tracks.push_back(MakeTrack(3, {AudioSourceKind::App}));

    RecorderResult validation{};
    EXPECT_TRUE(session.Validate(cfg, &validation));
    EXPECT_TRUE(validation.succeeded);
}

TEST(AudioSourceSemanticsTest, Validate_AcceptsEmptyPlan_RecordAudioTrue) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.record_audio = true;

    RecorderResult validation{};
    EXPECT_TRUE(session.Validate(cfg, &validation));
    EXPECT_TRUE(validation.succeeded);
}

TEST(AudioSourceSemanticsTest, Validate_RejectsAppSourceWithoutPid_RecordAudioTrue) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.record_audio = true;
    cfg.audio_track_plan.tracks.push_back(MakeTrack(0, {AudioSourceKind::App}));

    RecorderResult validation{};
    EXPECT_FALSE(session.Validate(cfg, &validation));
    EXPECT_EQ(validation.error_code, E_INVALIDARG);
    EXPECT_EQ(validation.error_phase, ErrorPhase::Prepare);
}

TEST(AudioSourceSemanticsTest, Validate_AcceptsSystemOutputMicMergedTrack) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.record_audio = true;
    cfg.audio_track_plan.tracks.push_back(MakeTrack(0, {AudioSourceKind::SystemOutput, AudioSourceKind::Mic}));

    RecorderResult validation{};
    EXPECT_TRUE(session.Validate(cfg, &validation));
    EXPECT_TRUE(validation.succeeded);
}

TEST(AudioSourceSemanticsTest, Validate_RejectsAppInMergedTrack_WithoutPid) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.record_audio = true;
    cfg.audio_track_plan.tracks.push_back(MakeTrack(0, {AudioSourceKind::SystemOutput, AudioSourceKind::App}));

    RecorderResult validation{};
    EXPECT_FALSE(session.Validate(cfg, &validation));
    EXPECT_EQ(validation.error_code, E_INVALIDARG);
    EXPECT_EQ(validation.error_phase, ErrorPhase::Prepare);
}

TEST(AudioSourceSemanticsTest, Validate_RejectsTooManyTracks_RecordAudioTrue) {
    RecorderSession session;
    RecorderConfig cfg = MakeValidBaseConfig();
    cfg.record_audio = true;
    cfg.audio_track_plan.tracks.push_back(MakeTrack(0, {AudioSourceKind::Mic}));
    cfg.audio_track_plan.tracks.push_back(MakeTrack(1, {AudioSourceKind::Mic}));
    cfg.audio_track_plan.tracks.push_back(MakeTrack(2, {AudioSourceKind::Mic}));
    cfg.audio_track_plan.tracks.push_back(MakeTrack(3, {AudioSourceKind::Mic}));

    RecorderResult validation{};
    EXPECT_FALSE(session.Validate(cfg, &validation));
    EXPECT_EQ(validation.error_code, E_INVALIDARG);
    EXPECT_EQ(validation.error_phase, ErrorPhase::Prepare);
}

} // namespace
