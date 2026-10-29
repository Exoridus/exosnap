#include <algorithm>
#include <cstdint>
#include <exosnap/engine/recording_media_types.h>
#include <gtest/gtest.h>

#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/audio_track_model.h>

#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
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

// ---------------------------------------------------------------------------
// Track indices are positions, so a plan has to be indexed like one
// ---------------------------------------------------------------------------
//
// track_index is where a worker writes: codec-private slots, per-track RMS,
// measured epochs, aligned durations -- all fixed-size arrays indexed by it, and
// the mux decides it has every header by asking for slots 0..count-1.
//
// Validation accepted anything. A plan indexed {0, 2} has two workers and leaves
// slot 1 empty forever, so the mux waits for a header nobody sends and the
// recording produces nothing while looking busy. These cases reject that before a
// worker starts, because afterwards the symptom names no cause.

namespace {

using exosnap::engine::DescribeInvalidTrackIndices;
using exosnap::engine::ResolvedAudioTrack;

AudioTrackPlan PlanWithIndices(const std::vector<uint32_t>& indices) {
    AudioTrackPlan plan;
    for (const uint32_t index : indices) {
        ResolvedAudioTrack track;
        track.sources = {AudioSourceKind::SystemOutput};
        track.source_gain_linear = {1.0f};
        track.track_index = index;
        plan.tracks.push_back(std::move(track));
    }
    return plan;
}

TEST(AudioTrackIndices, TheNormalDenseResolverPlanIsAccepted) {
    // The positive case, and the one that must never regress: what
    // ResolveAudioTracks actually produces.
    EXPECT_TRUE(DescribeInvalidTrackIndices(PlanWithIndices({0}), 3).empty());
    EXPECT_TRUE(DescribeInvalidTrackIndices(PlanWithIndices({0, 1}), 3).empty());
    EXPECT_TRUE(DescribeInvalidTrackIndices(PlanWithIndices({0, 1, 2}), 3).empty());
}

TEST(AudioTrackIndices, AnEmptyPlanIsAccepted) {
    // No plan means the single-loopback default path, which indexes itself.
    EXPECT_TRUE(DescribeInvalidTrackIndices(AudioTrackPlan{}, 3).empty());
}

TEST(AudioTrackIndices, AGapLeavesASlotNobodyFillsAndIsRejected) {
    const std::string reason = DescribeInvalidTrackIndices(PlanWithIndices({0, 2}), 3);
    ASSERT_FALSE(reason.empty()) << "a sparse plan was accepted";
    EXPECT_NE(reason.find("track_index 2"), std::string::npos) << reason;
    EXPECT_NE(reason.find("dense"), std::string::npos) << reason;
}

TEST(AudioTrackIndices, ADuplicateIndexIsRejected) {
    // Two workers writing one slot: the second silently overwrites the first.
    EXPECT_FALSE(DescribeInvalidTrackIndices(PlanWithIndices({0, 0}), 3).empty());
    EXPECT_FALSE(DescribeInvalidTrackIndices(PlanWithIndices({0, 1, 1}), 3).empty());
}

TEST(AudioTrackIndices, WrongOrderIsRejected) {
    // The contract is positional, so {1, 0} is not "the same set in another order";
    // it is two tracks writing each other's slots.
    EXPECT_FALSE(DescribeInvalidTrackIndices(PlanWithIndices({1, 0}), 3).empty());
    EXPECT_FALSE(DescribeInvalidTrackIndices(PlanWithIndices({0, 2, 1}), 3).empty());
}

TEST(AudioTrackIndices, AnOutOfRangeIndexIsRejected) {
    // The arrays are three long. An index past that is an out-of-bounds write
    // waiting for the first worker to publish anything.
    EXPECT_FALSE(DescribeInvalidTrackIndices(PlanWithIndices({3}), 3).empty());
    EXPECT_FALSE(DescribeInvalidTrackIndices(PlanWithIndices({0, 1, 99}), 3).empty());
}

TEST(AudioTrackIndices, TooManyTracksIsRejectedBeforeTheIndicesAreRead) {
    // Four dense indices are individually fine and still one track too many.
    const std::string reason = DescribeInvalidTrackIndices(PlanWithIndices({0, 1, 2, 3}), 3);
    ASSERT_FALSE(reason.empty());
    EXPECT_NE(reason.find("at most 3"), std::string::npos) << reason;
}

TEST(AudioTrackIndices, WhatTheResolverProducesIsAlwaysAccepted) {
    // The two halves joined: every plan the real resolver builds from ordinary
    // rows passes the validation the engine now applies to it. A rule the
    // production resolver cannot satisfy would reject every recording.
    for (const size_t row_count : {size_t{1}, size_t{2}, size_t{3}}) {
        std::vector<AudioSourceRow> rows;
        for (size_t i = 0; i < row_count; ++i) {
            AudioSourceRow row;
            row.kind = AudioSourceKind::SystemOutput;
            row.enabled = true;
            row.merge_with_above = false;
            rows.push_back(row);
        }
        const AudioTrackPlan plan = ResolveAudioTracks(rows);
        EXPECT_TRUE(DescribeInvalidTrackIndices(plan, 3).empty())
            << "the resolver produced a plan the engine would reject, for " << row_count << " row(s)";
    }
}

} // namespace
TEST(AudioIntentResolver, LegacyParticipationRecomputesAnchors) {
    using namespace exosnap::engine;
    LegacyRows25Intent legacy;
    legacy.id_namespace = NewId<PresetNamespaceId>();
    for (int i = 0; i != 3; ++i) {
        LegacyAudioRow row;
        row.row_id = NewId<LegacyRowId>();
        row.source_id = NewId<AudioSourceId>();
        row.anchor_track_id = NewId<AudioTrackId>();
        row.row = {AudioSourceKind::Mic, i != 1, i == 2};
        legacy.rows.push_back(row);
    }
    AudioEnvironmentSnapshot env;
    env.default_microphone_endpoint = "mic";
    env.microphone_endpoints = {"mic"};
    auto initial = ResolveAudioIntent(legacy, env);
    ASSERT_TRUE(initial.succeeded());
    ASSERT_EQ(initial.value()->tracks.size(), 1u);
    EXPECT_EQ(initial.value()->tracks[0].routes.size(), 2u);
    LegacyRowPatch patch;
    patch.enabled = true;
    auto edited = ApplyLegacyRowEdit(legacy, legacy.rows[1].row_id, patch);
    ASSERT_TRUE(edited.succeeded());
    auto enabled = ResolveAudioIntent(*edited.value(), env);
    ASSERT_TRUE(enabled.succeeded());
    ASSERT_EQ(enabled.value()->tracks.size(), 2u);
    EXPECT_EQ(enabled.value()->tracks[0].id, legacy.rows[0].anchor_track_id);
    EXPECT_EQ(enabled.value()->tracks[1].id, legacy.rows[1].anchor_track_id);
    EXPECT_EQ(enabled.value()->tracks[1].routes.size(), 2u);
}

