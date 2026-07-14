# Manual smoke: bare auto-record mode

This step needs a real GPU/NVENC and produces a real recording file, so it cannot
run in CI as a gtest. Run it by hand on a machine with an NVENC-capable GPU.

## Command (cmd.exe)

```
set EXOSNAP_OUTPUT_DIR=%TEMP%\exosnap-auto-record-smoke
mkdir "%EXOSNAP_OUTPUT_DIR%"
exosnap.exe --auto-record --target monitor --audio-rows sys --duration 5
```

## Command (Git Bash / PowerShell-equivalent)

```
export EXOSNAP_OUTPUT_DIR="$TEMP/exosnap-auto-record-smoke"
mkdir -p "$EXOSNAP_OUTPUT_DIR"
./exosnap.exe --auto-record --target monitor --audio-rows sys --duration 5
```

## Expected

- The process runs headless (no window, no interaction) for ~5 seconds plus a short
  finalize/remux grace period, then exits with code 0.
- Exactly one JSON line is printed to stdout, e.g.:
  ```json
  {"status":"ok","output_path":"C:\\...\\exosnap-auto-record-smoke\\...mkv","session_report_path":"","error_detail":""}
  ```
  `status` is `"ok"` and `output_path` points under `%TEMP%\exosnap-auto-record-smoke`.
- `ffprobe` on the reported `output_path` shows one video stream and one audio stream:
  ```
  ffprobe -hide_banner -show_streams "<output_path>"
  ```

## Failure modes (each still prints one JSON line and exits non-zero)

- `{"status":"error",...,"error_detail":"no matching capture target ..."}` — no
  monitor/window matched the requested target.
- `{"status":"error",...,"error_detail":"StartRecording refused ..."}` — the
  coordinator rejected the start (capability block or bad state).
- `{"status":"error",...,"error_detail":"recording unavailable ..."}` — the
  requested format failed capability validation.
- A timeout with no result before the grace period elapses also exits non-zero.
