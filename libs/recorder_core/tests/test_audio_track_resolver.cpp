#include <gtest/gtest.h>

#include <recorder_core/audio_track_model.h>

#include <cstddef>
#include <initializer_list>
#include <vector>

namespace {

using recorder_core::AudioSourceKind;
using recorder_core::AudioSourceRow;
using recorder_core::AudioTrackPlan;
using recorder_core::ResolveAudioTracks;

void ExpectTrack(const AudioTrackPlan& plan, const std::size_t track_position, const uint32_t expected_track_index,
                 const std::initializer_list<AudioSourceKind> expected_sources) {
    ASSERT_LT(track_position, plan.tracks.size());
    const auto& track = plan.tracks[track_position];

    EXPECT_EQ(track.track_index, expected_track_index);
    const std::vector<AudioSourceKind> expected(expected_sources);
    EXPECT_EQ(track.sources, expected);
}

TEST(AudioTrackResolverTest, DefaultConfig_AllEnabled_NoMerge) {
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, true, false},
        {AudioSourceKind::Mic, true, false},
        {AudioSourceKind::Sys, true, false},
    };

    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 3u);
    ExpectTrack(plan, 0, 0u, {AudioSourceKind::App});
    ExpectTrack(plan, 1, 1u, {AudioSourceKind::Mic});
    ExpectTrack(plan, 2, 2u, {AudioSourceKind::Sys});
}

TEST(AudioTrackResolverTest, MergeMicIntoApp) {
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, true, false},
        {AudioSourceKind::Mic, true, true},
        {AudioSourceKind::Sys, true, false},
    };

    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 2u);
    ExpectTrack(plan, 0, 0u, {AudioSourceKind::App, AudioSourceKind::Mic});
    ExpectTrack(plan, 1, 1u, {AudioSourceKind::Sys});
}

TEST(AudioTrackResolverTest, MergeAll) {
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, true, false},
        {AudioSourceKind::Mic, true, true},
        {AudioSourceKind::Sys, true, true},
    };

    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 1u);
    ExpectTrack(plan, 0, 0u, {AudioSourceKind::App, AudioSourceKind::Mic, AudioSourceKind::Sys});
}

TEST(AudioTrackResolverTest, AppDisabled_MicTopmost_SysMerged) {
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, false, false},
        {AudioSourceKind::Mic, true, false},
        {AudioSourceKind::Sys, true, true},
    };

    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 1u);
    ExpectTrack(plan, 0, 0u, {AudioSourceKind::Mic, AudioSourceKind::Sys});
}

TEST(AudioTrackResolverTest, AllDisabled) {
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, false, false},
        {AudioSourceKind::Mic, false, false},
        {AudioSourceKind::Sys, false, false},
    };

    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    EXPECT_TRUE(plan.tracks.empty());
}

TEST(AudioTrackResolverTest, OnlyMicEnabled) {
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, false, false},
        {AudioSourceKind::Mic, true, false},
        {AudioSourceKind::Sys, false, false},
    };

    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 1u);
    ExpectTrack(plan, 0, 0u, {AudioSourceKind::Mic});
}

TEST(AudioTrackResolverTest, TopmostEnabledMergeIsIgnored) {
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, false, false},
        {AudioSourceKind::Mic, true, true},
        {AudioSourceKind::Sys, true, false},
    };

    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 2u);
    ExpectTrack(plan, 0, 0u, {AudioSourceKind::Mic});
    ExpectTrack(plan, 1, 1u, {AudioSourceKind::Sys});
}

TEST(AudioTrackResolverTest, SysOnly) {
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, false, false},
        {AudioSourceKind::Mic, false, false},
        {AudioSourceKind::Sys, true, false},
    };

    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 1u);
    ExpectTrack(plan, 0, 0u, {AudioSourceKind::Sys});
}

TEST(AudioTrackResolverTest, TrackIndicesAreSequential) {
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, true, false}, {AudioSourceKind::Mic, true, true}, {AudioSourceKind::Sys, true, false},
        {AudioSourceKind::App, true, false}, {AudioSourceKind::Mic, true, true},
    };

    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 3u);
    ExpectTrack(plan, 0, 0u, {AudioSourceKind::App, AudioSourceKind::Mic});
    ExpectTrack(plan, 1, 1u, {AudioSourceKind::Sys});
    ExpectTrack(plan, 2, 2u, {AudioSourceKind::App, AudioSourceKind::Mic});
}

} // namespace