TEST(AudioIntentResolver, NativeMutedOrNonparticipatingSourceKeepsTrackAndDivisor) {
    using namespace exosnap::engine;
    TrackGraph1Intent graph;
    AudioSourceSpec source;
    source.id = NewId<AudioSourceId>();
    source.participation = false;
    graph.sources.push_back(source);
    AudioTrackSpec track;
    track.id = NewId<AudioTrackId>();
    graph.tracks.push_back(track);
    graph.routes.push_back({NewId<AudioRouteId>(), source.id, track.id});
    graph.default_monitor_track_ids = {track.id};
    auto resolved = ResolveAudioIntent(graph, {});
    ASSERT_TRUE(resolved.succeeded());
    ASSERT_EQ(resolved.value()->tracks.size(), 1u);
    EXPECT_EQ(resolved.value()->tracks[0].fixed_average_divisor, 1u);
    EXPECT_EQ(resolved.value()->tracks[0].routes[0].gain_linear, 1.0f);
    EXPECT_FALSE(resolved.value()->sources[0].capture_key.has_value());
}

TEST(AudioIntentResolver, NativeMixedAndMicrophoneTracksPreserveFanoutAcrossCaptureTargets) {
    using namespace exosnap::engine;
    TrackGraph1Intent graph;
    AudioSourceSpec system;
    system.id = NewId<AudioSourceId>();
    AudioSourceSpec microphone;
    microphone.id = NewId<AudioSourceId>();
    microphone.binding.kind = AudioBindingKind::MicrophoneEndpoint;
    graph.sources = {system, microphone};
    AudioTrackSpec mix;
    mix.id = NewId<AudioTrackId>();
    mix.order = 1;
    mix.name_kind = AudioTrackNameKind::Custom;
    mix.name = "Mix";
    AudioTrackSpec stem;
    stem.id = NewId<AudioTrackId>();
    stem.order = 0;
    graph.tracks = {mix, stem};
    graph.routes = {{NewId<AudioRouteId>(), system.id, mix.id},
                    {NewId<AudioRouteId>(), microphone.id, mix.id, -6.f},
                    {NewId<AudioRouteId>(), microphone.id, stem.id, 12.f}};
    AudioEnvironmentSnapshot environment;
    environment.default_playback_endpoint = "speaker";
    environment.playback_endpoints = {"speaker"};
    environment.default_microphone_endpoint = "mic";
    environment.microphone_endpoints = {"mic"};
    environment.default_playback_by_role[static_cast<size_t>(AudioEndpointRole::Multimedia)] = "speaker";
    environment.default_microphone_by_role[static_cast<size_t>(AudioEndpointRole::Multimedia)] = "mic";
    for (const bool window_target : {true, false}) {
        environment.window_target = window_target;
        auto resolved = ResolveAudioIntent(graph, environment);
        ASSERT_TRUE(resolved.succeeded()) << resolved.error()->context;
        ASSERT_EQ(resolved.value()->tracks.size(), 2u);
        const auto& tracks = resolved.value()->tracks;
        EXPECT_EQ(tracks[0].id, stem.id);
        EXPECT_EQ(tracks[1].name, "Mix");
        EXPECT_EQ(tracks[0].mux_index.value, 0u);
        EXPECT_EQ(tracks[1].mux_index.value, 1u);
        EXPECT_EQ(tracks[0].routes[0].source_id, tracks[1].routes[1].source_id);
        EXPECT_NEAR(tracks[0].routes[0].gain_linear, GainDbToLinear(12.f, false), 0.0001f);
        EXPECT_NEAR(tracks[1].routes[1].gain_linear, GainDbToLinear(-6.f, false), 0.0001f);
        EXPECT_EQ(tracks[1].fixed_average_divisor, 2u);
        EXPECT_EQ(resolved.value()->sources[0].capture_key->kind, AudioBindingKind::PlaybackEndpoint);
    }
    graph.tracks[1].record_enabled = false;
    graph.sources[1].participation = false;
    const auto silent = ResolveAudioIntent(graph, environment);
    ASSERT_TRUE(silent.succeeded());
    ASSERT_EQ(silent.value()->tracks.size(), 1u);
    EXPECT_EQ(silent.value()->tracks[0].id, mix.id);
    EXPECT_EQ(silent.value()->tracks[0].routes.size(), 2u);
    EXPECT_FALSE(silent.value()->sources[1].capture_key);
}

