# Soak, A/V-sync, and recovery drills (developer runbook)

Developer tooling for the 0.10 reliability-hardening promises: long-recording **soak**, **A/V-sync drift** validation, and **recovery drills**. This is dev/test infrastructure: nothing here changes user-visible behaviour, so it is not in `docs/product-spec.md`. The recovery/durability promises it exercises are already specified there (§ Crash recovery).

> **All thresholds here are advisory for 0.10, not a release gate,** except
> where `docs/release-checklist.md` §7 adopts a threshold as an explicit gate
> (the 0.9 clock-slaving soak uses A/V drift ≤ 20 ms / 2 h as its pass bar). The
> *infrastructure* is the deliverable. The *numbers* (A/V drift ≤ 20 ms/2 h, the
> leak slope, the powerloss window) are starting values that surface regressions,
> not pass/fail bars otherwise. Tighten them once real soak data exists.

The split is deliberate: everything deterministic and GPU-free runs in CI. The real GPU/display/audio/powerloss runs are **user-live on the developer machine**.

---

## 1. Headless soak: `exosnap-soak`

Drives a long recording through the **real** `RecorderSession` pipeline (or a GPU-free synthetic twin), samples engine + host-process metrics into a JSON-Lines timeline, applies the advisory abort budgets live, and writes a report.

**Why a separate host tool, not GUI automation or an extended `probe_record`.** Driving the shipping GUI for a 2-hour endurance run is forbidden and brittle (CLAUDE.md: no synthesized input) and would mix UI state into a measurement that should only be about the engine and the host process. `tools/probes/probe_record` stays a narrow, single-shot correctness probe with an `ffprobe`-based pass/fail contract. Folding an endurance run's metric timeline, advisory-abort policy, and leak-slope analysis into it would dilute both the probe's contract and this tool's.

### Build

`exosnap-soak` builds with the normal Debug/Release configure (it links `engine`, whose NVENC headers are vendored, so it builds even on the GPU-less CI runner). Only the **real recording path** needs a GPU at runtime.

```
cmake --preset windows-x64-debug
cmake --build build/windows-x64-debug --config Debug --target exosnap-soak
```

### Real 2 h soak (user-live, needs an NVIDIA GPU + a display)

Point the capture at a **deterministic test pattern** (a looping clip or a static scene: the point is a repeatable source, not a moving desktop):

```
exosnap-soak --minutes 120 --vcodec av1 --acodec opus --container mkv \
             --out D:\soak\run.mkv --report-dir D:\soak
```

- Stops after `--minutes` **or** on Ctrl-C (graceful `session.Stop()`).
- Writes `run.mkv.timeline.jsonl` (one metric sample per `--sample-ms`, default 1000 ms) next to the recording, plus `soak-report-<ts>.json` and `.md`.
- Exit code: `0` OK · `1` session failed · `2` no capture target / validate rejected · `3` advisory abort tripped.

A healthy run: **no advisory abort**, RSS/handle leak slope near zero, drift and skew within budget, and a final file that demuxes with a media duration \u2248 wall clock. Record the output **volume** (the report captures it): `FlushFileBuffers` on a slow disk can perturb the metrics.

### Synthetic twin (CI-able harness validation, never A/V-sync acceptance)

```
exosnap-soak --synthetic --seconds 60 --realtime --out %TEMP%\syn.mkv
```

Runs the shared `SyntheticSession` (real audio encode + mux + finalize, a deterministic in-process video feeder) faster than real time. It exercises the mux/audio/finalize + the report/abort plumbing at scale and catches skew/monotone regressions, but **cannot see real device-clock drift or real capture-path RAM growth** (ideal clocks, no GPU). Use it to validate the harness, never as A/V acceptance. The `--realtime` flag paces the feeder to wall-clock so a short run yields a spread of samples.

### The metric timeline

