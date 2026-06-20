// test_audio_gain_model.cpp
// Unit tests for GainDbToLinear and per-row gain propagation (ADR 0018).

#include <gtest/gtest.h>

#include <recorder_core/audio_track_model.h>

#include <cmath>
#include <vector>

namespace {

using recorder_core::AudioSourceKind;
using recorder_core::AudioSourceRow;
using recorder_core::AudioTrackPlan;
using recorder_core::GainDbToLinear;
using recorder_core::kMaxGainDb;
using recorder_core::kMinGainDb;
using recorder_core::ResolveAudioTracks;

// ---------------------------------------------------------------------------
// GainDbToLinear
// ---------------------------------------------------------------------------

TEST(GainDbToLinear, ZeroDb_ReturnsUnity) {
    EXPECT_NEAR(GainDbToLinear(0.0f, false), 1.0f, 1e-5f);
}

TEST(GainDbToLinear, Muted_ReturnsZero) {
    EXPECT_FLOAT_EQ(GainDbToLinear(0.0f, true), 0.0f);
    EXPECT_FLOAT_EQ(GainDbToLinear(12.0f, true), 0.0f);
    EXPECT_FLOAT_EQ(GainDbToLinear(-60.0f, true), 0.0f);
}

TEST(GainDbToLinear, MinusNinetyNineDb_NearZero) {
    // -60 dB should yield approximately 0.001.
    const float val = GainDbToLinear(kMinGainDb, false);
    EXPECT_NEAR(val, 0.001f, 1e-4f);
}

TEST(GainDbToLinear, PlusTwentyFourDb_AboveUnity) {
    const float val = GainDbToLinear(kMaxGainDb, false);
    // 10^(24/20) ≈ 15.85
    EXPECT_NEAR(val, 15.848932f, 1e-3f);
}

TEST(GainDbToLinear, MinusSixDb_HalfAmplitude) {
    // -6.0206 dB ≈ 0.5 amplitude; -6 dB ≈ 0.501.
    const float val = GainDbToLinear(-6.0f, false);
    EXPECT_NEAR(val, 0.5012f, 1e-3f);
}

TEST(GainDbToLinear, PlusSixDb_DoubleAmplitude) {
    const float val = GainDbToLinear(6.0f, false);
    EXPECT_NEAR(val, 1.9953f, 1e-3f);
}

// ---------------------------------------------------------------------------
// ResolveAudioTracks — source_gain_linear propagation
// ---------------------------------------------------------------------------

TEST(AudioGainModel, DefaultRows_UnityGain) {
    // Default rows: 0 dB, not muted → each track has gain 1.0.
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, true, false, 0.0f, false},
        {AudioSourceKind::Mic, true, false, 0.0f, false},
        {AudioSourceKind::Sys, true, false, 0.0f, false},
    };
    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 3u);
    for (const auto& track : plan.tracks) {
        ASSERT_EQ(track.source_gain_linear.size(), track.sources.size());
        for (const float g : track.source_gain_linear) {
            EXPECT_NEAR(g, 1.0f, 1e-5f);
        }
    }
}

TEST(AudioGainModel, MutedRow_ZeroGainInPlan) {
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, true, false, 0.0f, false}, // not muted
        {AudioSourceKind::Mic, true, false, 0.0f, true},  // muted
        {AudioSourceKind::Sys, true, false, 0.0f, false}, // not muted
    };
    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 3u);

    // App → unity
    ASSERT_EQ(plan.tracks[0].source_gain_linear.size(), 1u);
    EXPECT_NEAR(plan.tracks[0].source_gain_linear[0], 1.0f, 1e-5f);

    // Mic → muted = 0.0
    ASSERT_EQ(plan.tracks[1].source_gain_linear.size(), 1u);
    EXPECT_FLOAT_EQ(plan.tracks[1].source_gain_linear[0], 0.0f);

    // Sys → unity
    ASSERT_EQ(plan.tracks[2].source_gain_linear.size(), 1u);
    EXPECT_NEAR(plan.tracks[2].source_gain_linear[0], 1.0f, 1e-5f);
}

