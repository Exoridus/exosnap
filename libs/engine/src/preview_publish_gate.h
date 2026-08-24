#pragma once

// Pure throttle state machine deciding when a freshly composed video frame
// should be surfaced to the live WYSIWYG preview callback (Strand 3 slice 1).
// No I/O, no D3D11, no wall clock reads -- a plain state machine over
// caller-supplied monotonic timestamps, so throttle behavior is
// deterministically unit-testable without a real clock or GPU.
//
// Callers must invoke ShouldPublish only for frames that were actually newly
// composed (never for CFR-duplicated/skipped ticks); the gate does not (and
// cannot) verify that -- it is enforced by call placement in the encode loop.

#include <cstdint>

namespace exosnap::engine {

// Default minimum publish spacing for the ~30 Hz preview target.
//
// Deliberately 30 ms, NOT 33.33 ms: CFR frame PTS values are derived from a
// TRUNCATED frame interval ((10'000'000 / fps) * 100 -- e.g. 16'666'600 ns at
// 60 fps, 33'333'300 ns at 30 fps), so two 60 fps frames span 33'333'200 ns.
// A naive 33'333'333 ns threshold rejects that span and halves the effective
// preview rate to ~20 Hz (and 30 fps sessions to ~15 Hz). Setting the gate
// threshold below the target interval with ~10% hysteresis accepts the
// truncated spans while still keeping the publish rate at or under ~33 Hz
// for any frame rate.
inline constexpr uint64_t kPreviewMinIntervalNs = 30'000'000ULL; // 30 ms

class PreviewPublishGate {
  public:
    explicit PreviewPublishGate(uint64_t min_interval_ns) noexcept : m_min_interval_ns(min_interval_ns) {
    }

    // now_ns: monotonic timestamp of this candidate frame (any epoch, as long
    // as it is non-decreasing across calls for a given recording).
    // Returns true exactly when the frame should be published. On true, the
    // internal "last published" timestamp advances to now_ns.
    [[nodiscard]] bool ShouldPublish(uint64_t now_ns) noexcept {
        if (m_has_published) {
            if (now_ns < m_last_published_ns)
                return false; // non-monotonic input -- ignore rather than underflow
            if ((now_ns - m_last_published_ns) < m_min_interval_ns)
                return false;
        }
        m_last_published_ns = now_ns;
        m_has_published = true;
        return true;
    }

    // Resets the throttle so the next eligible frame publishes immediately.
    // Call when a new Record() session starts.
    void Reset() noexcept {
        m_has_published = false;
        m_last_published_ns = 0;
    }

  private:
    uint64_t m_min_interval_ns;
    bool m_has_published = false;
    uint64_t m_last_published_ns = 0;
};

} // namespace exosnap::engine
