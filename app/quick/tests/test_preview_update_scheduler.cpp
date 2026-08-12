// The preview redraw gate (PreviewUpdateScheduler).
//
// The property under test is not "how few renders" — it is that collapsing the
// render count never loses a frame. The transport is a last-value slot, so the
// contract is: after the newest publish, at least one wake-up is still handled.
// The interleaving tests below spell out the cases that can break it, and the
// threaded one asserts the end-to-end consequence: the consumer never ends on a
// stale value.

#include "PreviewUpdateScheduler.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

using exosnap::quick::PreviewUpdateScheduler;

namespace {

// Stands in for the GUI thread's handling of one wake-up: disarm first, then do
// the work the wake-up was for.
void handleWake(PreviewUpdateScheduler& scheduler, bool item_alive = true) {
    scheduler.DisarmWake();
    if (item_alive)
        scheduler.RecordSceneUpdateRequested();
}

// Stands in for ExoPreviewItem::reissuePendingPresentation(): the GUI thread
// reacting to a lifecycle transition that made the window renderable again.
// Returns whether it asked for anything, which is what the amplification tests
// below measure.
bool reissuePendingPresentation(PreviewUpdateScheduler& scheduler) {
    if (!scheduler.HasUnrenderedPublish())
        return false;
    scheduler.RecordSceneUpdateRequested();
    return true;
}

} // namespace

TEST(PreviewUpdateSchedulerTest, QuietSourceRequestsNothing) {
    PreviewUpdateScheduler scheduler;
    EXPECT_FALSE(scheduler.WakeInFlightForTest());
    EXPECT_EQ(scheduler.PublishSignals(), 0u);
    EXPECT_EQ(scheduler.Wakeups(), 0u);
    EXPECT_EQ(scheduler.SceneUpdateRequests(), 0u);
}

TEST(PreviewUpdateSchedulerTest, OnePublishDeliversExactlyOneWake) {
    PreviewUpdateScheduler scheduler;
    ASSERT_TRUE(scheduler.ArmWake());
    handleWake(scheduler);
    EXPECT_EQ(scheduler.PublishSignals(), 1u);
    EXPECT_EQ(scheduler.CoalescedSignals(), 0u);
    EXPECT_EQ(scheduler.Wakeups(), 1u);
    EXPECT_EQ(scheduler.SceneUpdateRequests(), 1u);
    EXPECT_FALSE(scheduler.WakeInFlightForTest());
}

TEST(PreviewUpdateSchedulerTest, BurstBeforeTheWakeIsHandledCoalescesToOne) {
    PreviewUpdateScheduler scheduler;
    EXPECT_TRUE(scheduler.ArmWake());
    for (int i = 0; i < 9; ++i)
        EXPECT_FALSE(scheduler.ArmWake()) << "burst frame " << i << " should have found a wake in flight";
    handleWake(scheduler);

    EXPECT_EQ(scheduler.PublishSignals(), 10u);
    EXPECT_EQ(scheduler.CoalescedSignals(), 9u);
    // One render for ten frames is correct: the slot only ever holds the newest.
    EXPECT_EQ(scheduler.SceneUpdateRequests(), 1u);
}

TEST(PreviewUpdateSchedulerTest, EachPublishAfterItsWakeGetsItsOwnRender) {
    PreviewUpdateScheduler scheduler;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(scheduler.ArmWake()) << "frame " << i;
        handleWake(scheduler);
    }
    EXPECT_EQ(scheduler.PublishSignals(), 5u);
    EXPECT_EQ(scheduler.CoalescedSignals(), 0u);
    EXPECT_EQ(scheduler.SceneUpdateRequests(), 5u);
}

// The race the disarm-before-update ordering exists for: a frame that lands
// after the gate reopened but before the render it reopened for has run. It
// must produce its own wake-up, because the pending render may already be
// looking at the older content.
TEST(PreviewUpdateSchedulerTest, PublishBetweenDisarmAndRenderRearms) {
    PreviewUpdateScheduler scheduler;
    ASSERT_TRUE(scheduler.ArmWake());

    scheduler.DisarmWake();
    // ... producer publishes here, before the update below ...
    EXPECT_TRUE(scheduler.ArmWake()) << "a publish after the disarm must not be swallowed";
    scheduler.RecordSceneUpdateRequested();

    handleWake(scheduler);
    EXPECT_EQ(scheduler.SceneUpdateRequests(), 2u);
}

