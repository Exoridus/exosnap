# Recording benchmark tools

These are developer measurement scripts, not product runtime. They launch a declared external workload, run a harness-enabled recorder, collect artifacts and analyze the measured interval. They must not teach the product how to start third-party benchmark applications.

## Run the current frontend

ExoSnap's current frontend is Qt Quick. Use an explicit current executable and a scenario from `scenarios/`:

```powershell
pwsh tools/benchmark/Invoke-BenchmarkRun.ps1 -Frontend quick -Scenario '<scenario name>' -QuickExe '<harness-enabled exosnap.exe>' -OutputRoot '<evidence directory>'
```

Accepted measurements require a Release configured with `EXOSNAP_BUILD_BENCHMARK_HARNESS=ON`. `-Calibration` permits a disposable Debug/tooling check but excludes it from accepted campaign statistics. `-SkipTopologyCheck` is a tooling escape, not an accepted measurement setting. Inspect the scenario's topology/workload preconditions before launching it on a shared desktop.

The scripts retain an explicit alternate-executable comparison interface. There is no Widgets frontend built by this tree. Do not run a default two-frontend campaign without providing both intended artifacts, and never label the Quick binary as another frontend merely to satisfy a script parameter.

## Measurement contract

The application owns benchmark warmup, measurement duration and stop. The orchestration owns workload lifetime and evidence collection. Keep executable hash/build configuration, effective recording config, source content, topology, tool versions and workload identity with every result. A run that silently changed any of these is not a controlled comparison.

Use the same source and resolution, codec/rate/quality, audio routing, capture backend and machine state on both sides. Compare independent repeats rather than one favorable run. Report failures, calibration runs and unavailable scenarios separately. Check that the harness actually exists in the binary; an option string alone is insufficient.

The bare recorder mode measures without frontend/preview competition, whereas normal auto-record measures the application path. These answer different questions. [Harnesses](../../docs/dev/harness-and-tracing.md) explains the switches; [encoder-quality measurement](../../docs/dev/encoder-quality-matrix.md) covers file-quality comparisons.

Store generated media, JSONL, CSV, reports and screenshots in an untracked evidence directory or review attachment, not permanent documentation. Promote only a measured current constraint or selected default's rationale.
