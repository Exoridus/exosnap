#include "benchmark/BenchmarkEffectiveConfig.h"

#include <exosnap/engine/recorder_session.h>
#include <gtest/gtest.h>

namespace exosnap::benchmark {
namespace {

TEST(BenchmarkEffectiveConfig, AudioCountUsesResolvedTracksAndHonorsDisabledAudio) {
    engine::RecorderConfig config;
    config.record_audio = true;
    config.resolved_audio_plan.emplace();
    config.resolved_audio_plan->tracks.resize(8);
    EXPECT_TRUE(DescribeEffectiveConfig(config).fields.contains(QStringLiteral("audio_track_count=8")));
    config.record_audio = false;
    EXPECT_TRUE(DescribeEffectiveConfig(config).fields.contains(QStringLiteral("audio_track_count=0")));
}

TEST(BenchmarkEffectiveConfig, LegacyAudioRetainsImplicitSingleTrackAndExplicitPlan) {
    engine::RecorderConfig config;
    config.record_audio = true;
    EXPECT_TRUE(DescribeEffectiveConfig(config).fields.contains(QStringLiteral("audio_track_count=1")));
    config.audio_track_plan.tracks.resize(2);
    EXPECT_TRUE(DescribeEffectiveConfig(config).fields.contains(QStringLiteral("audio_track_count=2")));
}

} // namespace
} // namespace exosnap::benchmark