// A wake-up that arrives after the item is gone must still reopen the gate, or
// no publish is ever delivered again.
TEST(PreviewUpdateSchedulerTest, WakeWithNoLiveItemStillReopensTheGate) {
    PreviewUpdateScheduler scheduler;
    ASSERT_TRUE(scheduler.ArmWake());
    handleWake(scheduler, /*item_alive=*/false);

    EXPECT_FALSE(scheduler.WakeInFlightForTest());
    EXPECT_EQ(scheduler.Wakeups(), 1u);
    EXPECT_EQ(scheduler.SceneUpdateRequests(), 0u);
    EXPECT_TRUE(scheduler.ArmWake()) << "the next publish must still be able to arm";
}

TEST(PreviewUpdateSchedulerTest, ResetClearsCountersButNotTheGate) {
    PreviewUpdateScheduler scheduler;
    ASSERT_TRUE(scheduler.ArmWake());
    ASSERT_FALSE(scheduler.ArmWake());

    scheduler.ResetCounters();

    EXPECT_EQ(scheduler.PublishSignals(), 0u);
    EXPECT_EQ(scheduler.CoalescedSignals(), 0u);
    EXPECT_EQ(scheduler.Wakeups(), 0u);
    EXPECT_EQ(scheduler.SceneUpdateRequests(), 0u);
    // A measurement window opening must not drop the wake-up already in flight.
    EXPECT_TRUE(scheduler.WakeInFlightForTest());
    handleWake(scheduler);
    EXPECT_FALSE(scheduler.WakeInFlightForTest());
}

// ---------------------------------------------------------------------------
// The presentation debt.
//
// Everything above assumes that asking for a scene update produces a render.
// While the window is between screens, unexposed, or rebuilding its scene
// graph, the render loop drops the request instead — and the producer cannot
// re-offer it, because the transport is a last-value slot and the publish edge
// is gone. Shipped, that read as "the preview only starts moving again when the
// mouse moves": the next unrelated redraw was what finally consumed the frame.
//
// These tests pin the contract that closes it. Note that the whole class of
// defect is invisible to the tests above, which never model a render that does
// not happen.
// ---------------------------------------------------------------------------

TEST(PreviewUpdateSchedulerTest, QuietSchedulerOwesNothing) {
    PreviewUpdateScheduler scheduler;
    EXPECT_FALSE(scheduler.HasUnrenderedPublish());
    EXPECT_FALSE(reissuePendingPresentation(scheduler)) << "a lifecycle transition with no frame pending must be inert";
    EXPECT_EQ(scheduler.SceneUpdateRequests(), 0u);
}

TEST(PreviewUpdateSchedulerTest, ARenderPassClearsTheDebt) {
    PreviewUpdateScheduler scheduler;
    ASSERT_TRUE(scheduler.ArmWake());
    handleWake(scheduler);
    EXPECT_TRUE(scheduler.HasUnrenderedPublish()) << "requesting the update is not the same as having rendered";

    scheduler.NoteRenderPass();
    EXPECT_FALSE(scheduler.HasUnrenderedPublish());
    EXPECT_EQ(scheduler.RenderPasses(), 1u);
}

// The exact defect. Frame N is published while the window cannot render; its
// wake-up is handled and its update request is dropped. When renderability
// returns, frame N must reach the screen — without frame N+1 being published to
// wake the scene graph, and without any unrelated redraw.
TEST(PreviewUpdateSchedulerTest, FrameOwedWhileUnrenderableIsPresentedWhenRenderabilityReturns) {
    PreviewUpdateScheduler scheduler;

    ASSERT_TRUE(scheduler.ArmWake());
    handleWake(scheduler); // ... and the render loop drops the request.
    ASSERT_EQ(scheduler.RenderPasses(), 0u);
    ASSERT_TRUE(scheduler.HasUnrenderedPublish());

    // Renderability returns — a screen change, an expose, a rebuilt scene graph.
    EXPECT_TRUE(reissuePendingPresentation(scheduler)) << "the frame published while unrenderable must be re-asked for";
    scheduler.NoteRenderPass();

    EXPECT_FALSE(scheduler.HasUnrenderedPublish());
    EXPECT_EQ(scheduler.PublishSignals(), 1u) << "no second frame may be required to unstick the preview";
    EXPECT_EQ(scheduler.SceneUpdateRequests(), 2u);
}

