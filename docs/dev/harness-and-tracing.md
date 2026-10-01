# Harness modes and tracing

Use the smallest instrument that can observe the property. These switches observe or invoke application-owned operations; they are not permission to synthesize input, take focus or administer Windows. Coordinate any interactive/native desktop run under [AGENTS](../../AGENTS.md).

## Window and preview diagnostics

| Instrument | What it observes | Passing boundary |
|---|---|---|
| `exosnap.exe --hwnd-audit` | Child HWND count, native styles and non-client inset | No native child windows over the Quick surface, no duplicate caption, native resize frame retained |
| `--window-trace` or `EXOSNAP_WINDOW_TRACE=1` | Persisted/resolved/pre-show/expose geometry in Qt and native coordinates | Correct final rectangle before visibility, no corrective first-frame jump |
| `--cursor-audit` | Declared pointing-hand controls versus Qt and OS cursor state | Enabled visible controls agree; disabled/clipped controls counted separately |
| `--window-maximize-cycle` | Native maximize, minimize and restore state/rectangles | Real zoom state, saved normal rectangle preserved, resize edges only while windowed |
| `EXOSNAP_PREVIEW_TRACE=1` | Producer, wakeup, update, render and outstanding-frame debt | Owed frame reissued after expose/screen/scene-graph transitions without a second capture frame |

`--hwnd-audit` exits success only when its native checks hold. An offscreen Qt test has no HWND and cannot establish them. The title band's visual height is not evidence of non-client inset. `WS_THICKFRAME` and native placement state matter even when the window looks correct.

`--cursor-audit` does not move the real pointer. It delivers hover information to the application's own window, reads Qt/native cursor results and restores the cursor. `never-fired` points to event delivery; `os-lost-it` points to native presentation. Live mouse activity can affect observation, so coordinate ambiguous runs rather than disabling the check.

The maximize cycle checks windowed resize edges, maximized non-resize edges, minimize/restore-to-maximized and exact normal-rectangle restoration. Exit codes distinguish state (1), geometry (2), absent window (3), normalization (4), hit test (5) and minimize/restore (6). The real Windows title-bar drag loop still requires an interactive check.

A preview trace line carries `owed`, `reissued`, publishes/wakeups/updates/renders. `owed=1` means a publication has not been followed by a render; a lifecycle event must service that debt. `preview.snapshot` on [Live Verify](live-verify.md) exposes structured counters and is preferred for automated assertions. A still screenshot cannot establish producer cadence or missing redraws.

## Deterministic visual capture

`--visual-test <path>` seeds a scenario, captures it and exits using isolated configuration. It can also save individual overlay scene-graph images. Those images prove scene content, not actual desktop alpha/composition or capture exclusion.

| Option | Purpose |
|---|---|
| `--visual-test-size WxH` | Explicit logical window size |
| `--visual-delay-ms N` | Bounded capture delay for construction |
| `--visual-page N` | Destination in Record, Settings, Diagnostics, Logs, About order |
| `--visual-appearance dark\|light`, `--visual-accent <id>` | Explicit appearance/accent |
| `--visual-shell-appearance dark\|light` | Shell-owned toast appearance, separate from app preference |
| `--visual-expert` | Settings Expert rows |
| `--visual-scroll F` | Fraction of current page's scrollable range |
| `--visual-popup source-picker\|notification-hub` | Lazy popup |
| `--visual-dialog <name>` | Named close/preset dialog |
| `--record-visual-state <name>` | Named Record lifecycle state |
| `--overlay-visual-state <name>` | Named recovery, error or crash surface, recording HUD variant (`hud-*`) or single desktop toast (`toast-success\|caution\|error\|info`) |
| `--desktop-pattern` | Deterministic source content on the desktop for the live preview. Implies `--visual-live-preview` |
| `--visual-live-preview` | Real capture in the preview instead of the test card |
| `--visual-full-page` | The window as if it were tall enough for the whole scrollable page |
| `--visual-expand-all` | Opens every collapsed element on the visible page before the capture |

With a selected source, the preview shows a synthetic 1920x1080 test card and opens no DXGI or WGC capture, so a screenshot is reproducible and never carries the developer's desktop. A state without a source keeps its empty stage. The live preview is opt-in and then photographs whatever is on the screen.

Use the registered scenario names from `app/visual_tests` rather than inventing a QML object name. Content seeds include `EXOSNAP_VISUAL_EDIT_SCENARIO`, `EXOSNAP_VISUAL_LOG_SCENARIO`, `EXOSNAP_VISUAL_DIAG_SCENARIO`, `EXOSNAP_VISUAL_DIAG_LIVE`, `EXOSNAP_VISUAL_NOTIFICATION_SCENARIO` and `EXOSNAP_VISUAL_SOURCE_SCENARIO`.

Page capture waits for the requested asynchronous loader to be ready, with a bounded failure log. Some overlay construction still depends on the capture delay; a missing/not-instantiated overlay is not a passed visual check. Diagnostics fixtures use a declared canonical capability set and apply the scenario's deviations. They are not observations of the host GPU.

The Edit visual fixture does not open real media. Use `--auto-edit` with real media for decoder/export behavior. A correct placeholder screenshot does not establish playable video, synchronized audio or successful output.

`cargo exo-dev screenshot` drives this harness. Without a subcommand it takes every named set in the appearance and accent of the developer's own ExoSnap settings. Only those two values are read from there, and every shot still runs in a throwaway configuration. `sweep --set <names>` takes named sets (`list` shows them) in both appearances, and `shot` takes one screenshot from options that mirror the flags and seeds above. All three multiply every shot by `--appearance`, `--accent`, `--size` and `--scale` when those are given. Record states and accents are checked against the product sources, because the app renders an unknown value as a plausible default and still exits 0.

