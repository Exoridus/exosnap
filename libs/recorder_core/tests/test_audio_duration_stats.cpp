#include <gtest/gtest.h>

#include "session_internal.h"

#include <cstdint>
#include <mutex>

namespace {

using recorder_core::SessionState;

void ApplyAudioDurationMax(SessionState& state, uint64_t last_audio_pts) {
    std::lock_guard lk(state.stats_mutex);
    if (last_audio_pts > state.stats.audio_duration_ns) {
        state.stats.audio_duration_ns = last_audio_pts;
    }
}

TEST(AudioDurationStatsTest, Stats_AudioDurationUsesMax) {
    SessionState state{};

    ApplyAudioDurationMax(state, 100);
    {
        std::lock_guard lk(state.stats_mutex);
        EXPECT_EQ(state.stats.audio_duration_ns, 100u);
    }

    ApplyAudioDurationMax(state, 200);
    {
        std::lock_guard lk(state.stats_mutex);
        EXPECT_EQ(state.stats.audio_duration_ns, 200u);
    }

    ApplyAudioDurationMax(state, 100);
    {
        std::lock_guard lk(state.stats_mutex);
        EXPECT_EQ(state.stats.audio_duration_ns, 200u);
    }
}

} // namespace
