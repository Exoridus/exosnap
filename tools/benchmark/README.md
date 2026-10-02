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
