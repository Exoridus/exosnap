# Soak, A/V-sync, and recovery drills (developer runbook)

Developer tooling for the 0.10 reliability-hardening promises: long-recording
**soak**, **A/V-sync drift** validation, and **recovery drills**. This is
dev/test infrastructure — nothing here changes user-visible behaviour, so it is
not in `docs/product-spec.md`. The recovery/durability promises it exercises are
already specified there (§ Crash recovery).

> **All thresholds here are advisory for 0.10 — not a release gate** — except
> where `docs/release-checklist.md` §7 adopts a threshold as an explicit gate
> (the 0.9 clock-slaving soak uses A/V drift ≤ 20 ms / 2 h as its pass bar). The
> *infrastructure* is the deliverable; the *numbers* (A/V drift ≤ 20 ms/2 h, the
> leak slope, the powerloss window) are starting values that surface regressions,
> not pass/fail bars otherwise. Tighten them once real soak data exists.

The split is deliberate: everything deterministic and GPU-free runs in CI; the
real GPU/display/audio/powerloss runs are **user-live on the developer machine**.

---

## 1. Headless soak — `exosnap-soak`

Drives a long recording through the **real** `RecorderSession` pipeline (or a
GPU-free synthetic twin), samples engine + host-process metrics into a JSON-Lines
timeline, applies the advisory abort budgets live, and writes a report.

### Build

`exosnap-soak` builds with the normal Debug/Release configure (it links
`recorder_core`, whose NVENC headers are vendored, so it builds even on the
GPU-less CI runner). Only the **real recording path** needs a GPU at runtime.

```
cmake --preset windows-x64-debug
cmake --build build/windows-x64-debug --config Debug --target exosnap-soak
```

### Real 2 h soak (user-live, needs an NVIDIA GPU + a display)

Point the capture at a **deterministic test pattern** (a looping clip or a static
scene — the point is a repeatable source, not a moving desktop):

```
exosnap-soak --minutes 120 --vcodec av1 --acodec opus --container mkv \
             --out D:\soak\run.mkv --report-dir D:\soak
```

- Stops after `--minutes` **or** on Ctrl-C (graceful `session.Stop()`).
- Writes `run.mkv.timeline.jsonl` (one metric sample per `--sample-ms`, default
  1000 ms) next to the recording, plus `soak-report-<ts>.json` and `.md`.
- Exit code: `0` OK · `1` session failed · `2` no capture target / validate
  rejected · `3` advisory abort tripped.

A healthy run: **no advisory abort**, RSS/handle leak slope near zero, drift and
skew within budget, and a final file that demuxes with a media duration ≈ wall
clock. Record the output **volume** (the report captures it) — `FlushFileBuffers`
on a slow disk can perturb the metrics.

### Synthetic twin (CI-able harness validation — never A/V-sync acceptance)

```
exosnap-soak --synthetic --seconds 60 --realtime --out %TEMP%\syn.mkv
```

Runs the shared `SyntheticSession` (real audio encode + mux + finalize, a
deterministic in-process video feeder) faster than real time. It exercises the
mux/audio/finalize + the report/abort plumbing at scale and catches skew/monotone
regressions, but **cannot see real device-clock drift or real capture-path RAM
growth** (ideal clocks, no GPU). Use it to validate the harness, never as A/V
acceptance. The `--realtime` flag paces the feeder to wall-clock so a short run
yields a spread of samples.

### The metric timeline

Each JSONL row: `t_s`, `av_drift_ms` (+ `_available`), `duration_skew_ms`
(+ `_available`), `frames_captured/emitted`, `frames_dropped_{coalesced,cfr,
backpressure,processing_failure}`, `frames_duplicated`, `audio_discontinuities`, `mux_queue_depth`,
`disk_fill_eta_s`, `rss_bytes`, `private_bytes`, `handle_count`, `gdi_objects`,
`user_objects`, `health_critical`, `bottleneck`. The report aggregates
min/max/mean/p99 per metric, the least-squares **leak slope** for RSS/handles,
cumulative drop totals, and the advisory abort verdict.