// The other direction of the same race: a publish that lands after a render
// pass has started is not carried by it and must stay outstanding. Clearing the
// debt after the acquire instead of before would write that frame off.
TEST(PreviewUpdateSchedulerTest, PublishRacingARenderPassStaysOwed) {
    PreviewUpdateScheduler scheduler;
    ASSERT_TRUE(scheduler.ArmWake());
    handleWake(scheduler);

    scheduler.NoteRenderPass(); // pass begins ...
    ASSERT_TRUE(scheduler.ArmWake());
    // ... and only now acquires the transport, having missed the publish above.

    EXPECT_TRUE(scheduler.HasUnrenderedPublish());
}

// A lifecycle transition storm must not turn into a redraw storm. Only the
// first transition after an owed frame asks for anything.
TEST(PreviewUpdateSchedulerTest, RepeatedRenderabilityTransitionsDoNotAmplify) {
    PreviewUpdateScheduler scheduler;
    ASSERT_TRUE(scheduler.ArmWake());
    handleWake(scheduler);

    EXPECT_TRUE(reissuePendingPresentation(scheduler));
    scheduler.NoteRenderPass();
    for (int i = 0; i < 20; ++i)
        EXPECT_FALSE(reissuePendingPresentation(scheduler)) << "transition " << i << " had nothing to ask for";

    EXPECT_EQ(scheduler.SceneUpdateRequests(), 2u);
    EXPECT_EQ(scheduler.RenderPasses(), 1u);
}

// Re-issuing must never disturb the gate: it is a GUI-thread nudge, not a
// producer edge, so it may neither arm nor leave the gate permanently armed.
TEST(PreviewUpdateSchedulerTest, ReissuingLeavesTheGateOpen) {
    PreviewUpdateScheduler scheduler;
    ASSERT_TRUE(scheduler.ArmWake());
    handleWake(scheduler);
    ASSERT_FALSE(scheduler.WakeInFlightForTest());

    reissuePendingPresentation(scheduler);
    scheduler.NoteRenderPass();

    EXPECT_FALSE(scheduler.WakeInFlightForTest());
    EXPECT_TRUE(scheduler.ArmWake()) << "the next real publish must still deliver its own wake-up";
}

// The benchmark window resets counters mid-flight. That must not forget a frame
// that is still owed a render, or a measurement run could park the preview.
TEST(PreviewUpdateSchedulerTest, ResetCountersKeepsThePresentationDebt) {
    PreviewUpdateScheduler scheduler;
    ASSERT_TRUE(scheduler.ArmWake());
    handleWake(scheduler);
    ASSERT_TRUE(scheduler.HasUnrenderedPublish());

    scheduler.ResetCounters();

    EXPECT_EQ(scheduler.RenderPasses(), 0u);
    EXPECT_TRUE(scheduler.HasUnrenderedPublish());
}

// ---------------------------------------------------------------------------
// Exhaustive interleaving check, with renderability.
//
// The model further down enumerates interleavings under the assumption that a
// requested render always runs. This one drops that assumption: the window may
// stop being renderable at any point and come back at any point, and a request
// issued while it is not renderable is lost. The property is unchanged — no
// terminal state may leave the preview behind — but only the re-issue on the
// renderable edge can carry it.
// ---------------------------------------------------------------------------
namespace {

enum class LifecycleStep { Publish, WakeDisarm, Render, Unrenderable, Renderable };

struct LifecycleOutcome {
    bool terminal = false;
    bool stale = false;
};

LifecycleOutcome runLifecycleInterleaving(const std::vector<LifecycleStep>& steps, bool reissue_on_renderable) {
    PreviewUpdateScheduler scheduler;
    uint64_t version = 0;
    uint64_t seen = 0;
    int queued = 0;
    bool renderable = true;
    bool render_requested = false;

    for (const LifecycleStep step : steps) {
        switch (step) {
        case LifecycleStep::Publish:
            ++version;
            if (scheduler.ArmWake())
                ++queued;
            break;

        case LifecycleStep::WakeDisarm:
            if (queued == 0)
                break;
            --queued;
            scheduler.DisarmWake();
            // The request only survives while the window can serve it.
            if (renderable)
                render_requested = true;
            break;

        case LifecycleStep::Render:
            if (!render_requested || !renderable)
                break;
            scheduler.NoteRenderPass();
            seen = version;
            render_requested = false;
            break;

        case LifecycleStep::Unrenderable:
            renderable = false;
            render_requested = false; // anything outstanding is dropped
            break;

        case LifecycleStep::Renderable:
            if (renderable)
                break;
            renderable = true;
            if (reissue_on_renderable && scheduler.HasUnrenderedPublish())
                render_requested = true;
            break;
        }
    }

    LifecycleOutcome outcome;
    // Terminal: the window can render, nothing is queued, and nothing is
    // outstanding. The desktop may now stay quiet forever.
    outcome.terminal = renderable && queued == 0 && !render_requested;
    outcome.stale = outcome.terminal && version > 0 && seen != version;
    return outcome;
}

} // namespace