TEST(AudioIntentResolver, MaterializationKeepsNormalizedLegacyRowsUnassigned) {
    using namespace exosnap::engine;
    LegacyRows25Intent legacy;
    legacy.id_namespace = NewId<PresetNamespaceId>();
    LegacyAudioRow row;
    row.row_id = NewId<LegacyRowId>();
    row.source_id = NewId<AudioSourceId>();
    row.anchor_track_id = NewId<AudioTrackId>();
    row.row = {AudioSourceKind::App, true, false};
    legacy.rows.push_back(row);
    auto graph = MaterializeLegacyIntent(legacy, {});
    ASSERT_TRUE(graph.succeeded());
    ASSERT_EQ(graph.value()->sources.size(), 1u);
    EXPECT_FALSE(graph.value()->sources[0].participation);
    EXPECT_EQ(graph.value()->sources[0].legacy_row_id, row.row_id);
    EXPECT_TRUE(graph.value()->routes.empty());
}
TEST(AudioIntentResolver, NonparticipatingMissingEndpointIsObservableAndDoesNotBlock) {
    using namespace exosnap::engine;
    TrackGraph1Intent graph;
    AudioSourceSpec source;
    source.id = NewId<AudioSourceId>();
    source.participation = false;
    graph.sources.push_back(source);
    AudioTrackSpec track;
    track.id = NewId<AudioTrackId>();
    graph.tracks.push_back(track);
    graph.routes.push_back({NewId<AudioRouteId>(), source.id, track.id});
    auto resolved = ResolveAudioIntent(graph, {});
    ASSERT_TRUE(resolved.succeeded());
    EXPECT_EQ(resolved.value()->sources[0].availability, AudioSourceAvailability::EndpointUnavailable);
    EXPECT_FALSE(resolved.value()->sources[0].capture_key);
    ASSERT_EQ(resolved.value()->tracks.size(), 1u);
}

