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
// The presentation debt below closes that. Publishes and presentations carry
// monotonic generations, and a frame counts as presented only when a render pass
// actually TOOK it off the transport. A pass that ran and lost the non-blocking
// acquire has shown nothing, so it leaves the debt standing — which is the whole
// difference between a preview that recovers by itself and one that waits for an
// unrelated redraw. Generations rather than a flag because the slot is
// last-value: a newer publish must subsume an older unpresented one without
// anything being replayed.
//
// Two things then settle the debt. A lifecycle transition (expose, screen
// change, rebuilt scene graph) re-issues exactly one update, one-shot per
// transition. And a pass that could not consume asks for a few more renders on a
// budget that only an actual publish or presentation refills. Neither is a timer
// and neither is a per-frame redraw: with no publish outstanding, both do
// nothing at all.
//
// Ownership/lifetime: held by std::shared_ptr so a capture pump thread or the
// engine's video thread can keep it alive without touching any QObject. It
// deliberately knows nothing about the item it eventually wakes.

#include <QtGlobal>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <vector>

namespace exosnap::quick {

// What one consume attempt did to the transport.
//
// Declared with the debt contract rather than with the consumer, because which
// outcome settles a generation is a property of the contract: the transport is a
// last-value slot, so a frame that was taken and then failed downstream is just
// as unavailable to any later render as one that was presented.
enum class PreviewConsumeOutcome {
    Missed,         // the acquire did not succeed; the frame is still in the slot
    TakenButFailed, // acquired and released, but the frame could not be converted
    Presented,      // acquired, converted, on screen
};

[[nodiscard]] constexpr bool SettlesPresentationDebt(PreviewConsumeOutcome outcome) noexcept {
    return outcome != PreviewConsumeOutcome::Missed;
}

class PreviewUpdateScheduler {
  public:
    // How many extra renders one publish may ask for after passes that could not
    // take it. Small on purpose: a contended acquire clears within a frame or
    // two, and the budget only refills on an actual publish or presentation, so
    // a transport that never yields cannot turn this into a spin.
    static constexpr quint64 kRetriesPerMiss = 3;

    PreviewUpdateScheduler() = default;

    PreviewUpdateScheduler(const PreviewUpdateScheduler&) = delete;
    PreviewUpdateScheduler& operator=(const PreviewUpdateScheduler&) = delete;

