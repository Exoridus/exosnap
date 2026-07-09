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

// Sleeping for "long enough" and then asserting the tick happened is a race: under
// `ctest -j 16` the collector thread may not get the CPU inside a fixed window, and the
// test fails on healthy code. Wait for the condition instead. The generous ceiling only
// bounds a genuinely stuck collector; a healthy one satisfies the predicate in a tick.
template <typename Predicate>
bool WaitFor(Predicate pred, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

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
    const bool fired = WaitFor([&] { return received.load(); });
    collector.Stop();

    ASSERT_TRUE(fired) << "the meter callback never fired";
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
    // 8 meter ticks per stats tick, so one stats tick implies several meter ticks.
    const bool fired = WaitFor([&] { return stats_count.load() >= 1; });
    collector.Stop();

    ASSERT_TRUE(fired) << "the stats callback never fired";
    EXPECT_GT(meter_count.load(), stats_count.load());
    EXPECT_GE(meter_count.load(), 2);
}

TEST(SessionStatsMeterCollectorTest, MeterCallback_NullDoesNotCrash) {
    SessionState state{};
    state.meter_callback = nullptr;

    // A stats callback proves a tick actually ran. Without it this test could pass
    // having never reached the null meter callback at all.
    std::atomic<int> stats_count{0};
    state.stats_callback = [&](const recorder_core::SessionStats&) { ++stats_count; };

    SessionStatsCollector collector(state);
    collector.Start();
    const bool ticked = WaitFor([&] { return stats_count.load() >= 1; });
    collector.Stop();

    ASSERT_TRUE(ticked) << "no tick ran, so the null meter callback was never exercised";
}

TEST(SessionStatsMeterCollectorTest, StatsCallback_StillFiredAtLowerCadence) {
    SessionState state{};
    std::atomic<int> stats_count{0};

    state.stats_callback = [&](const recorder_core::SessionStats&) { ++stats_count; };

    SessionStatsCollector collector(state);
    collector.Start();
    const bool fired = WaitFor([&] { return stats_count.load() >= 1; });
    collector.Stop();

    EXPECT_TRUE(fired);
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
    const bool fired = WaitFor([&] { return received.load(); });
    collector.Stop();

    ASSERT_TRUE(fired) << "the meter callback never fired";
    for (float v : captured.per_track_rms) {
        EXPECT_FLOAT_EQ(v, 0.0f);
    }
}

} // namespace