TEST(AudioGainModel, GainDb_PropagatesCorrectly) {
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, true, false, 6.0f, false},  // +6 dB ≈ 1.995
        {AudioSourceKind::Mic, true, false, -6.0f, false}, // −6 dB ≈ 0.501
    };
    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 2u);

    ASSERT_EQ(plan.tracks[0].source_gain_linear.size(), 1u);
    EXPECT_NEAR(plan.tracks[0].source_gain_linear[0], 1.9953f, 1e-3f);

    ASSERT_EQ(plan.tracks[1].source_gain_linear.size(), 1u);
    EXPECT_NEAR(plan.tracks[1].source_gain_linear[0], 0.5012f, 1e-3f);
}

TEST(AudioGainModel, MergedTrack_PerSourceGain) {
    // App at 0 dB, Mic merged at +6 dB.
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, true, false, 0.0f, false},
        {AudioSourceKind::Mic, true, true, 6.0f, false},
    };
    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 1u);
    ASSERT_EQ(plan.tracks[0].sources.size(), 2u);
    ASSERT_EQ(plan.tracks[0].source_gain_linear.size(), 2u);

    // App → unity
    EXPECT_NEAR(plan.tracks[0].source_gain_linear[0], 1.0f, 1e-5f);
    // Mic → +6 dB
    EXPECT_NEAR(plan.tracks[0].source_gain_linear[1], 1.9953f, 1e-3f);
}

TEST(AudioGainModel, MergedTrack_OneMuted) {
    // App and Mic merged, Mic is muted.
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, true, false, 0.0f, false}, {AudioSourceKind::Mic, true, true, 0.0f, true}, // muted
    };
    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 1u);
    ASSERT_EQ(plan.tracks[0].source_gain_linear.size(), 2u);

    EXPECT_NEAR(plan.tracks[0].source_gain_linear[0], 1.0f, 1e-5f);
    EXPECT_FLOAT_EQ(plan.tracks[0].source_gain_linear[1], 0.0f);
}

TEST(AudioGainModel, DisabledRow_NotInPlan) {
    // Disabled rows must not appear in the plan at all — unchanged behavior.
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, false, false, 0.0f, false},
        {AudioSourceKind::Mic, true, false, 0.0f, false},
    };
    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    ASSERT_EQ(plan.tracks.size(), 1u);
    ASSERT_EQ(plan.tracks[0].sources.size(), 1u);
    EXPECT_EQ(plan.tracks[0].sources[0], AudioSourceKind::Mic);
    ASSERT_EQ(plan.tracks[0].source_gain_linear.size(), 1u);
    EXPECT_NEAR(plan.tracks[0].source_gain_linear[0], 1.0f, 1e-5f);
}

TEST(AudioGainModel, GainParallelVectorAlwaysMatchesSources) {
    // Invariant: source_gain_linear.size() == sources.size() for every track.
    const std::vector<AudioSourceRow> rows = {
        {AudioSourceKind::App, true, false, 3.0f, false},
        {AudioSourceKind::Mic, true, true, -3.0f, false},
        {AudioSourceKind::Sys, true, true, 0.0f, true},
        {AudioSourceKind::App, true, false, 0.0f, false},
    };
    const AudioTrackPlan plan = ResolveAudioTracks(rows);
    for (const auto& track : plan.tracks) {
        EXPECT_EQ(track.source_gain_linear.size(), track.sources.size());
    }
}

// ---------------------------------------------------------------------------
// GainDbToLinear — domain constants
// ---------------------------------------------------------------------------

TEST(AudioGainModel, Constants_Valid) {
    EXPECT_LT(kMinGainDb, 0.0f);
    EXPECT_GT(kMaxGainDb, 0.0f);
    EXPECT_LT(kMinGainDb, kMaxGainDb);
}

} // namespace
