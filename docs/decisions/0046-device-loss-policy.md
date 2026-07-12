# ADR 0046: Coherent device-loss policy mid-recording

## Status

Accepted — implemented (engine). The audio path degrades a lost source to honest
silence and reactivates it; the display, window and webcam paths already behaved
this way and are documented here as one coherent policy. A standing user-facing
notification for the degraded state, and manual live-hardware verification, are
follow-ups (see Consequences).

Related: ADR 0013 (Output Duplication for monitor capture), ADR 0041 (held frame),
ADR 0018 (each enabled source is its own track), ADR 0016 (notifications),
ADR 0030 (output-format audio decorator).

## Context

Losing a capture device *during* a recording was handled differently by every
subsystem, and none of it was a deliberate, documented decision:

- **Monitor** (DXGI Output Duplication): holds the last frame and reopens the
  same output indefinitely (graceful).
- **Window** (Windows Graphics Capture): ends the whole recording cleanly when the
  window closes (the segment stays valid).
- **Audio endpoint** (SYS loopback / MIC / APP process loopback): ended the whole
  recording — including video — with an error. An unplugged USB mic could destroy
  an hour-long screen recording. Worse, this was only true for *unwrapped*
  single-source tracks: a source inside a `MixedAudioSrc` (any merged track, or a
  single source with non-unity gain) had its loss **silently swallowed** — the
  dead source vanished without a trace and never came back.
- **Webcam** (Media Foundation PiP): freezes the last frame and reopens the reader
  indefinitely (graceful).

The asymmetry was the real problem: the video primary path degrades gracefully,
but any audio endpoint loss was session-fatal (or silently lost). That contradicts
the track model (ADR 0018 — each source is its own track) and the honesty the
video path already lives.

## Decision

Freeze one coherent, per-device-type policy. The recording is **never retargeted**
onto a *different* device; the engine only holds or reacquires the **same** source
identity.

| Device | Loss | Policy |
|---|---|---|
| Monitor | `ACCESS_LOST` | Hold last frame + reopen same display (GDI name), unbounded |
| Monitor/GPU | `DEVICE_REMOVED`/`_HUNG`/`_RESET` | End cleanly (segment valid) |
| Window | `Item.Closed` | End cleanly (no reopen — the target is gone) |
| Audio source | endpoint invalidated / service down | **Degrade the source to honest silence + keep recording + reactivate same identity every 500 ms** |
| Webcam | reader dead | Hold last frame + reopen same device, unbounded |

The one behavior change is the audio path (Option B — "silence and continue"):

- On a source acquire failure classified as a device loss
  (`ClassifyAudioSourceLoss` → `DegradeSource`), the audio thread does **not**
  `RecordFailure`. It holds the encoder timeline continuous with encoder-cadence
  silence (a wall-clock computation against the accumulated frame counter — the
  discontinuity-gap path cannot measure an outage with no packets), and reactivates
  the source at the poll cadence (`DecideAudioDeviceLoss`, unbounded — honest
  silence never gets worse by waiting, so there is no give-up-and-kill branch, by
  analogy with the video reopen loop's `std::nullopt` budget). On reactivation the
  A/V clock-drift estimator is reset (the reacquired stream restarts its device
  position near zero).
- **Source-granular, not track-granular.** `MixedAudioSrc` no longer swallows a
  dead inner: it marks that inner degraded (surfaced via `DegradedSourceCount`),
  keeps mixing the survivors, and reacquires the degraded inner on `Reinit`.
- **Reinit identity per audio flavor:**
  - Fixed-`device_id` mic → reopens exactly that id.
  - Default mic (`nullopt`) and system-output (default render) → re-resolve the
    *current* Windows default. This is the unchanged semantics of "the system
    default", not a substitution, and is stated honestly in the docs.
  - APP / window-SYS process loopback (PID-keyed) → reacquire only while the same
    process instance is still alive (PID **plus** process creation time, so a
    recycled PID never grabs a stranger); otherwise the source stays permanently
    silent.
- If **every** audio source is lost, the recording continues **video-only** (the
  picture is the primary source).

Visibility (calm, not alarmist): a live Diagnostics figure
(`AudioDiagnostics.degraded_sources` / `source_degraded`) while any source is
degraded, and a post-flight fact (`SessionStats.audio_degraded_occurred`) so the
"Saved" report can note the recording contains a silence gap.

## Alternatives considered

- **A — keep audio loss session-fatal.** Rejected: disproportionate; a bumped mic
  destroys an otherwise-clean recording, contradicting the video/webcam paths and
  the track model. "The old stream is dead" only justifies killing the *stream*,
  not the *session* — a fresh source object re-`Init`s fine.
- **C — audio like video but with a give-up budget.** Rejected: the video path
  deliberately chose unbounded; an audio budget that kills the session would
  reintroduce the asymmetry.

## What is deliberately NOT built

- No mid-recording retarget onto a *different* display, window, mic or endpoint.
- No grabbing a stranger on PID reuse (process-loopback flavors fail closed).
- No coupling of the idle `*DeviceNotifier` UI snapshots to the running engine —
  loss detection stays HRESULT-driven in the engine thread (one source of truth).

## Consequences

- Audio device loss is now a survivable, honest degradation instead of a
  recording-ender. The `MixedAudioSrc` silent-death hole is closed.
- The `ClassifyWasapiAcquireFailure` source-layer classifier is unchanged (it still
  answers only "is the stream dead"); the session-level reaction moved to
  `ClassifyAudioSourceLoss` / `DecideAudioDeviceLoss` (`audio_device_loss_policy.h`).
- Remaining boundary: when *all* inners of a single merged track are lost at once,
  that track's timeline holds rather than being filled with exact-length silence;
  a bare single-source track and the video track always keep exact continuity.
- Follow-ups: a standing user-facing notification for the degraded state (the
  engine already exposes the signal); and manual live-hardware verification
  (real endpoint unplug/replug, default-device follow, PID-reuse, audio-service
  restart) — the automated coverage uses fake sources.