    // Producer side, any thread. True when the caller must deliver a wake-up.
    [[nodiscard]] bool ArmWake() noexcept {
        RecordPublishTiming();
        publish_signals_.fetch_add(1, std::memory_order_relaxed);
        // Monotonic, so a newer publish subsumes an older unpresented one: the
        // transport is a last-value slot and nothing is replayed. A single bool
        // could not express "presented generation 7 while 8 is already waiting".
        published_generation_.fetch_add(1, std::memory_order_acq_rel);
        retry_budget_.store(kRetriesPerMiss, std::memory_order_release);
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
    // the transport is acquired. Returns the generation this pass is about to
    // try for, which the caller hands back to NotePresented() if, and only if,
    // it actually took a frame off the transport.
    //
    // Read before the acquire, deliberately: a publish landing between this read
    // and a successful acquire is then acknowledged as the OLDER generation, so
    // its own debt survives and costs at most one redundant render later.
    // Reading afterwards would have the opposite error — acknowledging a frame
    // this pass never saw.
    [[nodiscard]] quint64 BeginRenderPass() noexcept {
        render_passes_.fetch_add(1, std::memory_order_relaxed);
        return published_generation_.load(std::memory_order_acquire);
    }

    // Render thread, after a render pass that actually consumed a frame.
    //
    // A render pass alone must NOT settle the debt. A pass that loses the
    // non-blocking acquire has shown nothing new, and treating it as a
    // presentation is what let a dropped scene-update request turn into a
    // multi-hundred-millisecond stall: the debt was cleared, the lifecycle
    // reissue then found nothing owed, and only an unrelated redraw eventually
    // moved the picture again.
    void NotePresented(quint64 generation) noexcept {
        RecordDebtSettled();
        quint64 presented = presented_generation_.load(std::memory_order_relaxed);
        while (presented < generation &&
               !presented_generation_.compare_exchange_weak(presented, generation, std::memory_order_acq_rel,
                                                            std::memory_order_relaxed)) {
        }
        retry_budget_.store(kRetriesPerMiss, std::memory_order_release);
    }

    // Render thread, after a pass that could not consume. True when the caller
    // should ask for one more render.
    //
    // Bounded twice over: only while a publish is genuinely still owed, and only
    // a few times before the budget runs out — which it can only refill by
    // actually presenting something. An unconditional re-request here is exactly
    // the display-refresh redraw loop this class was written to remove.
    [[nodiscard]] bool ShouldRetryAfterMiss() noexcept {
        if (!HasUnrenderedPublish())
            return false;
        quint64 budget = retry_budget_.load(std::memory_order_acquire);
        while (budget > 0) {
            if (retry_budget_.compare_exchange_weak(budget, budget - 1, std::memory_order_acq_rel,
                                                    std::memory_order_relaxed)) {
                retries_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    // Render thread, after the swap that ends a frame.
    //
    // A render pass and a completed swap are not the same event, and the
    // difference is the only way to tell a scene graph that never ran from one
    // that runs and never reaches the screen: an occluded or mode-changing
    // swap chain keeps rendering while presentation stalls. Counting the pass
    // alone cannot distinguish the two.
    void NoteFrameSwapped() noexcept {
        frame_swaps_.fetch_add(1, std::memory_order_relaxed);
    }

    // Any thread. True when the newest published frame has not been presented —
    // it is in the slot and the screen has not shown it.
    [[nodiscard]] bool HasUnrenderedPublish() const noexcept {
        return published_generation_.load(std::memory_order_acquire) >
               presented_generation_.load(std::memory_order_acquire);
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
    [[nodiscard]] quint64 FrameSwaps() const noexcept {
        return frame_swaps_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] quint64 MissRetries() const noexcept {
        return retries_.load(std::memory_order_relaxed);
    }

    // Milliseconds between SUCCESSFUL publishes. Says how fast the source was
    // actually feeding, which is the only thing that makes the consumer-side
    // intervals readable: a long arrival gap over a quiet desktop is the desktop
    // being quiet, not the preview stalling, and the two are indistinguishable
    // from the consumer's side alone.
    [[nodiscard]] std::vector<double> PublishIntervalsMs() const {
        return DrainMs(publish_interval_ns_, publish_interval_write_);
    }

    // Milliseconds a presentation debt stood open: from the FIRST publish after
    // the last successful consume, to that consume.
    //
    // Deliberately the oldest unpresented publish, not the newest. The transport
    // is a last-value slot, so with a producer publishing every ~33 ms the newest
    // waiting value is never more than one period old even while the picture has
    // been frozen for a second — measuring its age would report the stall as
    // healthy. What the eye sees is how long the preview owed a frame, and that
    // is this.
    [[nodiscard]] std::vector<double> PresentationDebtAgesMs() const {
        return DrainMs(debt_age_ns_, debt_age_write_);
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
        frame_swaps_.store(0, std::memory_order_relaxed);
        retries_.store(0, std::memory_order_relaxed);
        // The timing rings reset with the counters, so the producer and consumer
        // distributions in one report cover exactly the same window. They did
        // not, once, and the resulting cross-side arithmetic was nonsense.
        publish_interval_write_.store(0, std::memory_order_relaxed);
        debt_age_write_.store(0, std::memory_order_relaxed);
        for (auto& sample : publish_interval_ns_)
            sample.store(0, std::memory_order_relaxed);
        for (auto& sample : debt_age_ns_)
            sample.store(0, std::memory_order_relaxed);
        // NOT the debt clock: a publish already waiting when the window opens is
        // still waiting, and forgetting when it arrived would report the stall it
        // is part of as shorter than it was.
    }

  private:
    static constexpr std::size_t kTimingWindow = 1024;

    [[nodiscard]] static qint64 NowNs() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // Producer thread. Both timings are taken at the publish EDGE rather than
    // inside the producer's copy, so they carry the callback hop with them —
    // sub-millisecond, and the same on every sample, which is what matters for a
    // distribution.
    void RecordPublishTiming() noexcept {
        const qint64 now = NowNs();
        const qint64 previous = last_publish_ns_.exchange(now, std::memory_order_acq_rel);
        if (previous != 0)
            Store(publish_interval_ns_, publish_interval_write_, now - previous);
        // Only the FIRST publish of a debt stamps the clock; later ones leave it
        // alone so the age measures the whole outstanding stretch.
        qint64 expected = 0;
        debt_since_ns_.compare_exchange_strong(expected, now, std::memory_order_acq_rel, std::memory_order_relaxed);
    }

    // Render thread, on a consume that took a frame.
    void RecordDebtSettled() noexcept {
        const qint64 since = debt_since_ns_.exchange(0, std::memory_order_acq_rel);
        if (since != 0)
            Store(debt_age_ns_, debt_age_write_, NowNs() - since);
    }

    static void Store(std::array<std::atomic<qint64>, kTimingWindow>& ring, std::atomic<quint64>& write,
                      qint64 value) noexcept {
        const quint64 index = write.fetch_add(1, std::memory_order_relaxed);
        ring[index % kTimingWindow].store(value, std::memory_order_relaxed);
    }

    [[nodiscard]] static std::vector<double> DrainMs(const std::array<std::atomic<qint64>, kTimingWindow>& ring,
                                                     const std::atomic<quint64>& write) {
        const quint64 written = write.load(std::memory_order_relaxed);
        const std::size_t count = static_cast<std::size_t>(written < kTimingWindow ? written : kTimingWindow);
        std::vector<double> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const qint64 ns = ring[i].load(std::memory_order_relaxed);
            if (ns > 0)
                out.push_back(static_cast<double>(ns) / 1'000'000.0);
        }
        return out;
    }

    std::atomic<bool> wake_in_flight_{false};
    std::atomic<quint64> publish_signals_{0};
    std::atomic<quint64> coalesced_signals_{0};
    std::atomic<quint64> wakeups_{0};
    std::atomic<quint64> scene_update_requests_{0};
    std::atomic<quint64> published_generation_{0};
    std::atomic<quint64> presented_generation_{0};
    std::atomic<quint64> retry_budget_{0};
    std::atomic<quint64> retries_{0};
    std::atomic<quint64> render_passes_{0};
    std::atomic<quint64> frame_swaps_{0};

    std::atomic<qint64> last_publish_ns_{0};
    std::atomic<qint64> debt_since_ns_{0};
    std::atomic<quint64> publish_interval_write_{0};
    std::atomic<quint64> debt_age_write_{0};
    std::array<std::atomic<qint64>, kTimingWindow> publish_interval_ns_{};
    std::array<std::atomic<qint64>, kTimingWindow> debt_age_ns_{};
};

} // namespace exosnap::quick