### Advisory abort budgets

`SoakAbortPolicy` stops a clearly-diverging run early (so a broken 2 h soak fails
in minutes) on any **sustained** (not single-spike) violation: recorder failure ·
sustained `Critical` health · duration skew over budget **and still growing** ·
A/V drift over budget (when available) · drop ratio over budget · RSS/handle leak
slope over threshold (only after a baseline window). All defaults are advisory;
override with `--max-drift-ms` / `--max-skew-ms`.

---

## 2. A/V-sync drift — clapper + `av-sync-check.py`

Measures A/V **clock drift** of a finished file from a two- or three-marker clapper signal.

### Capture (user-live)

The backward-compatible two-marker form
`exosnap-soak --clapper --seconds 120` emits a full-frame **white flash** + loud
**beep** immediately and again after 120 seconds. For long acceptance runs use one
three-marker process whose total duration includes recording margins:

```powershell
exosnap-soak --clapper --seconds 7200 --markers 3 `
  --start-margin-seconds 10 --end-margin-seconds 10
```

That schedule emits at `+10 s`, `+3600 s`, and `+7190 s`; the 3-hour equivalent
(`--seconds 10800`) emits at `+10`, `+5400`, and `+10790`. Add
`--print-clapper-schedule` to validate either schedule without waiting or producing a
flash/beep. Durations and integer controls are strict: missing, zero, negative,
nonnumeric and overflowing values fail with exit 64.

The helper is a separate executable, does not acquire ExoSnap's single-instance mutex,
and returns from the clapper path before any recorder/capture session is constructed.
It owns one topmost Win32 full-screen window on the primary display only while each
marker is emitted and uses Win32 `Beep`; no media player or second ExoSnap process is
involved. Run it while ExoSnap records that primary display + system audio (SYS
loopback). Inherently live — it needs a real display and render endpoint that ExoSnap
captures.

### Analyze

```
python scripts/dev/av-sync-check.py <recorded-file> [--max-drift-ms 20]
```

For a scheduled three-marker run, provide the expected schedule so extra paired
disturbances cannot be silently selected:

```powershell
python scripts/dev/av-sync-check.py <recorded-file> --max-drift-ms 20 `
  --expected-markers 3 --marker-times-seconds 10,3600,7190
```

The analyzer pairs each flash edge to the closest beep edge within the bounded
cross-stream skew, then recovers start/end (and, for three markers, middle) PTS:

```
offset_start = flash_start - beep_start
offset_end   = flash_end   - beep_end
drift        = offset_end - offset_start     (over the measured span)
```

Three-marker output also includes `offset_middle`, `drift_start_middle`,
`drift_middle_end`, recognized flash/beep PTS and raw/paired event counts. Auto mode
accepts exactly two or three pairs and fails closed on extras; an explicit expected
schedule may select the matching marker set around disturbances. Opposing segment
drifts that exceed the total budget but cancel at the endpoint are printed and
recorded in JSON as a reliability finding even when the canonical start→end gate
passes.

**Only the drift is pass/fail.** The absolute `offset_start` carries a
device-dependent **emission skew** (~10–50 ms: GPU present → display capture vs.
WASAPI render → SYS loopback) that is *not* an ExoSnap error and cancels in the
drift. So the absolute offset is reported **advisory**; the exit code is driven by
drift (`0` within budget · `2` over · `3` unmeasurable). Hardening the absolute
offset would need a one-time calibrated emission-skew subtraction for the setup.

This is the drift **acceptance method for `av-clock-slaving`**: run it before
clock-slaving to measure the drift, after to prove the compensation. It is **not**
an acceptance of the absolute start offset.

Requires a **full system ffmpeg** (the app bundles a mux-only FFmpeg without the
`signalstats`/`astats`/`silencedetect` filters this needs). Pin your ffmpeg
version in a real acceptance run; HDR sessions tone-map the flash, so run HDR
A/V-sync separately.

### CI regression guard

