#pragma once

// A single WGC subscription on the currently-selected window capture target,
// used pre-flight to produce honest exclusive-fullscreen evidence (S2a). The
// existing hub consumers cannot serve this: the picker registry dies with the
// panel, DxgiCaptureHubService only knows monitors, and the record-page window
// preview runs past the registry model. So this is its own small plumbing: a
// dedicated STA-COM worker thread, its own CaptureHubRegistry, exactly one
// subscription, and a ~1 Hz WindowTargetFacts poll feeding the pure
// WindowEvidenceAccumulator.
//
// It exposes only a thread-safe Snapshot (target + facts + evidence). It makes
// no judgement — diagnostics::ResolveExclusiveEvidence turns the snapshot into a
// verdict, and both consumers (the Diagnostics card and the recording-admission
// gate) go through that one resolver.
//
// Threading contract, and the reason the snapshot carries its own hwnd: the
// snapshot is produced on the worker and copied out under a mutex, so any thread
// may read it without touching COM, D3D or GUI state. `hwnd` identifies the
// target the evidence belongs to, so a reader that has already retargeted can
// reject a snapshot describing the previous window instead of judging the new
// target by the old one's evidence.

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "diagnostics/WindowEvidenceAccumulator.h"
#include "diagnostics/WindowEvidenceSnapshot.h"
#include "diagnostics/WindowTargetFacts.h"

namespace exosnap {

class WindowEvidenceProbe {
  public:
    WindowEvidenceProbe();
    ~WindowEvidenceProbe();

    WindowEvidenceProbe(const WindowEvidenceProbe&) = delete;
    WindowEvidenceProbe& operator=(const WindowEvidenceProbe&) = delete;

    // Select the window to probe (HWND as uintptr). 0 unsubscribes (a monitor
    // target or no selection). Cheap and non-blocking; the worker applies it.
    void SetWindowTarget(uintptr_t hwnd);

    // Suspend / resume pumping while the recording engine owns the capture. The
    // subscription is retained; only the double capture load is avoided.
    void SetPaused(bool paused);

    // The published state. Its type and the resolver that reads it live in
    // diagnostics/WindowEvidenceSnapshot.h, so the "which target does this
    // evidence describe" contract is unit-pinned without WGC, D3D or a GPU.
    using Snapshot = diagnostics::WindowEvidenceSnapshot;
    [[nodiscard]] Snapshot CurrentSnapshot() const;

  private:
    void WorkerMain(std::stop_token stop_token);

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // Command state (guarded by mutex_).
    uintptr_t pending_hwnd_ = 0;
    bool pending_dirty_ = false;
    bool paused_ = false;

    Snapshot snapshot_; // guarded by mutex_

    std::jthread worker_;
};

} // namespace exosnap