TEST(AudioIntentResolver, DefaultAndFixedEndpointShareCaptureAndRequireMonitorConfirmation) {
    using namespace exosnap::engine;
    TrackGraph1Intent graph;
    AudioTrackSpec track;
    track.id = NewId<AudioTrackId>();
    graph.tracks.push_back(track);
    for (bool follow_default : {true, false}) {
        AudioSourceSpec source;
        source.id = NewId<AudioSourceId>();
        source.binding.follow_default = follow_default;
        source.binding.endpoint_id = follow_default ? "" : "playback";
        graph.sources.push_back(source);
        graph.routes.push_back({NewId<AudioRouteId>(), source.id, track.id});
    }
    AudioEnvironmentSnapshot environment;
    environment.default_playback_endpoint = "playback";
    environment.default_playback_by_role[static_cast<size_t>(AudioEndpointRole::Multimedia)] = "playback";
    environment.playback_endpoints = {"playback"};
    auto resolved = ResolveAudioIntent(graph, environment);
    ASSERT_TRUE(resolved.succeeded());
    EXPECT_EQ(resolved.value()->sources[0].capture_key, resolved.value()->sources[1].capture_key);
    EXPECT_TRUE(resolved.value()->monitor_confirmation_required);
    graph.monitor_selection_origin = MonitorSelectionOrigin::User;
    graph.default_monitor_track_ids = {track.id};
    auto user = ResolveAudioIntent(graph, environment);
    ASSERT_TRUE(user.succeeded());
    EXPECT_FALSE(user.value()->monitor_confirmation_required);
    EXPECT_FALSE(user.value()->warnings.empty());
}

TEST(AudioIntentResolver, TargetAndIndependentConfirmedProcessShareCapture) {
    using namespace exosnap::engine;
    const ProcessInstanceIdentity process{123, 456, "player"};
    TrackGraph1Intent graph;
    AudioTrackSpec track;
    track.id = NewId<AudioTrackId>();
    graph.tracks.push_back(track);
    for (const auto kind : {AudioBindingKind::CaptureTargetProcess, AudioBindingKind::IndependentProcess}) {
        AudioSourceSpec source;
        source.id = NewId<AudioSourceId>();
        source.binding.kind = kind;
        source.binding.process = process;
        graph.sources.push_back(source);
        graph.routes.push_back({NewId<AudioRouteId>(), source.id, track.id});
    }
    AudioEnvironmentSnapshot environment;
    environment.window_target = true;
    environment.process_loopback_supported = true;
    environment.capture_target_process = process;
    environment.confirmed_processes = {process};
    auto resolved = ResolveAudioIntent(graph, environment);
    ASSERT_TRUE(resolved.succeeded());
    EXPECT_EQ(resolved.value()->sources[0].capture_key, resolved.value()->sources[1].capture_key);
    EXPECT_EQ(resolved.value()->sources[0].spec.binding.kind, AudioBindingKind::CaptureTargetProcess);
    environment.confirmed_processes[0].creation_time = 457;
    EXPECT_FALSE(ResolveAudioIntent(graph, environment).succeeded());
}

