// Mid-recording audio-source degradation (ADR 0046, pure): the latch that decides
// WHEN the standing "audio source went silent" notice is raised, replaced and
// cleared from the AudioDiagnostics health facts the pipeline already publishes.
//
// The detection itself is not under test here — that lives in the engine
// (DegradedSourceCount -> OnAudioSourceHealth -> PipelineDiagnosticsAggregator)
// and has its own tests. What is pinned here is the user-facing contract that
// was lost in the Qt Quick cutover: exactly one notice per outage, no per-tick
// spam, an honest clear on recovery, and no latch crossing a session boundary.
//
// No test in this file sleeps; every "tick" is just another Observe() call.

#include "diagnostics/AudioSourceDegradation.h"

#include <gtest/gtest.h>

using namespace exosnap::diagnostics;
using recorder_core::DiagnosticsLifecycle;

namespace {

// A live recording snapshot with `degraded` sources currently lost.
AudioDegradationSample recordingSample(uint64_t generation, uint32_t degraded) {
    AudioDegradationSample s;
    s.session_generation = generation;
    s.valid = true;
    s.lifecycle = DiagnosticsLifecycle::Recording;
    s.degraded_sources = degraded;
    s.source_degraded = degraded > 0;
    return s;
}

AudioDegradationSample lifecycleSample(uint64_t generation, DiagnosticsLifecycle lifecycle, uint32_t degraded) {
    AudioDegradationSample s = recordingSample(generation, degraded);
    s.lifecycle = lifecycle;
    return s;
}

} // namespace

// ── Observability ────────────────────────────────────────────────────────────

TEST(AudioDegradationObservableTest, OnlyALiveOrPausedSessionReportsHealth) {
    EXPECT_TRUE(AudioDegradationObservable(DiagnosticsLifecycle::Recording));
    // The audio threads keep the endpoints open across a pause, so a device lost
    // while paused is a real outage the user hits on resume.
    EXPECT_TRUE(AudioDegradationObservable(DiagnosticsLifecycle::Paused));

    EXPECT_FALSE(AudioDegradationObservable(DiagnosticsLifecycle::Idle));
    EXPECT_FALSE(AudioDegradationObservable(DiagnosticsLifecycle::Initializing));
    EXPECT_FALSE(AudioDegradationObservable(DiagnosticsLifecycle::Stopping));
    EXPECT_FALSE(AudioDegradationObservable(DiagnosticsLifecycle::Completed));
    EXPECT_FALSE(AudioDegradationObservable(DiagnosticsLifecycle::Failed));
}

// ── Healthy ──────────────────────────────────────────────────────────────────

TEST(AudioSourceDegradationMonitorTest, HealthyRecording_NeverSignals) {
    AudioSourceDegradationMonitor monitor;
    for (int tick = 0; tick < 200; ++tick) {
        EXPECT_EQ(monitor.Observe(recordingSample(7, 0)), AudioDegradationSignal::None) << "tick " << tick;
    }
    EXPECT_EQ(monitor.degraded_sources(), 0u);
    EXPECT_EQ(monitor.reported_episodes(), 0u);
}

TEST(AudioSourceDegradationMonitorTest, InvalidSnapshotNeverRaises) {
    AudioSourceDegradationMonitor monitor;
    AudioDegradationSample s = recordingSample(1, 2);
    s.valid = false; // idle / no data — not a measurement of health
    EXPECT_EQ(monitor.Observe(s), AudioDegradationSignal::None);
    EXPECT_EQ(monitor.degraded_sources(), 0u);
}

// An inconsistent snapshot (the flag set, the count zero) must not raise a notice
// about no sources at all. The count is what the body says.
TEST(AudioSourceDegradationMonitorTest, DegradedFlagWithZeroCountReadsAsHealthy) {
    AudioSourceDegradationMonitor monitor;
    AudioDegradationSample s = recordingSample(1, 0);
    s.source_degraded = true;
    EXPECT_EQ(monitor.Observe(s), AudioDegradationSignal::None);
    EXPECT_EQ(monitor.degraded_sources(), 0u);
}

// ── Degraded transition ──────────────────────────────────────────────────────

