# Recording benchmark tools

These are developer measurement scripts, not product runtime. They launch a declared external workload, run a harness-enabled recorder, collect artifacts and analyze the measured interval. They must not teach the product how to start third-party benchmark applications.

The orchestration lives in `cargo exo-dev benchmark`, under `tools/exo-dev/src/benchmark/`. Scenario definitions stay in `scenarios/` as plain JSON; nothing here launches anything on its own.

## Run the current frontend

ExoSnap's current frontend is Qt Quick. Use an explicit current executable and a scenario from `scenarios/`:

```powershell
cargo exo-dev benchmark run --frontend quick --scenario '<scenario name>' --quick-exe '<harness-enabled exosnap.exe>' --output-root '<evidence directory>'
```

Accepted measurements require a Release configured with `EXOSNAP_BUILD_BENCHMARK_HARNESS=ON`. `--calibration` permits a disposable Debug/tooling check but excludes it from accepted campaign statistics. `--skip-topology-check` is a tooling escape, not an accepted measurement setting. Inspect the scenario's topology/workload preconditions before launching it on a shared desktop.

`cargo exo-dev benchmark topology --scenario '<scenario name>'` asserts the scenario's expected capture/UI displays without launching anything.

The command retains an explicit alternate-executable comparison interface (`--widgets-exe`). There is no Widgets frontend built by this tree. Do not run a default two-frontend campaign without providing both intended artifacts, and never label the Quick binary as another frontend merely to satisfy a parameter.

`cargo exo-dev benchmark campaign --scenario '<scenario name>'` runs the alternating campaign. Its default order is six Quick runs (the Widgets frontend was removed with the Qt Quick cutover, so the default never names it); pass `--order widgets,quick,...` explicitly when both artifacts are available.

## Idle recording matrix

The generic idle scenarios use H.264, MKV, Opus system audio, SDR 8-bit 4:2:0, a 15-second recording warmup and a 60-second measurement window. They require a primary capture display in the stated mode and a separate display for the frontend. The secondary display dimensions are unrestricted. The recorder's selected device must support the effective configuration.

| Scenario | Primary capture dimensions | Minimum reported refresh | Requested recording rate |
| --- | --- | --- | --- |
| `desktop-idle-1080p60` | 1920 x 1080 | 60 Hz | 60 fps |
| `desktop-idle-1440p60` | 2560 x 1440 | 60 Hz | 60 fps |
| `desktop-idle-2160p60` | 3840 x 2160 | 60 Hz | 60 fps |
| `desktop-idle-1080p144` | 1920 x 1080 | 144 Hz | 144 fps |

Set the capture display mode before selecting each scenario. Keep the desktop image, source activity, preview geometry, scaling, system audio activity and power state fixed across repeats. These cases measure idle recorder and visible-preview overhead. An idle source normally exercises CFR repeats, so retain unique captures, intentional repeats, coalescing and actual losses separately. The requested packet cadence alone does not establish unique-frame capture throughput.

From the configured Windows build environment, use an explicit Release executable built with `EXOSNAP_BUILD_BENCHMARK_HARNESS=ON` and a fresh evidence directory:

```powershell
$scenario = 'desktop-idle-1080p60'
$product = (Resolve-Path '.\build\windows-x64-ninja-release\app\exosnap.exe').Path
$out = '.workspace/benchmark-results/idle-1080p60-baseline'
Remove-Item Env:\EXOSNAP_STAGE_A_CACHE -ErrorAction SilentlyContinue
Remove-Item Env:\EXOSNAP_DEMAND_COMPOSITION -ErrorAction SilentlyContinue
cargo exo-dev benchmark topology --scenario $scenario
if ($LASTEXITCODE -ne 0) { throw 'The requested capture topology is unavailable.' }
cargo exo-dev benchmark campaign --scenario $scenario --quick-exe $product --output-root $out
```

Replace the executable path with the actual harness build. The command keeps the normal topology, Release/harness and run-acceptance checks enabled. It uses the desktop directly and requires no external benchmark application. Repeat for the other three modes when the display and encoder can support them. Keep an unsupported mode as an explicit missing result.

For Stage A and demand-composition experiments, first fix a representative scene and downstream output geometry. Record both switch values with each run and use separate output directories for baseline, Stage A only, demand only and both. The idle scenarios supply the overhead control. Stage A additionally needs actual overlay composition and pixel parity checks. Demand reduction additionally needs larger capture dimensions than recording output, an eligible SDR configuration and controlled preview demand. The scenario loader currently does not configure output dimensions or overlay populations. Supply those conditions through a separately configured product run or focused native compositor tests before extending the comparison. [Harnesses](../../docs/dev/harness-and-tracing.md) documents the available experiments and their eligibility.

Retain CPU time per output frame and per unique capture, submission wall time, measured GPU stage time and query coverage, queue/synchronization waits, P50/P95/P99, encoder latency, memory and decode parity where the selected harness exposes them. Missing measurements remain unavailable. Logical surface allocation and resident VRAM are separate quantities. Keep timing runs free of competing builds, alternate variant order, report per-run variation and extend selected cases to at least ten minutes to check growth and backpressure before making a performance claim.

## Comparing runs and calibrating scenes

```powershell
cargo exo-dev benchmark compare --run-dir '<run directory>' --run-dir '<run directory>'
```

Rejects a set whose accepted runs recorded different `effective_recording_config` fingerprints (a hard error, never a silent skip), and computes a delta only for a metric whose comparability the manifest schema itself marks `identical`. An `approximate` metric is reported side by side with no delta; a `frontend_only` metric is never subtracted. Emits both a console table and `comparison.json` next to the run directories.

```powershell
cargo exo-dev benchmark scene-survey --scene 4 --scene 5 --scene 8 --superposition-cli '<superposition_cli.exe>'
```

Surveys candidate Superposition scenes without ExoSnap running, which is also the headroom measurement (how much GPU is left for the capture path). Ranks scenes by FPS variability: a scene with a long static stretch is not stressing a capture path, whatever its average score.

## Measurement contract

The application owns benchmark warmup, measurement duration and stop. The orchestration owns workload lifetime and evidence collection. Keep executable hash/build configuration, effective recording config, source content, topology, tool versions and workload identity with every result. A run that silently changed any of these is not a controlled comparison.

Use the same source and resolution, codec/rate/quality, audio routing, capture backend and machine state on both sides. Compare independent repeats rather than one favorable run. Report failures, calibration runs and unavailable scenarios separately. Check that the harness actually exists in the binary; an option string alone is insufficient.

The bare recorder mode measures without frontend/preview competition, whereas normal auto-record measures the application path. These answer different questions. [Harnesses](../../docs/dev/harness-and-tracing.md) explains the switches; [encoder-quality measurement](../../docs/dev/encoder-quality-matrix.md) covers file-quality comparisons.

Store generated media, JSONL, CSV, reports and screenshots in an untracked evidence directory or review attachment, not permanent documentation. Promote only a measured current constraint or selected default's rationale.
