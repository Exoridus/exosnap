# Frontend A/B benchmark tooling

Development-only orchestration for the Qt Widgets vs. Qt Quick frontend
comparison. Nothing in here ships. The application knows how to record and how
to report on itself; it does not know what Superposition is, what a run
identity is, or where artifacts live. That boundary is deliberate — the moment
`QuickApplication` learns to launch a benchmark, the benchmark stops measuring
the product.

## What measures what

| Layer | Owner | Produces |
|---|---|---|
| Recording engine + preview instrumentation | `app/benchmark/`, driven by `auto_record::RunAutoRecordOnCoordinator` | `exosnap.json` (one per run) |
| External GPU workload | Superposition Benchmark 1.1 Pro CLI | `superposition.csv`, `superposition.txt` |
| Run identity, topology assertions, artifact layout | the scripts here | `run.json`, `scenario.json` |

`Superposition FPS` is **not** `ExoSnap captured FPS`. The comparison report
keeps them in separate sections for that reason.

## Prerequisites

Both frontends must be built from a **Release** configuration with the harness
explicitly enabled:

```powershell
cmake -S . -B build/windows-x64-release-bench `
  -DCMAKE_BUILD_TYPE=Release `
  -DEXOSNAP_BUILD_QUICK_SPIKE=ON `
  -DEXOSNAP_BUILD_BENCHMARK_HARNESS=ON
cmake --build build/windows-x64-release-bench --config Release --target exosnap exosnap_quick_spike
```

`EXOSNAP_BUILD_BENCHMARK_HARNESS` adds the automation code and changes nothing
else — no optimisation flag, no runtime policy. A Debug binary is rejected by
`Compare-BenchmarkRuns.ps1`; measuring two unoptimised builds says nothing about
what users run.

## Scripts

| Script | Purpose |
|---|---|
| `BenchmarkTopology.psm1` | Reads the real display topology (primary flag, pixel mode, EDID model) and asserts the scenario's expectation. Aborts rather than benchmarking the wrong monitor. |
| `Invoke-SceneSurvey.ps1` | One-time Superposition scene calibration. Runs candidate scenes without ExoSnap and reports FPS, low-tail FPS, frame-to-frame variability and headroom against 144 Hz. |
| `Invoke-BenchmarkRun.ps1` | A single run: topology check → workload → ExoSnap `--auto-record` → artifact collection → acceptance check. |
| `Invoke-BenchmarkCampaign.ps1` | Alternating multi-run campaign (default `W Q Q W W Q`) with cooldowns. |
| `Compare-BenchmarkRuns.ps1` | Builds the comparison dataset. Refuses to compare runs whose effective recording configuration differs. |

## Two rules the tooling enforces for you

**Effective configuration, not command lines.** Every report carries
`effective_recording_config.fingerprint`, a digest over the `RecorderConfig` the
engine was actually handed (`RecordingCoordinator::LastCommittedRecorderConfig`).
An earlier campaign compared two runs launched with identical flags that were
nevertheless recording differently, because one frontend seeded
`OutputSettingsModel::Defaults()` on its way to `StartRecording`. Comparing flags
could not have caught it. `Compare-BenchmarkRuns.ps1` diffs the fields and
aborts.

**Comparability classes.** Deltas are computed only for metrics marked
`identical`. `approximate` metrics are printed side by side with their probe
text; `frontend_only` metrics are never subtracted. A Widgets preview "frame" is
one swap-chain Present of a quad; a Quick preview "frame" is a scene-graph render
of the whole window. Their difference is not a performance result.

## Physical setup

The canonical machine has two displays on the RTX 5070 Ti:

* **Display 1 — LG 27GL850**, primary, physically left, 2560×1440 @ 144 Hz,
  10-bit RGB, SDR. Superposition renders here; ExoSnap captures here.
* **Display 2 — LG 27GL650F**, physically right, 1920×1080 @ ~144 Hz, 10-bit
  RGB, SDR. ExoSnap's window lives here, visible and un-minimised, so the
  application's real cost is measured without the application appearing inside
  the image it is capturing.

Window placement is not scripted from the outside: both frontends resolve it
through `benchmark::ResolveHarnessWindowPlacement()`, so "same logical size,
equivalent placement" is a property of the binaries rather than of a script that
could drift.

Keep the Windows graphics configuration fixed across a campaign: HDR off, SDR on
both displays, 10-bit desktop signal, hardware-accelerated GPU scheduling on,
VRR on, optimisations for windowed games on, dynamic refresh rate off.

## Typical sequence

```powershell
# 1. Choose the deterministic scene (once).
.\Invoke-SceneSurvey.ps1 -Scenes 4,5,8,12,15 -Quality high
#    Freeze the winner in scenarios/superposition-1440p144-headroom.json and set
#    calibration_status to "frozen".

# 2. One disposable validation run. Never counted.
.\Invoke-BenchmarkRun.ps1 -Frontend quick -Scenario superposition-1440p144-headroom -Calibration

# 3. The campaigns.
.\Invoke-BenchmarkCampaign.ps1 -Scenario desktop-idle-1440p144
.\Invoke-BenchmarkCampaign.ps1 -Scenario superposition-1440p144-headroom

# 4. The comparison.
.\Compare-BenchmarkRuns.ps1 -Scenario desktop-idle-1440p144
.\Compare-BenchmarkRuns.ps1 -Scenario superposition-1440p144-headroom
```

Artifacts land under `.workspace/benchmark-results/<scenario>/<frontend>-runNN/`
and are untracked. They are the historical reference for the Widgets frontend
once it is removed, so preserve the raw files — not just a summary.

## Not part of the A/B

No Y4M or raw-frame dumping during a frontend comparison. It changes disk I/O,
CPU and memory, and the campaign is supposed to measure normal production-style
recording. The encoding-corpus workflow is a separate exercise built on top of
an accepted recording.