TEST(AudioSourceDegradationMonitorTest, FirstDegradedSource_RaisesExactlyOnce) {
    AudioSourceDegradationMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 0)), AudioDegradationSignal::None);

    EXPECT_EQ(monitor.Observe(recordingSample(1, 1)), AudioDegradationSignal::Raise);
    EXPECT_EQ(monitor.degraded_sources(), 1u);
    EXPECT_EQ(monitor.reported_episodes(), 1u);
}

// A recording that is already degraded on its very first snapshot still gets its
// notice — the latch has no "warm-up" tick.
TEST(AudioSourceDegradationMonitorTest, DegradedOnTheFirstSnapshot_Raises) {
    AudioSourceDegradationMonitor monitor;
    EXPECT_EQ(monitor.Observe(recordingSample(1, 1)), AudioDegradationSignal::Raise);
    EXPECT_EQ(monitor.degraded_sources(), 1u);
}

// ── Continued degraded: no spam ──────────────────────────────────────────────

TEST(AudioSourceDegradationMonitorTest, ContinuedDegraded_NeverReAnnounces) {
    AudioSourceDegradationMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 1)), AudioDegradationSignal::Raise);

    // ~40 seconds at the 5 Hz diagnostics cadence.
    for (int tick = 0; tick < 200; ++tick) {
        EXPECT_EQ(monitor.Observe(recordingSample(1, 1)), AudioDegradationSignal::None) << "tick " << tick;
    }
    EXPECT_EQ(monitor.reported_episodes(), 1u) << "the outage never ended, so it is still one episode";
}

// ── Multiple sources: the count is the identity of the degraded SET ──────────

TEST(AudioSourceDegradationMonitorTest, DegradedSetGrows_RaisesAgainWithTheNewCount) {
    AudioSourceDegradationMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 1)), AudioDegradationSignal::Raise);
    ASSERT_EQ(monitor.degraded_sources(), 1u);

    // A second endpoint goes down while the first is still out: the caller
    // replaces the standing notice in place with the new body.
    EXPECT_EQ(monitor.Observe(recordingSample(1, 2)), AudioDegradationSignal::Raise);
    EXPECT_EQ(monitor.degraded_sources(), 2u);
    EXPECT_EQ(monitor.reported_episodes(), 1u) << "still the same outage, not a second episode";
}

TEST(AudioSourceDegradationMonitorTest, DegradedSetShrinksButIsNotEmpty_StillOneStandingNotice) {
    AudioSourceDegradationMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 3)), AudioDegradationSignal::Raise);

    // Two of the three came back. The notice must NOT clear — one source is still
    // silent — and must not claim three either.
    EXPECT_EQ(monitor.Observe(recordingSample(1, 1)), AudioDegradationSignal::Raise);
    EXPECT_EQ(monitor.degraded_sources(), 1u);
    EXPECT_EQ(monitor.reported_episodes(), 1u);
}

// ── Recovery ─────────────────────────────────────────────────────────────────

TEST(AudioSourceDegradationMonitorTest, EverySourceRecovers_ClearsExactlyOnce) {
    AudioSourceDegradationMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 2)), AudioDegradationSignal::Raise);

    EXPECT_EQ(monitor.Observe(recordingSample(1, 0)), AudioDegradationSignal::Clear);
    EXPECT_EQ(monitor.degraded_sources(), 0u);

    for (int tick = 0; tick < 50; ++tick) {
        EXPECT_EQ(monitor.Observe(recordingSample(1, 0)), AudioDegradationSignal::None) << "tick " << tick;
    }
}

// product-spec: "clears the moment every source reactivates OR the recording
// ends". The terminal snapshot carries the outage's last count; it must clear.
TEST(AudioSourceDegradationMonitorTest, SessionEndsWhileDegraded_Clears) {
    AudioSourceDegradationMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(4, 1)), AudioDegradationSignal::Raise);

    EXPECT_EQ(monitor.Observe(lifecycleSample(4, DiagnosticsLifecycle::Completed, 1)), AudioDegradationSignal::Clear);
    EXPECT_EQ(monitor.degraded_sources(), 0u);
}