Each JSONL row: `t_s`, `av_drift_ms` (+ `_available`), `duration_skew_ms` (+ `_available`), `frames_captured/emitted`, `frames_dropped_{coalesced,cfr, backpressure,processing_failure}`, `frames_duplicated`, `audio_discontinuities`, `mux_queue_depth`, `disk_fill_eta_s`, `rss_bytes`, `private_bytes`, `handle_count`, `gdi_objects`, `user_objects`, `health_critical`, `bottleneck`. The report aggregates min/max/mean/p99 per metric, the least-squares **leak slope** for RSS/handles, cumulative drop totals, and the advisory abort verdict.

### Advisory abort budgets

`SoakAbortPolicy` stops a clearly-diverging run early (so a broken 2 h soak fails in minutes) on any **sustained** (not single-spike) violation: recorder failure \u00b7 sustained `Critical` health \u00b7 duration skew over budget **and still growing** \u00b7 A/V drift over budget (when available) \u00b7 drop ratio over budget \u00b7 RSS/handle leak slope over threshold (only after a baseline window). All defaults are advisory. Override with `--max-drift-ms` / `--max-skew-ms`.

---

## 2. A/V-sync drift: clapper + `av-sync-check.py`

Measures A/V **clock drift** of a finished file from a clapper signal of two or more markers.

**Why a separate Python script against a full system `ffmpeg`, not built into `exosnap-soak` itself.** Frame-luma and audio-amplitude marker detection needs `avfilter`/`swscale`, and the FFmpeg build this application vendors and ships is deliberately mux-only (`cmake/VendorFFmpeg.cmake`) with neither. Building the analysis into the C++ soak tool would mean either linking a second, full-featured FFmpeg into a developer-only tool or reimplementing frame/amplitude decoding by hand. A short script against whatever full `ffmpeg` is already on the developer's machine costs neither.

### Capture (user-live)

The backward-compatible two-marker form `exosnap-soak --clapper --seconds 120` emits a full-frame **white flash** + loud **beep** immediately and again after 120 seconds. For long acceptance runs use one three-marker process whose total duration includes recording margins:

```powershell
exosnap-soak --clapper --seconds 7200 --markers 3 `
  --start-margin-seconds 10 --end-margin-seconds 10
```

That schedule emits at `+10 s`, `+3600 s`, and `+7190 s`. The 3-hour equivalent (`--seconds 10800`) emits at `+10`, `+5400`, and `+10790`. Add `--print-clapper-schedule` to validate either schedule without waiting or producing a flash/beep. Durations and integer controls are strict: missing, zero, negative, nonnumeric and overflowing values fail with exit 64.

**Three markers are usually not enough**: see *How many markers a budget needs* below. `--markers` takes any count from 2 upward, spread evenly across the span between the margins.

The helper is a separate executable, does not acquire ExoSnap's single-instance mutex, and returns from the clapper path before any recorder/capture session is constructed. It owns one topmost Win32 full-screen window on the primary display only while each marker is emitted and uses Win32 `Beep`. No media player or second ExoSnap process is involved. Run it while ExoSnap records that primary display + system audio (SYS loopback). Inherently live: it needs a real display and render endpoint that ExoSnap captures.

### Analyze

```
python scripts/dev/av-sync-check.py <recorded-file> [--max-drift-ms 20]
```

For a scheduled three-marker run, provide the expected schedule so extra paired disturbances cannot be silently selected:

```powershell
python scripts/dev/av-sync-check.py <recorded-file> --max-drift-ms 20 `
  --expected-markers 3 --marker-times-seconds 10,3600,7190
```

The analyzer pairs each flash edge to the closest beep edge within the bounded cross-stream skew, locates each edge to sub-sample precision, and fits a straight line through every marker's offset:

```
offset_i  = flash_i - beep_i
offset(t) = intercept + slope * t          (weighted least squares over all markers)
drift     = slope * span                   (the verdict's quantity)
```

The endpoint difference `offset_end - offset_start` is still reported, as a diagnostic. It is not the verdict: an endpoint difference is the drift only when the drift is linear, and two points cannot show whether it is. A run whose offset moves 60 ms and comes back reports exactly zero endpoint drift.