The `dev-scripts` CI job provisions + version-verifies system ffmpeg and runs the
analyzer against a committed golden clip (`tests/fixtures/av-sync/
clapper-golden.mp4`, ~16 KB, drift ≈ 0 by construction). Regenerate it with
`python scripts/dev/gen-av-sync-fixture.py` when the analyzer changes. The golden
clip's residual drift (~one video-frame quantum) is expected — it guards the
*script*, it is not a real drift budget.

---

## 3. Recovery drills

Adversarial tests of the recovery machinery (`RecoveryService.Scan/Finish`,
repair-remux, durability flush) across the matrix
**{Recording, Finalize, Remux} × {Ordered-Stop, Process-Kill, Powerloss}**.

### CI drills (deterministic, no GPU)

- `recovery.recovery_drill_tests` — drives `RecoveryService` against **real**
  synthetic-pipeline MKVs (real `MatroskaStreamWriter`). Ordered-Stop → clean
  finalize → rename → demuxable. Process-Kill (modelled by truncating a finalized
  MKV: its committed clusters are exactly what a killed recording leaves) →
  `RemuxToMkv` repair must return and either repair or preserve the artefact.
  MP4-intended → progressive remux.
- `recorder_core.recovery_truncation_tests` — the **powerloss proxy**: truncate a
  genuinely-flushed MKV at assorted offsets and assert the repair never crashes and
  salvages up to the last complete cluster. Models "the tail is gone"; it does
  **not** prove the durability window.

**What CI does NOT cover:** the real `RecordingCoordinator`
Add/`UpdateFinalized`/`Remove` manifest choreography — the drill child reproduces
that ordering, it does not execute it (the real coordinator needs a GPU). An
ordering bug in the coordinator's crash window stays invisible in CI; only the
live-kill drill below catches it.

### Live drills (user-live — the only proof of the durability promise)

Per phase (Recording / Finalize / Remux), against the **real app**:

1. **Process-kill:** `TerminateProcess` (or Task Manager) mid-phase, restart,
   confirm the recovery overlay lists the entry and `Finish` recovers it.
2. **Powerloss:** a real hard-reset / VM forced-off mid-phase. Only this proves
   the ≤ 2 s (`kDurabilityFlushInterval`) + reorder-window + one non-rendered
   cluster loss model actually holds and the file recovers.

### Resolved: stale partial MP4 at the target path

The MP4 Remux×Kill drill (`RemuxMp4ProcessKill_ReplacesStalePartialAtTargetPath`)
was a deliberate **xfail** and is now a hard assertion.

> Previously `RecoveryService.Finish` remuxed **directly** to the final,
> user-visible MP4 path. A kill/powerloss mid-remux left a corrupt half-MP4 exactly
> where the user expected their result, and next launch recovery produced the good
> MP4 under a *different* name (`ResolveUniqueOutputPath`) and **left the corrupt
> file behind**. No data was lost (the MKV was still the source of truth), but the
> user saw a broken file at the target path.

`Finish` now remuxes to a sibling `.part` temp on the target's own volume and
atomically renames it onto the target with `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`.
A corrupt partial already at `final_output_path` (this recording's own interrupted
remux) is overwritten in place; a crash *during* the recovery remux leaves only the
`.part` temp, never a half-written file at the target. The same temp+atomic-rename
guard is applied to the MKV repair-remux path.

The **live** `RecordingCoordinator` remux (`RunRemuxJob` / `StartSegmentRemuxThread`)
now carries the identical guarantee: it remuxes to a sibling `.part` temp on the
target's own volume and atomically renames it onto the final MP4 on success, using
the same `MakeSiblingTempPath` / `AtomicReplaceInPlace` primitives (shared in
`app/services/AtomicFileOps.*`). A kill mid-remux leaves only the `.part` temp — the
user-visible output path never holds a half-written MP4. The live-path drills
`LiveRemuxMp4_NeverWritesTargetMidFlightThenPublishesAtomically` and
`LiveRemuxMp4_CancelLeavesTargetUntouchedAndRemovesTemp` exercise that sequence
(including a mid-flight check that the target stays empty until the atomic publish).
