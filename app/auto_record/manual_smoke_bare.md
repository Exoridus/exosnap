# Bare recording-harness smoke

This checks the headless recorder harness, not official release acceptance. Use a non-Release build or a Release configured with `EXOSNAP_BUILD_BENCHMARK_HARNESS=ON`. The target needs actual supported capture/NVENC hardware.

```powershell
$env:EXOSNAP_CONFIG_DIR = Join-Path $env:TEMP 'exosnap-bare-config'
$env:EXOSNAP_OUTPUT_DIR = Join-Path $env:TEMP 'exosnap-bare-output'
& '<harness-enabled exosnap.exe>' --auto-record --auto-record-bare --target monitor --duration 10 --frame-rate 60 --container mkv --video-codec av1 --audio-codec opus --audio-rows sys
```

Confirm the process creates no application window/preview, records through the real coordinator, returns a completed result and produces inspectable media at the isolated output. Use ffprobe to check the selected streams and duration. Compare logs for the same configuration with and without `--auto-record-bare` when measuring frontend overhead; do not compare unmatched hardware, source content or load.

`--auto-record-bare` without `--auto-record` is invalid. A successful bare run does not prove QML interaction, preview parity or native window behavior. See [Harnesses](../../docs/dev/harness-and-tracing.md) and [Live Verify](../../docs/dev/live-verify.md) for the other layers.
