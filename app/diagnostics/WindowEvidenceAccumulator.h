#pragma once

// The pure evidence-accumulation core behind WindowEvidenceProbe (S2a). Fed one
// sample per pump — the current hub frame kind, its generation counter, and the
// window's shape — it maintains the timing facts the severity ladder needs:
//   * how long the probe has been subscribed,
//   * how long since a genuinely new frame arrived,
//   * whether any fresh frame arrived AFTER the window became FullscreenShaped
//     (the correlation that separates an FSE freeze from a legitimately static
//     borderless window that kept producing frames).
//
// It is deliberately free of Qt, COM, D3D and the wall clock (time is passed in),
// so the accumulation is unit-pinned without a GPU. WindowEvidenceProbe owns the
// COM worker; this owns the reasoning.

#include <chrono>
#include <cstdint>

#include "WindowTargetFacts.h"

namespace exosnap::diagnostics {

class WindowEvidenceAccumulator {
  public:
    using TimePoint = std::chrono::steady_clock::time_point;

    // Begin a fresh subscription at `now`; forgets all prior history.
    void Reset(TimePoint now) noexcept;

    // One pump sample. A generation that differs from the last non-zero one is a
    // genuinely new frame. `shape` is the window's current shape (from the ~1 Hz
    // fact poll); the transition Normal -> FullscreenShaped arms the correlation.
    void Update(TimePoint now, recorder_core::HubFrameKind kind, uint64_t generation, WindowShape shape) noexcept;

    // The evidence snapshot as of `now` (which may be later than the last Update).
    [[nodiscard]] WindowHubEvidence Evidence(TimePoint now) const noexcept;

  private:
    bool started_ = false;
    TimePoint subscribed_at_{};
    recorder_core::HubFrameKind last_kind_ = recorder_core::HubFrameKind::None;
    uint64_t last_generation_ = 0;
    bool had_fresh_frame_ = false;
    TimePoint last_fresh_frame_{};
    bool fullscreen_shape_ = false;  // window is currently FullscreenShaped
    bool fresh_since_shape_ = false; // a fresh frame arrived since entering that shape
};

} // namespace exosnap::diagnostics
