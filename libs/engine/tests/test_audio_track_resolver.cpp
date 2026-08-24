#include <gtest/gtest.h>

#include <exosnap/engine/audio_track_model.h>

#include <cstddef>
#include <initializer_list>
#include <vector>

namespace {

using exosnap::engine::AudioSourceKind;
using exosnap::engine::AudioSourceRow;
using exosnap::engine::AudioTrackPlan;
using exosnap::engine::ResolveAudioTracks;

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

// ---------------------------------------------------------------------------
// NormalizeSourceRowsForTarget
//
// App and Sys both address a window's process — App captures its tree, Sys
// captures everything outside it. A display target has no such process, and a row
// that survives into the plan there blocks the recording with "Window target PID
// unavailable".
// ---------------------------------------------------------------------------

using exosnap::engine::NormalizeSourceRowsForTarget;

std::vector<AudioSourceKind> KindsOf(const std::vector<AudioSourceRow>& rows) {
    std::vector<AudioSourceKind> kinds;
    kinds.reserve(rows.size());
    for (const AudioSourceRow& row : rows) {
        kinds.push_back(row.kind);
    }
    return kinds;
}

TEST(NormalizeSourceRowsForTargetTest, WindowTargetKeepsEveryRowUntouched) {
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, true, false},
        {AudioSourceKind::Sys, true, false},
        {AudioSourceKind::Mic, true, false},
    };

    EXPECT_EQ(KindsOf(NormalizeSourceRowsForTarget(rows, /*window_target=*/true)), KindsOf(rows));
}

TEST(NormalizeSourceRowsForTargetTest, DisplayTargetDropsAppAndKeepsMic) {
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, true, false},
        {AudioSourceKind::Mic, true, false},
    };

    const std::vector<AudioSourceKind> kinds = KindsOf(NormalizeSourceRowsForTarget(rows, /*window_target=*/false));
    EXPECT_EQ(kinds, (std::vector<AudioSourceKind>{AudioSourceKind::Mic}));
}

TEST(NormalizeSourceRowsForTargetTest, DisplayTargetRewritesSysToSystemOutput) {
    // The user asked for system audio and must still get it: with no app to
    // exclude, "everything but the app" is the full system output.
    const std::vector<AudioSourceRow> rows = {{AudioSourceKind::Sys, true, false}};

    const std::vector<AudioSourceKind> kinds = KindsOf(NormalizeSourceRowsForTarget(rows, /*window_target=*/false));
    EXPECT_EQ(kinds, (std::vector<AudioSourceKind>{AudioSourceKind::SystemOutput}));
}

TEST(NormalizeSourceRowsForTargetTest, DisplayTargetPreservesRowSettings) {
    std::vector<AudioSourceRow> rows = {{AudioSourceKind::Sys, true, false}};
    rows[0].gain_db = -6.0f;
    rows[0].muted = true;

    const std::vector<AudioSourceRow> out = NormalizeSourceRowsForTarget(rows, /*window_target=*/false);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].gain_db, -6.0f);
    EXPECT_TRUE(out[0].muted);
}

TEST(NormalizeSourceRowsForTargetTest, RewrittenSysDoesNotDuplicateAnExistingSystemOutput) {
    // Two full loopbacks would record the same audio twice.
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::SystemOutput, true, false},
        {AudioSourceKind::Sys, true, false},
    };

    const std::vector<AudioSourceKind> kinds = KindsOf(NormalizeSourceRowsForTarget(rows, /*window_target=*/false));
    EXPECT_EQ(kinds, (std::vector<AudioSourceKind>{AudioSourceKind::SystemOutput}));
}

TEST(NormalizeSourceRowsForTargetTest, DisplayTargetPlanNeverAsksForAProcessId) {
    // The end-to-end property the failure was about: whatever the stored rows, a
    // display plan contains no process-scoped source.
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, true, false},
        {AudioSourceKind::Sys, true, false},
        {AudioSourceKind::Mic, true, false},
    };

    const AudioTrackPlan plan = ResolveAudioTracks(NormalizeSourceRowsForTarget(rows, /*window_target=*/false));
    for (const auto& track : plan.tracks) {
        for (const AudioSourceKind kind : track.sources) {
            EXPECT_NE(kind, AudioSourceKind::App);
            EXPECT_NE(kind, AudioSourceKind::Sys);
        }
    }
    EXPECT_FALSE(plan.tracks.empty()) << "system audio and the microphone survive";
}

