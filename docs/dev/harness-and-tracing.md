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
| `--overlay-visual-state <name>` | Named recovery/error/crash surface |
| `--desktop-pattern` | Deterministic source content behind the preview |

Use the registered scenario names from `app/visual_tests` rather than inventing a QML object name. Content seeds include `EXOSNAP_VISUAL_EDIT_SCENARIO`, `EXOSNAP_VISUAL_LOG_SCENARIO`, `EXOSNAP_VISUAL_DIAG_SCENARIO`, `EXOSNAP_VISUAL_DIAG_LIVE`, `EXOSNAP_VISUAL_NOTIFICATION_SCENARIO` and `EXOSNAP_VISUAL_SOURCE_SCENARIO`.

Page capture waits for the requested asynchronous loader to be ready, with a bounded failure log. Some overlay construction still depends on the capture delay; a missing/not-instantiated overlay is not a passed visual check. Diagnostics fixtures use a declared canonical capability set and apply the scenario's deviations. They are not observations of the host GPU.

The Edit visual fixture does not open real media. Use `--auto-edit` with real media for decoder/export behavior. A correct placeholder screenshot does not establish playable video, synchronized audio or successful output.

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
