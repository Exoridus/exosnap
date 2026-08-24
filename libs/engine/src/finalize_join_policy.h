#pragma once

// Two-phase recording-shutdown policy, factored out of RecorderSession so it can
// be unit tested without capture hardware, threads, or a wall clock (time and
// bytes are parameters).
//
// Background — the false-timeout defect this fixes:
//   On stop, every worker (video, audio, AND the mux thread) shared ONE fixed
//   120 s join budget. But the mux thread does more than drain a queue: it runs
//   finalize (Cues -> Render, back-patch Duration/SeekHead/Segment size), which
//   is O(keyframes/duration) and disk-bound. On a long recording finalising to a
//   NAS, finalize can legitimately run past 120 s while writing bytes the whole
//   time — yet it was reported as a hung worker (m=TIMEOUT) and the file left
//   effectively unfinalised. Locally (1-2 h) finalize is sub-second, which is why
//   the defect was rarely seen.
//
// The fix splits shutdown into two phases with different waiting policies:
//   Phase 1 — join the PRODUCER workers (video/audio) under a short budget. They
//             only have to flush their encoders (already bounded) and exit; if
//             one does not, that is the real "worker hangs" fault.
//   Phase 2 — wait for the mux thread's finalize under a PROGRESS-based policy:
//             keep waiting as long as bytes are still being written, and abort
//             only on a genuine stall (no byte progress for a whole stall window)
//             or an optional hard cap. Not a fixed large timeout.
//
// The two faults are reported distinctly (see ClassifyShutdownFault) so a hung
// producer is never conflated with a slow-but-working finalize.

#include <cstdint>

namespace exosnap::engine {

// Decision emitted by FinalizeProgressTracker for one observation of the finalize
// byte count.
enum class FinalizeWaitDecision {
    KeepWaiting,  // finalize is still progressing (or within the stall window) — wait
    StalledAbort, // no byte progress for a whole stall window (or hard cap hit) — give up
};

// Progress-based finalize wait. Feed it the cumulative finalize byte count at a
// monotonic timestamp (both supplied by the caller — no wall clock inside) and it
// decides whether the finalize is still making progress.
//
// Semantics:
//   * The first Observe() seeds the baseline and returns KeepWaiting.
//   * An observation whose byte count is strictly greater than the PREVIOUS
//     observation is progress: it resets the stall timer.
//   * An equal count is no progress. A LOWER count (e.g. a fresh per-segment
//     writer resetting bytes_written) is not progress either, but the baseline is
//     rebased to it so that subsequent growth above the reset low counts as
//     progress again.
//   * If no progress is seen for a whole stall_window_ms, the decision is
//     StalledAbort.
//   * If hard_cap_ms > 0 and the total elapsed since the first observation reaches
//     it, the decision is StalledAbort even while progressing (a safety valve).
//     hard_cap_ms == 0 disables the cap — the wait is then purely progress-based.
class FinalizeProgressTracker {
  public:
    explicit FinalizeProgressTracker(uint64_t stall_window_ms, uint64_t hard_cap_ms = 0) noexcept
        : m_stall_window_ms(stall_window_ms), m_hard_cap_ms(hard_cap_ms) {
    }

    FinalizeWaitDecision Observe(uint64_t bytes_now, uint64_t now_ms) noexcept {
        if (!m_seeded) {
            m_seeded = true;
            m_start_ms = now_ms;
            m_last_bytes = bytes_now;
            m_last_progress_ms = now_ms;
            return FinalizeWaitDecision::KeepWaiting;
        }

        if (bytes_now > m_last_bytes) {
            m_last_progress_ms = now_ms; // growth since the previous sample — still working
        }
        // Rebase in both directions: growth advances the mark; a drop (writer reset)
        // rebases to the new low so later growth above it is recognised as progress.
        m_last_bytes = bytes_now;

        if (m_hard_cap_ms > 0 && ElapsedSince(m_start_ms, now_ms) >= m_hard_cap_ms) {
            return FinalizeWaitDecision::StalledAbort;
        }

        if (ElapsedSince(m_last_progress_ms, now_ms) >= m_stall_window_ms) {
            return FinalizeWaitDecision::StalledAbort;
        }
        return FinalizeWaitDecision::KeepWaiting;
    }

    [[nodiscard]] uint64_t last_bytes() const noexcept {
        return m_last_bytes;
    }
    [[nodiscard]] uint64_t last_progress_ms() const noexcept {
        return m_last_progress_ms;
    }

  private:
    // Monotonic-time subtraction guarded against a non-monotonic caller (never
    // underflows to a huge unsigned value).
    static uint64_t ElapsedSince(uint64_t earlier_ms, uint64_t now_ms) noexcept {
        return now_ms > earlier_ms ? (now_ms - earlier_ms) : 0;
    }

    uint64_t m_stall_window_ms;
    uint64_t m_hard_cap_ms;
    bool m_seeded = false;
    uint64_t m_start_ms = 0;
    uint64_t m_last_progress_ms = 0;
    uint64_t m_last_bytes = 0;
};

// Which shutdown fault (if any) to report, keeping the two phases distinct.
enum class ShutdownFault {
    None,            // both phases completed cleanly
    WorkerHang,      // Phase 1: a producer (video/audio) failed to flush and join in time
    FinalizeStalled, // Phase 2: finalize made no byte progress and was abandoned
};

// Producer-hang dominates: if a producer never joined, that is the fault to
// surface regardless of the finalize outcome (a producer that never emitted EOS
// can itself be why finalize could not complete). Only when producers joined
// cleanly does a failed finalize get reported as a distinct stall.
inline ShutdownFault ClassifyShutdownFault(bool producers_joined, bool finalize_completed) noexcept {
    if (!producers_joined) {
        return ShutdownFault::WorkerHang;
    }
    if (!finalize_completed) {
        return ShutdownFault::FinalizeStalled;
    }
    return ShutdownFault::None;
}

} // namespace exosnap::engine