By default, output goes to the local screenshot directory reported by the command, in a `<commit date>_<short hash>` subdirectory, with `-dirty` for a tree with uncommitted changes and `_<label>` for `--label`, unless `--out-dir` is given. Each page has a subdirectory holding `<state>_<appearance>-<accent>_<size>.png` and that shot's overlay grabs. A later run into the same directory replaces the shots it retakes and keeps the others, and `manifest.json` (binary path, build time, SHA-256, HEAD, dirty flag, version, exact invocation per shot) and the `index.html` contact sheet always describe the whole directory. A shot whose log shows a harness option that found nothing to act on, or an unknown record or overlay state, is reported as `warn`, not `ok`.

The `settings` and `diagnostics` sets take each page in Simple and Expert, each as the window, as the full page (`full`) and as the full page with every collapsed element open (`full-expanded`). The full page stitches the window above and below the page's scroller around the scroller's complete content, so no card falls between two scroll positions.

The overlay windows are saved as separate files because they sit at their own desktop positions. For the `hud` set, the run also composes each theme and scale's overlay grabs into `overlays/sheet_<theme>_<size>.png`, one row per shot on a checkerboard. It compares windows side by side and does not reproduce their desktop placement. Overlays follow the display scale, not the window size, so vary `--scale` for them.

```powershell
cargo exo-dev screenshot
cargo exo-dev screenshot shot --page settings --dialog preset-rename
cargo exo-dev screenshot sweep --set pages,record --size 1600x1000,860x700
cargo exo-dev screenshot sweep --set hud --scale 1,1.5,2
```

The run holds the build tree's lock, so a concurrent build cannot relink the binary mid-sweep. Scrolling and dialogs are applied only after the active page's loader is Ready, so the default four parallel jobs do not lose them to a slow page load.

## Recording harness

`--auto-record` runs the real coordinator inside the normal Quick frontend. `--auto-record-bare` requires it and uses a `QCoreApplication` without a QML engine, window or preview. The bare form measures recorder work without the competing GUI, not the complete user's experience.

```powershell
$env:EXOSNAP_OUTPUT_DIR = Join-Path $env:TEMP 'exosnap-measurement'
exosnap.exe --auto-record --auto-record-bare --target monitor --duration 30 --frame-rate 60 --container mkv --video-codec av1 --audio-codec opus --audio-rows sys
```

Use isolated configuration as well when the selected mode does not provide it. Harness outputs are scratch artifacts, not source files or normal user recordings. `--cq <n>` selects a canonical quality value for a measurement, independently of the named ladder.

`--capture-backend default|wgc` is a developer override for a monitor target. Default chooses the product's display duplication path; `wgc` measures WGC on the same monitor. It is refused for other target kinds and is never persisted. It is not a supported end-user backend selector.

Harness-only modes require a non-Release configuration or a Release configured with `EXOSNAP_BUILD_BENCHMARK_HARNESS=ON`. Do not use a shipping release binary as though the harness was enabled. The presence of an option string in the executable is insufficient evidence that its guarded implementation is available. Official release recording checks use [Live Verify](live-verify.md).

The capture log records requested/effective WGC minimum update interval, support and target rate. Hub preview can report an unspecified target rate because it is producer-driven rather than the recorder's cadence.

## Crashes and memory lifetime

Build the ASan preset described in [Build and test](build-and-test.md) for suspected use-after-free or invalid-memory access. Keep the report, allocation/free stacks and exact binary identity together. Release PDBs and crash dumps are separate artifacts; neither belongs in the portable user package.

[Soak and recovery drills](soak-and-recovery-drills.md) cover endurance and interruption. [Encoder-quality measurement](encoder-quality-matrix.md) covers objective file comparisons. Neither a visual fixture nor a short launch smoke substitutes for those tests.

## Focused performance measurements

Set `EXOSNAP_PERFORMANCE_TRACE=1` for aggregate decode readback, normalization, CPU conversion, GPU upload, shader compilation, resource creation, queue wait, decoder/presentation drops, mailbox replacement and audio polling counts. Durations measure host-side calls, including any blocking, rather than fenced GPU execution. GPU stage timings separately cover luminance, tone mapping, composition and conversion. Unmeasured stages remain unavailable.

The `probe_edit_playback` target accepts `--decode-only` or `--decode-upload` after the media path and start timestamp arguments. The latter uses the real GPU frame converter without Qt presentation. `EXOSNAP_EDIT_SOFTWARE_DECODE=1` permits a software comparison. These runs measure callback throughput, not display cadence or seek-to-visible latency. Use representative codecs, resolutions and bit depths and retain the exact fixtures with the measurement.

`--preview-benchmark <report.json> --benchmark-seconds <seconds>` measures preview publication and consumption plus process CPU. Optional `EXOSNAP_PREVIEW_BENCHMARK_STATE=record|settings|minimized`, `EXOSNAP_PREVIEW_BENCHMARK_RATE=0|15|30|60|120` and `EXOSNAP_PREVIEW_BENCHMARK_ANIMATION=0|1` select the isolated measurement state. Reports include logical persistent surface bytes. Keep the source stimulus fixed and avoid competing builds during measurements.

Development harness builds accept `EXOSNAP_QML_PROFILE=1` to expose only QML profiler services. `--navigation-lifecycle-test` records first and warm page-ready times and can run with `QT_QPA_PLATFORM=offscreen`. Profiled construction costs include instrumentation overhead. Real click-to-visible latency and idle preview resource measurements require the actual desktop. Compare the application closed, Record visible, another page, minimized, preview Off/limited and recording without inferring game-FPS impact from preview activity alone.
