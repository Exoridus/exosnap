#include <gtest/gtest.h>

#include <recorder_core/audio_track_model.h>
#include <recorder_core/packet_types.h>
#include <recorder_core/recorder_session.h>

#include <filesystem>

namespace {

using recorder_core::AudioCodec;
using recorder_core::AudioSourceKind;
using recorder_core::AudioTrackPlan;
using recorder_core::EncodedAudioPacket;
using recorder_core::RecorderConfig;
using recorder_core::RecorderResult;
using recorder_core::RecorderSession;
using recorder_core::ResolvedAudioTrack;

TEST(MultiTrackPacketTypesTest, EncodedAudioPacket_DefaultTrackIdIsZero) {
    const EncodedAudioPacket pkt{};
    EXPECT_EQ(pkt.track_id, 0u);
}

TEST(MultiTrackPacketTypesTest, EncodedAudioPacket_AllowsTrackIdOne) {
    EncodedAudioPacket pkt{};
    pkt.track_id = 1;
    EXPECT_EQ(pkt.track_id, 1u);
}

TEST(MultiTrackPacketTypesTest, EncodedAudioPacket_AllowsTrackIdTwo) {
    EncodedAudioPacket pkt{};
    pkt.track_id = 2;
    EXPECT_EQ(pkt.track_id, 2u);
}

TEST(MultiTrackPacketTypesTest, RecorderConfig_DefaultTrackPlanIsEmpty) {
    const RecorderConfig cfg{};
    EXPECT_TRUE(cfg.audio_track_plan.tracks.empty());
}

TEST(MultiTrackPacketTypesTest, RecorderConfig_StoresOneTrackPlan) {
    RecorderConfig cfg{};
    ResolvedAudioTrack t0{};
    t0.sources = {AudioSourceKind::App};
    t0.track_index = 0;
    cfg.audio_track_plan.tracks.push_back(t0);

    ASSERT_EQ(cfg.audio_track_plan.tracks.size(), 1u);
    EXPECT_EQ(cfg.audio_track_plan.tracks[0].track_index, 0u);
    ASSERT_EQ(cfg.audio_track_plan.tracks[0].sources.size(), 1u);
    EXPECT_EQ(cfg.audio_track_plan.tracks[0].sources[0], AudioSourceKind::App);
}

TEST(MultiTrackPacketTypesTest, RecorderConfig_StoresThreeTrackPlan) {
    RecorderConfig cfg{};
    ResolvedAudioTrack t0{};
    t0.sources = {AudioSourceKind::App};
    t0.track_index = 0;

    ResolvedAudioTrack t1{};
    t1.sources = {AudioSourceKind::Mic};
    t1.track_index = 1;

    ResolvedAudioTrack t2{};
    t2.sources = {AudioSourceKind::Sys};
    t2.track_index = 2;

    cfg.audio_track_plan.tracks = {t0, t1, t2};

    ASSERT_EQ(cfg.audio_track_plan.tracks.size(), 3u);
    EXPECT_EQ(cfg.audio_track_plan.tracks[0].track_index, 0u);
    EXPECT_EQ(cfg.audio_track_plan.tracks[1].track_index, 1u);
    EXPECT_EQ(cfg.audio_track_plan.tracks[2].track_index, 2u);
}

TEST(MultiTrackPacketTypesTest, Validate_RejectsAudioTrackPlanAboveMax) {
    RecorderSession session;
    RecorderConfig cfg{};

    cfg.output_path = std::filesystem::current_path() / "validate_tracks_limit.mkv";
    cfg.target.kind = recorder_core::CaptureTarget::Kind::Monitor;
    cfg.target.native_id = 1;

    for (uint32_t i = 0; i < 4; ++i) {
        ResolvedAudioTrack track{};
        track.track_index = i;
        cfg.audio_track_plan.tracks.push_back(track);
    }

    RecorderResult validation{};
    EXPECT_FALSE(session.Validate(cfg, &validation));
    EXPECT_EQ(validation.error_detail, "audio_track_plan: max 3 audio tracks supported");
}

TEST(MultiTrackPacketTypesTest, Validate_AcceptsOpusForMatroska) {
    RecorderSession session;
    RecorderConfig cfg{};

    cfg.output_path = std::filesystem::current_path() / "validate_opus_ok.mkv";
    cfg.target.kind = recorder_core::CaptureTarget::Kind::Monitor;
    cfg.target.native_id = 1;
    cfg.audio_codec = AudioCodec::Opus;

    RecorderResult validation{};
    EXPECT_TRUE(session.Validate(cfg, &validation));
    EXPECT_TRUE(validation.succeeded);
}

TEST(MultiTrackPacketTypesTest, Validate_RejectsUnknownAudioCodec) {
    RecorderSession session;
    RecorderConfig cfg{};

    cfg.output_path = std::filesystem::current_path() / "validate_unknown_audio_codec.mkv";
    cfg.target.kind = recorder_core::CaptureTarget::Kind::Monitor;
    cfg.target.native_id = 1;
    cfg.audio_codec = static_cast<AudioCodec>(999);

    RecorderResult validation{};
    EXPECT_FALSE(session.Validate(cfg, &validation));
    EXPECT_EQ(validation.error_detail, "Unsupported audio codec; supported: AudioCodec::AacMf, AudioCodec::Opus");
}

} // namespace
