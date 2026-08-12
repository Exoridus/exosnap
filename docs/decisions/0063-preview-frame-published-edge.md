# ADR 0063: The preview transport carries a frame-published edge

## Status

Accepted. Amends the "Consequences" section of ADR 0058.

## Context

ADR 0058 built the Qt Quick preview on the existing NT-handle + keyed-mutex shared texture and
recorded the shortcut it had to take:

> The current bridge polls at the scene cadence because the existing shared-texture protocol has no
> frame-available notification. This produced stable results but many zero-timeout mutex misses;
> producer-driven or adaptive scheduling remains a useful optimization but does not block Record-area
> cutover.

The Widgets/Quick A/B campaign turned that from a note into a number. Quick used roughly double the
process CPU under a real GPU workload (2.70 % → 5.55 %) and roughly quadruple at idle
(1.34 % → 5.23 %), while every recording-side metric — problem drops, emitted frames, A/V drift,
acquire time, submit p95 — stayed equivalent. The cause was localised to one line: `preprocess()`
ended with `window_->update()`, so each rendered frame asked for the next one. On an idle desktop
that produced 10 061 whole-window scene-graph renders against 3 consumed source frames.

The polling was not laziness. The transport genuinely could not say anything else. It is a
single-slot shared texture with a keyed mutex, and a consumer's only question is a non-blocking
`AcquireSync` that answers "nothing new" and "the producer is mid-copy" identically. Qt Quick has no
partial redraw, so every one of those retries redrew the entire window.

Two alternatives were considered and rejected:

* **A fixed preview frame-rate cap.** The approved-but-unimplemented spec
  `docs/superpowers/specs/2026-08-07-preview-frame-rate-cap-design.md` describes a *product*
  setting — a user-visible "Preview frame rate" row that also allows Off. It is a useful feature and
  is unaffected by this ADR, but it is a ceiling on a poll loop, not a reason to render. Capping the
  loop at 60 still redraws 60 unchanged pictures a second on a static desktop.
* **Coupling the preview to the recording frame rate.** Recording cadence and preview cadence are
  independent concerns; the idle preview has no recording to take a rate from at all.

## Decision

The preview transport gains an explicit per-frame edge, emitted by the producer after a frame has
actually reached the shared texture, and the Quick preview renders because of that edge instead of
because it just rendered.

```text
producer publishes into the shared texture
        |
        | frame-published edge (no payload — the slot always holds the newest frame)
        v
PreviewUpdateScheduler: at most one wake-up in flight
        |
        | queued to the GUI thread
        v
ExoPreviewItem::requestSceneUpdate() -> QQuickItem::update()
        |
        v
one scene-graph render -> preprocess() consumes the frame
```

The edge exists at all three producers:

| producer | site |
|---|---|
| recording engine (WYSIWYG tap) | `RecorderSession::SetPreviewFramePublishedCallback`, fired from `VideoThread` |
| idle DXGI capture hub | `DxgiCaptureHubService::FramePublishedSink`, fired on the pump thread |
| idle WGC capture hub | `WgcCaptureHubService::FramePublishedSink`, fired on the pump thread |

The engine-side callback is a bare `std::function<void()>` with the same contract as the existing
`PreviewSharedHandleCallback` — set before `Record()`, must return fast, must not touch D3D on the
video thread. It carries no UI concept, so the engine stays UI-agnostic: it announces a fact about
its own transport, exactly as the handle callback already does.

**A contention drop deliberately does not signal.** `TryPublish` failing means the consumer has not
taken the *previous* frame yet, so a redraw is already on its way to it. Signalling there would add a
render without adding a picture.

### The scheduling gate

`PreviewUpdateScheduler` is a shared, QObject-free gate held by `std::shared_ptr` so a capture pump
thread or the engine's video thread can hold it without touching any Qt object lifetime. Producers
call `ArmWake()`; the GUI-thread handler calls `DisarmWake()` **before** requesting the update.

That ordering is the whole correctness argument:

* A publish whose `ArmWake()` returned true has its own wake-up, handled after the publish.
* A publish whose `ArmWake()` returned false was swallowed — but only because an earlier wake-up had
  not been disarmed yet, and disarming is the first thing its handler does. That handler therefore
  still runs after the swallowed publish.

Because the slot is last-value, one render after the newest publish is sufficient; nothing has to be
replayed. Disarming *after* the update request inverts this and loses the final frame of a burst,
which shows up as a preview frozen on stale content until the desktop next changes.

The cost of the ordering is bounded redundancy: a publish landing between the disarm and the render
can produce one extra render that finds nothing to consume. One extra redraw, never a loop.

The property is pinned by an exhaustive enumeration of interleavings rather than by a thread race —
a racing producer does not reliably hit the failure, because it only bites the last publish of a
burst. `test_preview_update_scheduler.cpp` checks every terminal state up to depth 11 and separately
asserts that the inverted ordering *does* produce a stale terminal state, so the ordering test cannot
silently stop meaning anything.

### What still schedules a render

The edge is added to, not substituted for, the ordinary `QQuickItem` invalidations: geometry and
corner radius, normalized source rect, visibility, a new or cleared source, and scene-graph
invalidation/re-initialisation. `preprocess()` no longer requests anything.

## Consequences

The idle preview stops rendering entirely while the desktop is quiet — DXGI Output Duplication
produces no frame, so there is no edge, so there is no render. During recording the engine's tap is
already gated to ~30 Hz, which now becomes the preview's render rate instead of the display's 144 Hz.

`mutex_misses` and `source_delivery_fps` remain `Approximate` in the benchmark contract for the same
reason as before — both are observed per consumer attempt — but their magnitudes change with the
attempt rate, so pre-fix and post-fix numbers for them are not comparable.

The Widgets preview is untouched: it presents from a dedicated render thread on a fixed interval and
passes a null frame sink.

This does not make the preview free. Qt Quick still renders the whole window per update where the
Widgets renderer presented only its quad; the migration's win was always structural (one top-level
HWND, so `WM_NCHITTEST` reaches the window and overlays can composite over the preview), not
per-area repainting. What this removes is the renders that had no new picture in them.