// ---------------------------------------------------------------------------
// AudioSourceKindDisplayName / DeriveAudioTrackName
//
// The muxed-file track name (KaxTrackName, see matroska_stream_writer.cpp) is
// built from these two pure functions rather than from an if-chain inside the
// writer, so the source->name mapping is testable independent of the
// container format.
// ---------------------------------------------------------------------------

using exosnap::engine::AudioSourceKindDisplayName;
using exosnap::engine::DeriveAudioTrackName;
using exosnap::engine::ResolvedAudioTrack;

ResolvedAudioTrack MakeTrack(const std::initializer_list<AudioSourceKind> sources) {
    ResolvedAudioTrack track;
    track.sources = std::vector<AudioSourceKind>(sources);
    track.source_gain_linear.assign(track.sources.size(), 1.0f);
    return track;
}

TEST(AudioSourceKindDisplayNameTest, EachKindHasItsOwnLabel) {
    EXPECT_EQ(AudioSourceKindDisplayName(AudioSourceKind::App), "Application");
    EXPECT_EQ(AudioSourceKindDisplayName(AudioSourceKind::Mic), "Microphone");
    EXPECT_EQ(AudioSourceKindDisplayName(AudioSourceKind::Sys), "System");
}

TEST(AudioSourceKindDisplayNameTest, SystemOutputSharesSysLabel) {
    // SystemOutput is the pid-free stand-in NormalizeSourceRowsForTarget swaps
    // in for Sys on a Display/Region target -- to the user it is still "the
    // system", so it must not surface a different label.
    EXPECT_EQ(AudioSourceKindDisplayName(AudioSourceKind::SystemOutput),
              AudioSourceKindDisplayName(AudioSourceKind::Sys));
}

TEST(DeriveAudioTrackNameTest, SingleSourceIsJustItsDisplayName) {
    EXPECT_EQ(DeriveAudioTrackName(MakeTrack({AudioSourceKind::App})), "Application");
    EXPECT_EQ(DeriveAudioTrackName(MakeTrack({AudioSourceKind::Mic})), "Microphone");
    EXPECT_EQ(DeriveAudioTrackName(MakeTrack({AudioSourceKind::Sys})), "System");
    EXPECT_EQ(DeriveAudioTrackName(MakeTrack({AudioSourceKind::SystemOutput})), "System");
}

TEST(DeriveAudioTrackNameTest, MergedSourcesJoinInResolutionOrder) {
    EXPECT_EQ(DeriveAudioTrackName(MakeTrack({AudioSourceKind::Sys, AudioSourceKind::Mic})), "System + Microphone");
    // Reversed input order produces a reversed name -- the join must follow
    // ResolvedAudioTrack::sources exactly, never re-sort it (e.g. alphabetically).
    EXPECT_EQ(DeriveAudioTrackName(MakeTrack({AudioSourceKind::Mic, AudioSourceKind::Sys})), "Microphone + System");
}

TEST(DeriveAudioTrackNameTest, ThreeMergedSourcesJoinAll) {
    EXPECT_EQ(DeriveAudioTrackName(MakeTrack({AudioSourceKind::App, AudioSourceKind::Sys, AudioSourceKind::Mic})),
              "Application + System + Microphone");
}

TEST(DeriveAudioTrackNameTest, EndToEnd_FollowsResolveAudioTracksMergeOrder) {
    // The name a real merged row config produces, driven through the actual
    // resolver rather than a hand-built ResolvedAudioTrack.
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, true, false},
        {AudioSourceKind::Mic, true, true},
        {AudioSourceKind::Sys, true, false},
    };
    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 2u);
    EXPECT_EQ(DeriveAudioTrackName(plan.tracks[0]), "Application + Microphone");
    EXPECT_EQ(DeriveAudioTrackName(plan.tracks[1]), "System");
}

} // namespace
