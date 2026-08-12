#pragma once

// The gate between "the producer published a preview frame" and "the Qt Quick
// scene graph renders one more time".
//
// Why it exists
// -------------
// The preview transport is a single shared D3D11 texture behind a keyed mutex
// (recorder_core/preview_shared_texture.h). It is a LAST-VALUE SLOT, not a
// queue, and it carries no readiness signal: a consumer can only find out that
// something arrived by trying a non-blocking AcquireSync. The first Qt Quick
// preview therefore re-requested a window render at the end of every rendered
// frame, which made the scene graph redraw the whole window at display refresh
// whether or not the desktop had changed. Measured on an idle desktop: 10 061
// window renders against 3 consumed source frames.
//
// The producers now emit an explicit per-frame edge instead, and this class is
// what turns that edge into at most one queued GUI-thread wake-up.
//
// Contract
// --------
//   producer thread : ArmWake()  -> true  => deliver a wake-up to the GUI thread
//                                  false => one is already in flight; do nothing
//   GUI thread      : DisarmWake() FIRST, then request the scene update
//
// DisarmWake() must run BEFORE the update is requested. That ordering is what
// makes the gate lossless:
//
//   * A publish whose ArmWake() returned true has its own wake-up, handled
//     after the publish, so a render follows it.
//   * A publish whose ArmWake() returned false was swallowed — but only because
//     an earlier wake-up had not been disarmed yet, and disarming is the first
//     thing its handler does. That handler therefore still runs after the
//     swallowed publish, and its update produces a render that sees it.
//
// Because the slot is last-value, one render after the newest publish is
// enough; nothing has to be replayed. Disarming AFTER the update request would
// invert this and genuinely lose frames.
//
// The cost of the ordering is a bounded redundancy: a publish that lands
// between the disarm and the render can produce one extra render that finds
// nothing to consume. One extra redraw, never an unbounded loop.
//
// What the gate alone does NOT cover
// ----------------------------------
// The contract above assumes that requesting a scene update eventually produces
// a render. That assumption fails while the window cannot render at all — it is
// unexposed, it is crossing a monitor boundary, its scene graph is being
// rebuilt. The request is dropped by the render loop, and the producer never
// re-offers it: the transport is a last-value slot, so the publish edge that
// wake-up belonged to is gone. The preview then sits on the last presented
// frame until some UNRELATED redraw (a hover, a resize) happens to run a render
// pass that consumes the newest frame — which is why the defect showed up as
// "the preview only moves again when the mouse moves".
//
// The render-pass counter below closes that. It records how many publishes had
// happened when the last render pass ran, so the GUI thread can ask on a
// lifecycle transition whether a publish is still owed a presentation, and
// re-issue exactly one update for it. That is a one-shot per transition, not a
// timer and not a per-frame redraw: with no publish outstanding it does nothing
// at all.
//
// Ownership/lifetime: held by std::shared_ptr so a capture pump thread or the
// engine's video thread can keep it alive without touching any QObject. It
// deliberately knows nothing about the item it eventually wakes.

#include <QtGlobal>

#include <atomic>

namespace exosnap::quick {

class PreviewUpdateScheduler {
  public:
    PreviewUpdateScheduler() = default;

    PreviewUpdateScheduler(const PreviewUpdateScheduler&) = delete;
    PreviewUpdateScheduler& operator=(const PreviewUpdateScheduler&) = delete;

    // Producer side, any thread. True when the caller must deliver a wake-up.
    [[nodiscard]] bool ArmWake() noexcept {
        publish_signals_.fetch_add(1, std::memory_order_relaxed);
        presentation_pending_.store(true, std::memory_order_release);
        if (wake_in_flight_.exchange(true, std::memory_order_acq_rel)) {
            coalesced_signals_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    // GUI thread, at the START of handling a wake-up — before requesting the
    // scene update. Runs even when the wake-up finds nothing to update, or the
    // gate stays armed forever and no further wake-up is ever delivered.
    void DisarmWake() noexcept {
        wake_in_flight_.store(false, std::memory_order_release);
        wakeups_.fetch_add(1, std::memory_order_relaxed);
    }

    // Number of scene updates actually requested. Lower than Wakeups() when a
    // wake-up arrived after the item or its adapter had gone.
    void RecordSceneUpdateRequested() noexcept {
        scene_update_requests_.fetch_add(1, std::memory_order_relaxed);
    }

    // Render thread, at the START of a render pass that can consume — before
    // the transport is acquired.
    //
    // Before, deliberately. A publish landing between this clear and the
    // acquire re-raises the debt even though this pass may well have carried
    // it, which costs at most one redundant render later. Clearing afterwards
    // would have the opposite error: a publish the pass missed would be
    // recorded as presented and would never be asked for again.
    void NoteRenderPass() noexcept {
        presentation_pending_.store(false, std::memory_order_release);
        render_passes_.fetch_add(1, std::memory_order_relaxed);
    }

    // GUI thread. True when a publish has not yet been followed by a render
    // pass — the newest frame is in the slot and the screen has not shown it.
    [[nodiscard]] bool HasUnrenderedPublish() const noexcept {
        return presentation_pending_.load(std::memory_order_acquire);
    }

    [[nodiscard]] quint64 PublishSignals() const noexcept {
        return publish_signals_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] quint64 CoalescedSignals() const noexcept {
        return coalesced_signals_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] quint64 Wakeups() const noexcept {
        return wakeups_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] quint64 SceneUpdateRequests() const noexcept {
        return scene_update_requests_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] quint64 RenderPasses() const noexcept {
        return render_passes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool WakeInFlightForTest() const noexcept {
        return wake_in_flight_.load(std::memory_order_acquire);
    }

    // Counters only. Neither the gate nor the presentation debt is reset: a
    // benchmark window opening must not drop a wake-up that is already on the
    // queue, nor forget a frame that is still owed a render.
    void ResetCounters() noexcept {
        publish_signals_.store(0, std::memory_order_relaxed);
        coalesced_signals_.store(0, std::memory_order_relaxed);
        wakeups_.store(0, std::memory_order_relaxed);
        scene_update_requests_.store(0, std::memory_order_relaxed);
        render_passes_.store(0, std::memory_order_relaxed);
    }

  private:
    std::atomic<bool> wake_in_flight_{false};
    std::atomic<quint64> publish_signals_{0};
    std::atomic<quint64> coalesced_signals_{0};
    std::atomic<quint64> wakeups_{0};
    std::atomic<quint64> scene_update_requests_{0};
    std::atomic<bool> presentation_pending_{false};
    std::atomic<quint64> render_passes_{0};
};

} // namespace exosnap::quick
