# probe_wgc_preview

## Purpose

Prove that Windows Graphics Capture (WGC) can capture a selected monitor or window, expose live frame timing data, and drive a minimal preview window through a D3D11 texture path.

This is an isolated capability probe, not production recording code.

## Build

```pwsh
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug --target probe_wgc_preview
```

The executable is produced at:
`build/windows-x64-debug/apps/probes/probe_wgc_preview/<Configuration>/probe_wgc_preview.exe`

## Run

```pwsh
.\build\windows-x64-debug\apps\probes\probe_wgc_preview\Debug\probe_wgc_preview.exe
```

On startup the probe:
1. Enumerates all attached desktop monitors, then
2. Enumerates all visible top-level windows with a non-empty title.
3. Prints a numbered list of targets.
4. Prompts for a target number or `q` to quit.
5. Opens a preview window and begins capture of the selected target.
6. Closes cleanly when the preview window is closed.

## Expected manual validation

1. Run the probe.
2. Select a **monitor** target and confirm:
   - A preview window opens.
   - Live frames from the selected monitor appear in the window.
   - Console prints 1-second metrics (target kind, content size, FPS, total frames, dropped=n/a).
   - Closing the preview window prints a run summary and exits cleanly.
3. Select a **window** target (e.g. Notepad, File Explorer) and confirm the same behavior for that window.
4. (Optional) While capturing a window, close the source window and observe that the probe stops gracefully.

## Output fields

### 1-second console metrics

```
[metrics] target=<monitor|window> size=<W>x<H> fps=<f> total=<N> dropped=n/a
```

| Field    | Meaning                                           |
|----------|---------------------------------------------------|
| target   | `monitor` or `window`                             |
| size     | current captured content size (pixels)            |
| fps      | source FPS over the last ~1 second interval       |
| total    | total captured frame count since capture start    |
| dropped  | `n/a` — drop/skip measurement is not reliable at probe level |

### Run summary (on exit)

```
=== SUMMARY ===
  target kind        : <monitor|window>
  duration (s)       : <f>
  total frames       : <N>
  avg fps            : <f>
  source loss        : <yes|no>
=================
```

## Known limitations

- Drop/skip frame counts are not measured; `n/a` is printed.
- No encoder, audio, muxing, diagnostics, or settings code.
- The preview window uses a simple CopyResource + swap-chain present path; no HDR or format conversion.
- Only B8G8R8A8_UNORM format is used.
- The probe is Windows-only by design.
- Resize of the capture source triggers a swap-chain recreation on the next frame; brief visual glitch is expected.
- If the target window is minimized or off-screen, WGC may deliver stale or no frames.
- Frames are retrieved via `TryGetNextFrame()` polling in the main loop, not via the `FrameArrived` event (which requires a DispatcherQueue).