TEST(AudioIntentResolver, LegacyDisabledSystemCollisionDoesNotEnableLaterSys) {
    using namespace exosnap::engine;
    LegacyRows25Intent legacy;
    legacy.id_namespace = NewId<PresetNamespaceId>();
    for (const auto kind : {AudioSourceKind::SystemOutput, AudioSourceKind::Sys, AudioSourceKind::Mic}) {
        LegacyAudioRow row;
        row.row_id = NewId<LegacyRowId>();
        row.source_id = NewId<AudioSourceId>();
        row.anchor_track_id = NewId<AudioTrackId>();
        row.row = {kind, kind != AudioSourceKind::SystemOutput, false};
        legacy.rows.push_back(row);
    }
    AudioEnvironmentSnapshot environment;
    environment.default_microphone_endpoint = "mic";
    environment.microphone_endpoints = {"mic"};
    auto resolved = ResolveAudioIntent(legacy, environment);
    ASSERT_TRUE(resolved.succeeded());
    ASSERT_EQ(resolved.value()->tracks.size(), 1u);
    EXPECT_EQ(resolved.value()->tracks[0].id, legacy.rows[2].anchor_track_id);
    EXPECT_EQ(resolved.value()->tracks[0].routes[0].source_id, legacy.rows[2].source_id);
    EXPECT_EQ(legacy.rows[1].row.kind, AudioSourceKind::Sys);
}

TEST(AudioIntentResolver, EmptyNativeCardsKeepVisiblePositionAndDenseMuxIndices) {
    using namespace exosnap::engine;
    TrackGraph1Intent graph;
    AudioSourceSpec source;
    source.id = NewId<AudioSourceId>();
    source.participation = false;
    graph.sources.push_back(source);
    for (uint32_t order : {2u, 0u, 1u}) {
        AudioTrackSpec track;
        track.id = NewId<AudioTrackId>();
        track.order = order;
        graph.tracks.push_back(track);
    }
    graph.routes.push_back({NewId<AudioRouteId>(), source.id, graph.tracks[0].id});
    auto resolved = ResolveAudioIntent(graph, {});
    ASSERT_TRUE(resolved.succeeded());
    ASSERT_EQ(resolved.value()->tracks.size(), 1u);
    EXPECT_EQ(resolved.value()->tracks[0].id, graph.tracks[0].id);
    EXPECT_EQ(resolved.value()->tracks[0].visible_position, 3u);
    EXPECT_EQ(resolved.value()->tracks[0].name, "Audio 3");
    EXPECT_EQ(resolved.value()->tracks[0].mux_index.value, 0u);
    graph.routes[0].source_id = NewId<AudioSourceId>();
    EXPECT_FALSE(ResolveAudioIntent(graph, {}).succeeded());
}
TEST(AudioIntentResolver, LegacyOracleMatchesAllThreeRowKindsParticipationMergeAndMuteMasks) {
    using namespace exosnap::engine;
    for (uint32_t kinds = 0; kinds < 64; ++kinds) {
        LegacyRows25Intent legacy;
        legacy.id_namespace = NewId<PresetNamespaceId>();
        legacy.microphone_binding.endpoint_role = AudioEndpointRole::Console;
        for (uint32_t ordinal = 0; ordinal < 3; ++ordinal) {
            LegacyAudioRow row;
            row.row_id = NewId<LegacyRowId>();
            row.source_id = NewId<AudioSourceId>();
            row.anchor_track_id = NewId<AudioTrackId>();
            row.row.kind = static_cast<AudioSourceKind>((kinds >> (2 * ordinal)) & 3);
            row.row.gain_db = float(int(ordinal) * 6 - 6);
            legacy.rows.push_back(row);
        }
        for (uint32_t enabled = 0; enabled < 8; ++enabled)
            for (uint32_t merged = 0; merged < 8; ++merged)
                for (uint32_t muted = 0; muted < 8; ++muted)
                    for (bool window : {false, true}) {
                        std::vector<AudioSourceRow> raw;
                        for (uint32_t ordinal = 0; ordinal < 3; ++ordinal) {
                            auto& row = legacy.rows[ordinal].row;
                            row.enabled = (enabled & (1u << ordinal)) != 0;
                            row.merge_with_above = (merged & (1u << ordinal)) != 0;
                            row.muted = (muted & (1u << ordinal)) != 0;
                            raw.push_back(row);
                        }
                        const auto oracle = ResolveAudioTracks(NormalizeSourceRowsForTarget(raw, window));
                        AudioEnvironmentSnapshot environment;
                        environment.window_target = window;
                        environment.process_loopback_supported = true;
                        environment.capture_target_process = ProcessInstanceIdentity{42, 99, "fixture"};
                        environment.confirmed_processes = {*environment.capture_target_process};
                        environment.default_microphone_endpoint = "mic";
                        environment.default_playback_endpoint = "playback";
                        environment.microphone_endpoints = {"mic"};
                        environment.playback_endpoints = {"playback"};
                        const auto result = ResolveAudioIntent(legacy, environment);
                        ASSERT_TRUE(result.succeeded())
                            << kinds << ',' << enabled << ',' << merged << ',' << muted << ',' << window;
                        ASSERT_EQ(result.value()->tracks.size(), oracle.tracks.size());
                        for (size_t track = 0; track < oracle.tracks.size(); ++track) {
                            const auto& resolved = result.value()->tracks[track];
                            ASSERT_EQ(resolved.routes.size(), oracle.tracks[track].sources.size());
                            EXPECT_EQ(resolved.fixed_average_divisor, resolved.routes.size());
                            for (size_t route = 0; route < resolved.routes.size(); ++route) {
                                const auto source = std::find_if(
                                    result.value()->sources.begin(), result.value()->sources.end(),
                                    [&](const auto& item) { return item.spec.id == resolved.routes[route].source_id; });
                                ASSERT_NE(source, result.value()->sources.end());
                                const auto kind = source->spec.binding.kind == AudioBindingKind::MicrophoneEndpoint
                                                      ? AudioSourceKind::Mic
                                                  : source->spec.binding.kind == AudioBindingKind::PlaybackEndpoint
                                                      ? AudioSourceKind::SystemOutput
                                                  : source->spec.binding.kind == AudioBindingKind::LegacyExcludeTarget
                                                      ? AudioSourceKind::Sys
                                                      : AudioSourceKind::App;
                                EXPECT_EQ(kind, oracle.tracks[track].sources[route]);
                                EXPECT_FLOAT_EQ(source->spec.muted ? 0.0f : resolved.routes[route].gain_linear,
                                                oracle.tracks[track].source_gain_linear[route]);
                            }
                        }
                    }
    }
}