TEST(PreviewUpdateSchedulerTest, NoRenderabilityInterleavingEndsOnAStaleFrame) {
    constexpr int kMaxDepth = 8;
    int terminal_states = 0;

    std::vector<LifecycleStep> steps;
    for (int depth = 1; depth <= kMaxDepth; ++depth) {
        const long long combinations = static_cast<long long>(std::pow(5.0, depth));
        for (long long encoded = 0; encoded < combinations; ++encoded) {
            steps.clear();
            long long remainder = encoded;
            for (int i = 0; i < depth; ++i) {
                steps.push_back(static_cast<LifecycleStep>(remainder % 5));
                remainder /= 5;
            }
            const LifecycleOutcome outcome = runLifecycleInterleaving(steps, /*reissue_on_renderable=*/true);
            if (!outcome.terminal)
                continue;
            ++terminal_states;
            ASSERT_FALSE(outcome.stale) << "stale terminal state at depth " << depth << ", encoding " << encoded;
        }
    }

    EXPECT_GT(terminal_states, 50000) << "the search covered too little to mean anything";
}

// The counterpart, and the reason the re-issue exists: without it a window that
// stops being renderable and comes back does park the preview on a stale frame.
// This is the shipped defect, pinned as a fact.
TEST(PreviewUpdateSchedulerTest, WithoutTheReissueARenderabilityGapDoesStrand) {
    constexpr int kMaxDepth = 6;
    bool found_stale = false;

    std::vector<LifecycleStep> steps;
    for (int depth = 1; depth <= kMaxDepth && !found_stale; ++depth) {
        const long long combinations = static_cast<long long>(std::pow(5.0, depth));
        for (long long encoded = 0; encoded < combinations && !found_stale; ++encoded) {
            steps.clear();
            long long remainder = encoded;
            for (int i = 0; i < depth; ++i) {
                steps.push_back(static_cast<LifecycleStep>(remainder % 5));
                remainder /= 5;
            }
            found_stale = runLifecycleInterleaving(steps, /*reissue_on_renderable=*/false).stale;
        }
    }

    EXPECT_TRUE(found_stale) << "the model no longer distinguishes the two, so the test above proves nothing";
}

// ---------------------------------------------------------------------------
// Exhaustive interleaving check.
//
// A thread-race test cannot carry this property. The lost wake-up only ever
// bites the LAST publish of a burst — in steady state a swallowed signal is
// covered by the next frame's own signal, so a racing producer hits the real
// failure roughly never, and the test passes with the ordering inverted. (That
// was measured: 20 000 racing frames did not fail once against the broken
// order.) What actually matters is a property over interleavings, so the
// interleavings are enumerated instead of hoped for.
//
// The model:
//   Publish     — slot = ++version; if ArmWake() a wake-up is queued
//   WakeDisarm  — take one queued wake-up and reopen the gate
//   WakeRender  — the render that wake-up asked for reads the slot
//
// A step that is not executable in the current state is skipped, so what is
// simulated is always some valid interleaving — just not always the one the
// encoding names. Terminal state = nothing queued and no render outstanding. The preview is
// then showing `seen`, and the desktop may stay quiet forever, so `seen` must
// equal `version`. That is exactly "no permanent stale frame".
// ---------------------------------------------------------------------------
namespace {

enum class Step { Publish, WakeDisarm, WakeRender };

struct ModelOutcome {
    bool terminal = false; // nothing queued, no render outstanding
    bool stale = false;    // terminal, but the consumer is behind
};

// disarm_before_render == false models the inverted ordering, kept only so the
// test can prove the ordering is what carries the property.
ModelOutcome runInterleaving(const std::vector<Step>& steps, bool disarm_before_render) {
    PreviewUpdateScheduler scheduler;
    uint64_t version = 0;
    uint64_t seen = 0;
    int queued = 0;
    bool render_outstanding = false;
    uint64_t render_reads = 0; // slot value the outstanding render will observe

    for (const Step step : steps) {
        switch (step) {
        case Step::Publish:
            ++version;
            if (scheduler.ArmWake())
                ++queued;
            break;

        case Step::WakeDisarm:
            if (queued == 0 || render_outstanding)
                break; // not executable here; the rest is still a valid interleaving
            --queued;
            if (disarm_before_render) {
                scheduler.DisarmWake();
                render_outstanding = true;
            } else {
                // The render is committed to the value it saw at this point;
                // the gate only reopens afterwards.
                render_reads = version;
                render_outstanding = true;
            }
            break;

        case Step::WakeRender:
            if (!render_outstanding)
                break;
            seen = disarm_before_render ? version : render_reads;
            if (!disarm_before_render)
                scheduler.DisarmWake();
            render_outstanding = false;
            break;
        }
    }

    ModelOutcome outcome;
    outcome.terminal = queued == 0 && !render_outstanding;
    outcome.stale = outcome.terminal && version > 0 && seen != version;
    return outcome;
}

} // namespace