TEST(AudioSourceDegradationMonitorTest, PausedWhileDegraded_KeepsTheNoticeStanding) {
    AudioSourceDegradationMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(4, 1)), AudioDegradationSignal::Raise);

    EXPECT_EQ(monitor.Observe(lifecycleSample(4, DiagnosticsLifecycle::Paused, 1)), AudioDegradationSignal::None);
    EXPECT_EQ(monitor.degraded_sources(), 1u);
}

// ── Second episode ───────────────────────────────────────────────────────────

TEST(AudioSourceDegradationMonitorTest, SecondOutageInTheSameSession_RaisesAgain) {
    AudioSourceDegradationMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 1)), AudioDegradationSignal::Raise);
    ASSERT_EQ(monitor.Observe(recordingSample(1, 0)), AudioDegradationSignal::Clear);
    ASSERT_EQ(monitor.reported_episodes(), 1u);

    // The same mic is unplugged a second time. A latch that never re-armed would
    // leave the user uninformed for the rest of the recording.
    EXPECT_EQ(monitor.Observe(recordingSample(1, 1)), AudioDegradationSignal::Raise);
    EXPECT_EQ(monitor.reported_episodes(), 2u);
}

// ── Session lifecycle ────────────────────────────────────────────────────────

TEST(AudioSourceDegradationMonitorTest, NewSessionDoesNotInheritTheDegradedLatch) {
    AudioSourceDegradationMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 2)), AudioDegradationSignal::Raise);

    // Generation 2 is a different recording that happens to have the same degraded
    // count. Without the reset this would read as "unchanged" and the new session
    // would never get its own notice.
    EXPECT_EQ(monitor.Observe(recordingSample(2, 2)), AudioDegradationSignal::Raise);
    EXPECT_EQ(monitor.reported_episodes(), 1u) << "episodes are per session";
}

TEST(AudioSourceDegradationMonitorTest, NewHealthySessionAfterADegradedOne_SignalsNothing) {
    AudioSourceDegradationMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 1)), AudioDegradationSignal::Raise);

    // The caller clears its own toast on the recording-start edge; the monitor
    // simply must not emit a stray Clear/Raise into the new session.
    EXPECT_EQ(monitor.Observe(recordingSample(2, 0)), AudioDegradationSignal::None);
    EXPECT_EQ(monitor.degraded_sources(), 0u);
    EXPECT_EQ(monitor.reported_episodes(), 0u);
}

TEST(AudioSourceDegradationMonitorTest, StaleSnapshotFromAFinishedSessionIsIgnored) {
    AudioSourceDegradationMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(1, 1)), AudioDegradationSignal::Raise);
    ASSERT_EQ(monitor.Observe(recordingSample(2, 0)), AudioDegradationSignal::None); // new session, healthy

    // A late callback from generation 1 must not raise a notice about a recording
    // that is over.
    EXPECT_EQ(monitor.Observe(recordingSample(1, 3)), AudioDegradationSignal::None);
    EXPECT_EQ(monitor.degraded_sources(), 0u);
}

TEST(AudioSourceDegradationMonitorTest, StaleSnapshotCannotClearTheCurrentSessionsNotice) {
    AudioSourceDegradationMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(5, 1)), AudioDegradationSignal::Raise);
    ASSERT_EQ(monitor.Observe(recordingSample(6, 1)), AudioDegradationSignal::Raise); // new session, also degraded

    EXPECT_EQ(monitor.Observe(recordingSample(5, 0)), AudioDegradationSignal::None);
    EXPECT_EQ(monitor.degraded_sources(), 1u) << "the running session is still degraded";
}

// Reset() is what the composition root calls on a fresh recording start. It drops
// the latch, but must not forget which generations are already over — otherwise a
// late snapshot from the previous session would re-arm the latch it just cleared.
TEST(AudioSourceDegradationMonitorTest, ResetDropsTheLatchButKeepsTheGenerationHighWaterMark) {
    AudioSourceDegradationMonitor monitor;
    ASSERT_EQ(monitor.Observe(recordingSample(9, 2)), AudioDegradationSignal::Raise);

    monitor.Reset();
    EXPECT_EQ(monitor.degraded_sources(), 0u);
    EXPECT_EQ(monitor.reported_episodes(), 0u);

    EXPECT_EQ(monitor.Observe(recordingSample(8, 2)), AudioDegradationSignal::None) << "generation 8 is history";
}