Three-marker output also includes `offset_middle`, `drift_start_middle`, `drift_middle_end`, recognized flash/beep PTS and raw/paired event counts. Auto mode accepts exactly two or three pairs and fails closed on extras: it has no schedule to check against, so nothing there separates four real markers from three plus a disturbance. A longer schedule is declared with `--expected-markers` (any count) and optionally `--marker-times-seconds`. The analyzer then picks the most evenly spaced matching set, which is what a clapper emits. Opposing segment drifts that exceed the total budget but cancel at the endpoint are printed and recorded in JSON as a reliability finding even when the endpoint gate passes.

### The reference has to qualify before there is a verdict

Before any budget is applied, the clapper signal is checked for whether it can carry the judgement at all. Three conditions, and the run reports **exit 3, could not measure** (never a pass) when any of them fails:

- **At least three markers.** Two define a line exactly; their residuals are zero by construction and nonlinearity is invisible.
- **A slope uncertain by well under the budget.** Each edge is located to a finite precision, and that propagates into the fitted drift. When the fit is uncertain by more than a third of the budget, "within budget" and "too noisy to tell" are the same measurement.
- **Residuals inside what the edges allow.** A marker further from the fitted line than its own edge uncertainty means the offset did not move linearly, and then no single rate describes the run.

`--unqualified-reference` prints the numbers with a verdict anyway. It is a diagnostic switch, not a way past a gate.

### How many markers a budget needs

An edge is located to the precision of its own sampling grid, sharpened by interpolating the threshold crossing and widened by the baseline noise. At 60 fps video and a 10 ms astats window, a clean marker's offset is good to roughly ±5 ms.

The uncertainty of the **total drift** does not shrink with a longer run. A longer span determines the *rate* proportionally better and is then multiplied by that same longer span, so the two cancel exactly. Only more markers, or sharper edges, help, which is why a campaign that lengthens a run to make a verdict possible is doing nothing. With `n` evenly spaced markers each good to `σ`, the total drift is uncertain by `σ · sqrt(12(n-1) / (n(n+1)))`:

| markers | multiplier | at σ = 5 ms | at σ = 9 ms |
|---|---|---|---|
| 3 | 1.41 | ±7.1 ms | ±12.7 ms |
| 5 | 1.26 | ±6.3 ms | ±11.3 ms |
| 9 | 1.03 | ±5.2 ms | ±9.3 ms |
| 20 | 0.74 | ±3.7 ms | ±6.6 ms |

A 20 ms budget needs the uncertainty under about 6.7 ms, so it wants five markers at clean edges and twenty at noisy ones, and the three the schedule used to be limited to are not enough at either. When the reference does not qualify, the verdict names the count that would reach it at the edge precision it actually measured.

**Only the drift is pass/fail.** The absolute `offset_start` carries a device-dependent **emission skew** (~10-50 ms: GPU present \u2192 display capture vs. WASAPI render \u2192 SYS loopback) that is *not* an ExoSnap error and cancels in the drift: it is the fit's intercept. So the absolute offset is reported **advisory**. The exit code is driven by the fitted drift (`0` within budget \u00b7 `2` over \u00b7 `3` unmeasurable, which now includes an unqualified reference). Hardening the absolute offset would need a one-time calibrated emission-skew subtraction for the setup.

This is the drift **acceptance method for `av-clock-slaving`**: run it before clock-slaving to measure the drift, after to prove the compensation. It is **not** an acceptance of the absolute start offset.

Requires a **full system ffmpeg** (the app bundles a mux-only FFmpeg without the `signalstats`/`astats`/`silencedetect` filters this needs). Pin your ffmpeg version in a real acceptance run. HDR sessions tone-map the flash, so run HDR A/V-sync separately.

### CI regression guard

The `dev-scripts` CI job provisions + version-verifies system ffmpeg and runs the analyzer against a committed golden clip (`tests/fixtures/av-sync/ clapper-golden.mp4`, ~20 KB, five markers over 4 s, checked against a 25 ms budget). Regenerate it with `python scripts/dev/gen-av-sync-fixture.py` when the analyzer changes. `--markers` and `--duration` control the schedule.

