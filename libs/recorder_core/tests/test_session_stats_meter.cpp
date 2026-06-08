#include <gtest/gtest.h>

#include "session_internal.h"
#include "session_stats_collector.h"

#include <recorder_core/session_stats.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace {

using recorder_core::MeterCallback;
using recorder_core::MeterSnapshot;
using recorder_core::SessionState;
using recorder_core::SessionStatsCollector;
using recorder_core::StatsCallback;

// ---------------------------------------------------------------------------
// MeterSnapshot unit tests (no threading)
// ---------------------------------------------------------------------------

TEST(MeterSnapshotTest, DefaultIsAllZeros) {
    MeterSnapshot snap;
    for (float v : snap.per_track_rms) {
        EXPECT_FLOAT_EQ(v, 0.0f);
    }
}

TEST(MeterSnapshotTest, CanStoreAndReadAllTracks) {
    MeterSnapshot snap;
    snap.per_track_rms[0] = 0.8f;
    snap.per_track_rms[1] = 0.3f;
    snap.per_track_rms[2] = 0.6f;
    EXPECT_FLOAT_EQ(snap.per_track_rms[0], 0.8f);
    EXPECT_FLOAT_EQ(snap.per_track_rms[1], 0.3f);
    EXPECT_FLOAT_EQ(snap.per_track_rms[2], 0.6f);
}

// ---------------------------------------------------------------------------
// SessionStatsCollector meter callback tests
// ---------------------------------------------------------------------------

TEST(SessionStatsMeterCollectorTest, MeterCallback_InvokedWithCorrectRms) {
    SessionState state{};
    {
        std::lock_guard lk(state.stats_mutex);
        state.stats.per_track_rms[0] = 0.7f;
        state.stats.per_track_rms[2] = 0.4f;
    }

    std::atomic<bool> received{false};
    MeterSnapshot captured;
    state.meter_callback = [&](const MeterSnapshot& snap) {
        // Capture first invocation only
        if (!received.exchange(true)) {
            captured = snap;
        }
    };

    SessionStatsCollector collector(state);
    collector.Start();
    // 33 ms cadence; 100 ms gives ~3 ticks
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    collector.Stop();

    ASSERT_TRUE(received.load());
    EXPECT_FLOAT_EQ(captured.per_track_rms[0], 0.7f);
    EXPECT_FLOAT_EQ(captured.per_track_rms[1], 0.0f);
    EXPECT_FLOAT_EQ(captured.per_track_rms[2], 0.4f);
}

TEST(SessionStatsMeterCollectorTest, MeterCallback_FiresMoreFrequentlyThanStatsCallback) {
    SessionState state{};
    std::atomic<int> meter_count{0};
    std::atomic<int> stats_count{0};

    state.meter_callback = [&](const MeterSnapshot&) { ++meter_count; };
    state.stats_callback = [&](const recorder_core::SessionStats&) { ++stats_count; };

    SessionStatsCollector collector(state);
    collector.Start();
    // 8 meter ticks per stats tick; 300 ms → ~9 meter ticks, ~1 stats tick
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    collector.Stop();

    EXPECT_GT(meter_count.load(), stats_count.load());
    EXPECT_GE(meter_count.load(), 2);
}

TEST(SessionStatsMeterCollectorTest, MeterCallback_NullDoesNotCrash) {
    SessionState state{};
    state.meter_callback = nullptr;

    SessionStatsCollector collector(state);
    collector.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    collector.Stop();
    // Must complete without crash or hang
}

TEST(SessionStatsMeterCollectorTest, StatsCallback_StillFiredAtLowerCadence) {
    SessionState state{};
    std::atomic<int> stats_count{0};

    state.stats_callback = [&](const recorder_core::SessionStats&) { ++stats_count; };

    SessionStatsCollector collector(state);
    collector.Start();
    // 8 × 33 ms = 264 ms per stats tick; 600 ms gives headroom for Windows timer jitter
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    collector.Stop();

    EXPECT_GE(stats_count.load(), 1);
}

TEST(SessionStatsMeterCollectorTest, MeterCallback_ZeroRmsWhenNoAudio) {
    SessionState state{};
    // per_track_rms is default-initialized to zeros

    std::atomic<bool> received{false};
    MeterSnapshot captured;
    state.meter_callback = [&](const MeterSnapshot& snap) {
        if (!received.exchange(true)) {
            captured = snap;
        }
    };

    SessionStatsCollector collector(state);
    collector.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    collector.Stop();

    ASSERT_TRUE(received.load());
    for (float v : captured.per_track_rms) {
        EXPECT_FLOAT_EQ(v, 0.0f);
    }
}

} // namespace