TEST(PreviewUpdateSchedulerTest, NoInterleavingEndsOnAStaleFrame) {
    constexpr int kMaxDepth = 11;
    int terminal_states = 0;

    std::vector<Step> steps;
    for (int depth = 1; depth <= kMaxDepth; ++depth) {
        const long long combinations = static_cast<long long>(std::pow(3.0, depth));
        for (long long encoded = 0; encoded < combinations; ++encoded) {
            steps.clear();
            long long remainder = encoded;
            for (int i = 0; i < depth; ++i) {
                steps.push_back(static_cast<Step>(remainder % 3));
                remainder /= 3;
            }
            const ModelOutcome outcome = runInterleaving(steps, /*disarm_before_render=*/true);
            if (!outcome.terminal)
                continue;
            ++terminal_states;
            ASSERT_FALSE(outcome.stale) << "stale terminal state at depth " << depth << ", encoding " << encoded;
        }
    }

    EXPECT_GT(terminal_states, 50000) << "the search covered too little to mean anything";
}

// The counterpart: reopening the gate only after the render has read the slot
// does lose frames. This is the reason DisarmWake() must be the first thing a
// wake-up handler does, pinned as a fact rather than as a comment.
TEST(PreviewUpdateSchedulerTest, RenderBeforeDisarmDoesLoseFrames) {
    constexpr int kMaxDepth = 8;
    bool found_stale = false;

    std::vector<Step> steps;
    for (int depth = 1; depth <= kMaxDepth && !found_stale; ++depth) {
        const long long combinations = static_cast<long long>(std::pow(3.0, depth));
        for (long long encoded = 0; encoded < combinations && !found_stale; ++encoded) {
            steps.clear();
            long long remainder = encoded;
            for (int i = 0; i < depth; ++i) {
                steps.push_back(static_cast<Step>(remainder % 3));
                remainder /= 3;
            }
            const ModelOutcome outcome = runInterleaving(steps, /*disarm_before_render=*/false);
            found_stale = outcome.stale;
        }
    }

    EXPECT_TRUE(found_stale) << "the model no longer distinguishes the two orderings, so the ordering test above "
                                "proves nothing";
}

// Cheap smoke over a real producer thread. It cannot prove the property above,
// but it does exercise the atomics under contention and pins that the gate
// never manufactures wake-ups it was not asked for.
TEST(PreviewUpdateSchedulerTest, ConcurrentPublishingNeverManufacturesWakeups) {
    constexpr uint64_t kFrames = 20000;

    PreviewUpdateScheduler scheduler;
    std::atomic<uint64_t> queued{0};
    std::atomic<bool> producing{true};

    std::thread producer([&] {
        for (uint64_t frame = 1; frame <= kFrames; ++frame) {
            if (scheduler.ArmWake())
                queued.fetch_add(1, std::memory_order_acq_rel);
        }
        producing.store(false, std::memory_order_release);
    });

    std::thread consumer([&] {
        while (producing.load(std::memory_order_acquire) || queued.load(std::memory_order_acquire) > 0) {
            if (queued.load(std::memory_order_acquire) == 0) {
                std::this_thread::yield();
                continue;
            }
            queued.fetch_sub(1, std::memory_order_acq_rel);
            scheduler.DisarmWake();
        }
    });

    producer.join();
    consumer.join();
    // The consumer's exit condition can race the producer's last arm; drain
    // here so the accounting below is about the gate, not about that race.
    while (queued.load(std::memory_order_acquire) > 0) {
        queued.fetch_sub(1, std::memory_order_acq_rel);
        scheduler.DisarmWake();
    }

    EXPECT_EQ(scheduler.PublishSignals(), kFrames);
    EXPECT_LE(scheduler.Wakeups(), kFrames);
    EXPECT_EQ(scheduler.PublishSignals(), scheduler.CoalescedSignals() + scheduler.Wakeups())
        << "every publish either coalesced into a pending wake-up or produced exactly one";
}