TEST(AudioIntentResolver, FrozenMonitorSelectionOmitsEmptyAndDisabledCards) {
    using namespace exosnap::engine;
    TrackGraph1Intent graph;
    AudioSourceSpec source;
    source.id = NewId<AudioSourceId>();
    source.participation = false;
    graph.sources.push_back(source);
    for (uint32_t order = 0; order < 3; ++order) {
        AudioTrackSpec track;
        track.id = NewId<AudioTrackId>();
        track.order = order;
        track.record_enabled = order != 1;
        graph.tracks.push_back(track);
        graph.default_monitor_track_ids.push_back(track.id);
        if (order != 0)
            graph.routes.push_back({NewId<AudioRouteId>(), source.id, track.id});
    }
    graph.monitor_selection_origin = MonitorSelectionOrigin::User;
    const auto result = ResolveAudioIntent(graph, {});
    ASSERT_TRUE(result.succeeded());
    ASSERT_EQ(result.value()->default_monitor_track_ids.size(), 1u);
    EXPECT_EQ(result.value()->default_monitor_track_ids[0], graph.tracks[2].id);
    EXPECT_EQ(graph.default_monitor_track_ids.size(), 3u);
    EXPECT_EQ(result.value()->warnings.size(), 2u);
}

TEST(AudioIntentResolver, LegacySystemOutputKeepsConsoleDefaultRole) {
    using namespace exosnap::engine;
    LegacyRows25Intent legacy;
    legacy.id_namespace = NewId<PresetNamespaceId>();
    LegacyAudioRow row;
    row.row_id = NewId<LegacyRowId>();
    row.source_id = NewId<AudioSourceId>();
    row.anchor_track_id = NewId<AudioTrackId>();
    row.row.kind = AudioSourceKind::SystemOutput;
    legacy.rows.push_back(row);
    AudioEnvironmentSnapshot environment;
    environment.playback_endpoints = {"console", "multimedia"};
    environment.default_playback_by_role[0] = "console";
    environment.default_playback_by_role[1] = "multimedia";
    const auto result = ResolveAudioIntent(legacy, environment);
    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.value()->sources[0].capture_key->endpoint_id, "console");
}