Five markers, not two. The clip's flash and beep are emitted on one synthetic timeline, so its *true* drift is zero, but the 60 fps frame grid quantises each flash edge, and with two markers the resulting uncertainty is worse than the budget the run would be judged against, so the analyzer correctly refuses a verdict. A fixture that only ever exercises that refusal guards nothing. At five markers the fit is certain to about \u00b16 ms and the clip passes on its measured drift rather than on a budget widened until it did.

The residual drift the clip does report is the frame grid, not a real A/V budget: this guards the *script*.

---

## 3. Recovery drills

Adversarial tests of the recovery machinery (`RecoveryService.Scan/Finish`, repair-remux, durability flush) across the matrix **{Recording, Finalize, Remux} × {Ordered-Stop, Process-Kill, Powerloss}**.

### CI drills (deterministic, no GPU)

- `recovery.recovery_drill_tests`: drives `RecoveryService` against **real** synthetic-pipeline MKVs (real `MatroskaStreamWriter`). Ordered-Stop \u2192 clean finalize \u2192 rename \u2192 demuxable. Process-Kill (modelled by truncating a finalized MKV: its committed clusters are exactly what a killed recording leaves) \u2192 `RemuxToMkv` repair must return and either repair or preserve the artefact. MP4-intended \u2192 progressive remux.
- `recorder_core.recovery_truncation_tests`, the **powerloss proxy**: truncate a genuinely-flushed MKV at assorted offsets and assert the repair never crashes and salvages up to the last complete cluster. Models "the tail is gone". It does **not** prove the durability window.

**What CI does NOT cover:** the real `RecordingCoordinator` Add/`UpdateFinalized`/`Remove` manifest choreography: the drill child reproduces that ordering, it does not execute it (the real coordinator needs a GPU). An ordering bug in the coordinator's crash window stays invisible in CI. Only the live-kill drill below catches it.

### Live drills (user-live, the only proof of the durability promise)

Per phase (Recording / Finalize / Remux), against the **real app**:

1. **Process-kill:** `TerminateProcess` (or Task Manager) mid-phase, restart, confirm the recovery overlay lists the entry and `Finish` recovers it.
2. **Powerloss:** a real hard-reset / VM forced-off mid-phase. Only this proves the ≤ 2 s (`kDurabilityFlushInterval`) + reorder-window + one non-rendered cluster loss model actually holds and the file recovers.

### Resolved: stale partial MP4 at the target path

The MP4 Remux×Kill drill (`RemuxMp4ProcessKill_ReplacesStalePartialAtTargetPath`) was a deliberate **xfail** and is now a hard assertion.

> Previously `RecoveryService.Finish` remuxed **directly** to the final,
> user-visible MP4 path. A kill/powerloss mid-remux left a corrupt half-MP4 exactly
> where the user expected their result, and next launch recovery produced the good
> MP4 under a *different* name (`ResolveUniqueOutputPath`) and **left the corrupt
> file behind**. No data was lost (the MKV was still the source of truth), but the
> user saw a broken file at the target path.

`Finish` now remuxes to a sibling `.part` temp on the target's own volume and atomically renames it onto the target with `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`. A corrupt partial already at `final_output_path` (this recording's own interrupted remux) is overwritten in place. A crash *during* the recovery remux leaves only the `.part` temp, never a half-written file at the target. The same temp+atomic-rename guard is applied to the MKV repair-remux path.

The **live** `RecordingCoordinator` remux (`RunRemuxJob` / `StartSegmentRemuxThread`) now carries the identical guarantee: it remuxes to a sibling `.part` temp on the target's own volume and atomically renames it onto the final MP4 on success, using the same `MakeSiblingTempPath` / `AtomicReplaceInPlace` primitives (shared in `app/services/AtomicFileOps.*`). A kill mid-remux leaves only the `.part` temp: the user-visible output path never holds a half-written MP4. The live-path drills `LiveRemuxMp4_NeverWritesTargetMidFlightThenPublishesAtomically` and `LiveRemuxMp4_CancelLeavesTargetUntouchedAndRemovesTemp` exercise that sequence (including a mid-flight check that the target stays empty until the atomic publish).
