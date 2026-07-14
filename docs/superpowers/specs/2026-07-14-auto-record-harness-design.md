# Auto-Record Harness — Design

Status: approved (brainstorming), 2026-07-14. Not yet implemented.

## Problem

A large share of the manual live-verify checklist (`.workspace/live-verify-checklist-0.9.md`)
consists of "start a recording with configuration X, then inspect the output file" — no subjective
human judgment actually required, just file/log inspection that happens to currently require a
human to click through the UI to produce the file. CLAUDE.md forbids driving the running app (mouse/
keyboard synthesis, window automation) but already carves out `--visual-test` as a CLI/env-driven,
non-interactive harness for pixel verification. This spec extends the same carve-out to recording
verification: a CLI/env-driven harness that produces a real output file non-interactively, which is
then inspected with `ffprobe`/`ffmpeg`/log parsing instead of a screenshot diff.

## Non-goals

- Does not touch Edit/Output/Save (trim-handle drag, scrubbing, marker placement) — those are
  interactive-drag checks by nature and stay human-only regardless of this harness.
- Does not attempt device-loss/hot-plug/UAC/network-disconnect scenarios that require disrupting the
  developer's actual machine state (real monitor unplug, real device swap, real UAC prompt). Those
  stay on the manual checklist.
- Does not run in CI (no GPU/display on CI runners) — this is a developer/agent tool, opt-in only,
  same positioning as the `[Live]` items in `docs/release-checklist.md`.

## Scope: which checklist items this covers

**Bare mode (7 items, no `MainWindow`):** APP-row audio corruption signature (#1), present-mode
diagnostics false-positive across two recordings (#6), HDR10 bitstream SEI (#7), 10-bit tonemap
intermediate (#8), WGC-HDR window (#9), 4:4:4 snapshot fidelity (#11), avcC 4:4:4 trailer (#12).
(Numbers refer to `.workspace/live-verify-checklist-0.9.md`.)

**Preview mode (3 items, off-screen `MainWindow`):** capture-hub lease log markers on the
start/stop transition (#14), idle-preview color/cursor/webcam-PiP fidelity (#15, fluidity/60 Hz
timing deferred — see Open questions), native-HDR10 preview showing the encoded frame (#17).

**Stays human-only, this harness does not change it:** Idle-OD-probe with a real game in exclusive
fullscreen (#13) and monitor hot-plug during idle preview (#16) — both require either a third-party
application or a disruptive, visible change to the developer's actual display topology.

## Architecture

Two entry points in one new module, `app/auto_record/AutoRecordHarness.{h,cpp}`, mirroring the
existing `app/visual_tests/VisualTestHarness.{h,cpp}` pattern (`HasXRequest`, `ParseXOptions`,
`RunX`).

**Bare mode.** `main.cpp` checks `HasAutoRecordRequest(args)` before constructing `MainWindow` (same
position as the existing `--visual-test` check, but bare mode skips `MainWindow` construction
entirely). It constructs a `RecordingCoordinator` directly, runs the existing capability-detection
bootstrap (reused, not reimplemented — same code path the real app's async startup probe uses,
called synchronously here), applies `AutoRecordOptions` via `SetOutputSettings`/`SetVideoSettings`,
calls `StartRecording(target, audio_ui_state)`, waits `duration_seconds` on a `QTimer` inside the
Qt event loop, calls `StopRecording()`, waits (bounded grace period) for `ResultReadyCallback`,
prints the result JSON, exits. No window is ever created.

**Preview mode** (`--enable-preview`). Builds the real `MainWindow` off-screen (reusing
`--visual-test`'s off-screen/non-primary-display convention), activates the idle preview the same
way `RecordPage` does when shown (no click — a direct call to the existing preview-activation
entry point), then drives the **same coordinator instance `MainWindow` owns** through the bare-mode
start/stop sequence above. For #15/#17, reuses `WriteVisualScreenshot()` unchanged for the
color/cursor/PiP comparison.

**Output isolation.** No new mechanism — reuses the existing `EXOSNAP_OUTPUT_DIR` env override
already supported by `RecordingCoordinator::EffectiveOutputFolder()`. Callers (Claude, or a
delegated subagent) always set this to a scratch directory outside the repo. Output files are never
committed, per the standing live-recording-verification approval.

**Config surface (CLI flags, `VisualTestOptions`-style struct):**

```
--auto-record
--target=monitor|window|region
--target-window-title=<substring>   # required when --target=window
--audio-rows=app,sys,mic            # comma list of enabled rows
--merge-above=<row>                 # optional
--container=mkv|mp4|webm
--video-codec=h264|hevc|av1
--audio-codec=opus|aac|pcm
--chroma=420|444
--bit-depth=8|10
--hdr=off|tonemap|native
--duration=<seconds>                # default 10
--capture-frame-at=<seconds>        # optional, triggers CaptureFrame() mid-recording
--enable-preview                    # switches to preview mode
--screenshot-path=<path>            # preview mode only
```

Invalid combinations (e.g. `--chroma=444 --bit-depth=10`) are **not** pre-validated by the harness —
they pass through to the real capability resolver, so its reconcile-to-4:2:0 behavior is exercised
and verifiable rather than silently avoided.

**Output contract.** One JSON object on stdout on completion, then exit 0 (success) or 1 (recorder
reported failure):

```json
{
  "status": "ok",
  "output_path": "...",
  "session_report_path": "",
  "error_detail": ""
}
```

`session_report_path` is always empty: `RecordingCoordinator` writes its session report to disk
internally but exposes no public accessor for the path, so the harness has nothing to read. Adding
one is a small, separate follow-up, not covered here. `screenshot_path` is not part of the contract
— in preview mode the caller already knows the path, since it's the same `--screenshot-path` value
they passed on the command line; echoing it back would be redundant.

The harness never blocks indefinitely: a bounded grace period after `StopRecording()` (covering
remux) is enforced, after which it exits with an explicit timeout status rather than hanging.

## CLAUDE.md / AGENTS.md update

Add a sentence under "Never drive the running application" naming `--auto-record` as the same class
of exception as `--visual-test`: CLI/env-configured, no mouse/keyboard synthesis, no window
automation; bare mode never creates a window, preview mode creates one off-screen only to reuse the
existing preview/hub and screenshot machinery, never to click through it. Also codify the existing
verbal approval: output files go through `EXOSNAP_OUTPUT_DIR` to a scratch directory and are never
committed.

## Testing

A unit test for `ParseAutoRecordOptions` (argument parsing, malformed/missing values), matching
whatever coverage exists for `ParseVisualTestOptions`. The harness itself is not part of the `ctest`
gate or CI — it needs a real GPU/display, so it stays a manual/agent-invoked tool, documented as such
next to the `[Live]` items in `docs/release-checklist.md`'s spirit.

## Open questions (for the implementation plan, not blocking this spec)

1. **#15 fluidity (~60 Hz)** — screenshot diffing covers color/cursor/PiP correctness but not frame
   timing. Whether to add a lightweight frame-callback-timestamp log (variance check) or drop the
   fluidity sub-claim from the automated version and leave it as a human spot-check is an
   implementation-time call, not a design blocker.
2. Exact capability-bootstrap reuse point (which existing startup function bare mode calls
   synchronously) needs to be pinned down against the current `app/main.cpp` startup sequence when
   the plan is written — not re-derived from scratch, but confirmed against then-current code.
