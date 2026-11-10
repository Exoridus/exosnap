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

Only an explicit screenshot request on Qt's actual offscreen platform permits render-only overlay composition without native capture exclusion. The exclusion authority remains false, and normal native windows still require successful exclusion before becoming visible. Requested overlay windows must contain nontransparent pixels and save successfully. Unknown requested overlay states fail instead of capturing a fallback page. The software preview omits the shader shadows of countdown and toast cards so their actual components remain visible; it cannot verify their native shadows or desktop placement. Region-selection fixtures compose empty and drawn drafts without selecting a physical display or starting capture.

The screenshot runner creates a real per-shot output directory and sets both `EXOSNAP_CONFIG_DIR` and `EXOSNAP_OUTPUT_DIR` before startup. Explicit offscreen visual fixtures validate that same output path in Settings and recording, so an unrelated nonexistent default folder cannot replace a requested toast with a real startup blocker. Named toast readiness checks actual manager event types. The manifest's per-shot `env` field lists scenario and scale overrides; each retained shot log also records the explicit rendering and isolation overrides, without copying inherited environment variables or credentials.

The standalone updater has its own deterministic render path, used by its tests and evidence: `exosnap-updater.exe --preview-state <idle|checking|available|ready|uptodate|cancelled|download|progress|verifying|success|amber|red|green|reboot|reinstall|long>` renders a canned state from the real controller, `--appearance dark|light` sets the shared theme singleton for that render, `--preview-smoke` exits after the first paint, and `--screenshot <path>` grabs the real Quick window and exits. These are argument-configured harness modes; they do not drive update transactions.

The separate `exosnap-setup-preview.exe` target renders the offline Setup without loading Burn or installing anything.
Use `--preview-state <checking|install|manage|uninstall|uninstall-data|progress|protected|success|reboot|failure|removed|cancelled>`,
`--ui-language en|de`, and either `--screenshot <path>` or `--preview-smoke`.
Set `QT_QPA_PLATFORM=offscreen` and `QT_QUICK_BACKEND=software` for deterministic rendering without desktop input.
The preview uses the development Qt plugin directory for the offscreen plugin. The real bootstrapper uses only its private deployed runtime.

`--preview-state lifecycle` starts in detection, then transitions through installation, applying and success after the window is shown.
Run it with `QT_QPA_PLATFORM=windows` to check native resize constraints. It places a nonactivating tool window outside all monitors,
reports geometry and focus state, and exits automatically. `--screenshot <path>` captures the installation page after detection.
This exercises the native Qt window lifecycle without Burn. Static state previews do not verify asynchronous detection or the packaged bootstrapper.

Updater previews also include `unknown`, `repair` and `repair-failed` for the unconfirmed MSI result and repair path.
Both Setup and updater screenshot previews accept `--preview-details` to capture their existing diagnostic popup without desktop input.

| Option | Purpose |
|---|---|
| `--visual-test-size WxH` | Explicit logical window size |
| `--visual-delay-ms N` | Bounded capture delay for construction |
| `--visual-page N` | Index 0-2 Record/Settings/Diagnostics, 3 the internal logs view (legacy alias normalized to Diagnostics + logs), 4 About, 5 Edit |
| `--visual-appearance dark\|light`, `--visual-accent <id>` | Explicit appearance/accent |
| `--visual-shell-appearance dark\|light` | Shell-owned toast appearance, separate from app preference |
| `--visual-expert` | Expert recording-profile controls |
| `--visual-scroll F` | Fraction of current page's scrollable range |
| `--visual-popup source-picker\|source-picker-windows\|source-picker-search\|source-picker-filters\|notification-hub\|gamepad` | Lazy popup |
| `--visual-dialog <name>` | Named close/preset dialog |
| `--record-visual-state <name>` | Named Record lifecycle state |
| `--overlay-visual-state <name>` | Named recovery, error or crash surface, recording HUD variant (`hud-*`) or single desktop toast (`toast-success\|caution\|error\|info`) |
| `--desktop-pattern` | Deterministic source content on the desktop for the live preview. Implies `--visual-live-preview` |
| `--visual-live-preview` | Real capture in the preview instead of the test card |
| `--visual-full-page` | The window as if it were tall enough for the whole scrollable page |
| `--visual-expand-all` | Opens every collapsible element on the visible page before the capture |
| `--visual-collapse-all` | Closes every collapsible element on the visible page before the capture |

With a selected source, the preview shows a synthetic 1920x1080 test card and opens no DXGI or WGC capture, so a screenshot is reproducible and never carries the developer's desktop. A state without a source keeps its empty stage. The live preview is opt-in and then photographs whatever is on the screen.

Use the registered scenario names from `app/visual_tests` rather than inventing a QML object name. Content seeds include `EXOSNAP_VISUAL_EDIT_SCENARIO`, `EXOSNAP_VISUAL_PROFILE_SECTION`, `EXOSNAP_VISUAL_LOG_SCENARIO`, `EXOSNAP_VISUAL_DIAG_SCENARIO`, `EXOSNAP_VISUAL_DIAG_LIVE`, `EXOSNAP_VISUAL_NOTIFICATION_SCENARIO` and `EXOSNAP_VISUAL_SOURCE_SCENARIO`.

Page capture waits for the requested asynchronous loader to be ready, with a bounded failure log. Some overlay construction still depends on the capture delay; a missing/not-instantiated overlay is not a passed visual check. Diagnostics fixtures use a declared canonical capability set and apply the scenario's deviations. They are not observations of the host GPU.

The Edit visual fixtures build synthetic multi-clip workspaces through the editor adapters. Deterministic source frames feed thumbnail generation, synthetic audio feeds waveform analysis, and the software-rendered viewer displays a synthetic still. The viewer does not report a native presented-frame timestamp. These fixtures cover empty and missing media, multiple tracks, clip/junction/spacer/range selections, pending edits, loop state, track controls, panels, monitoring, popups and export states. Source-choice and replacement-review scenes import tiny valid disposable media through the normal source-admission pipeline. Other scenes use synthetic source content. No scene runs an export. Use `--auto-edit --auto-edit-media <path>` with real media for decoder/export behavior. Add `--auto-edit-add-media <path>` to append a second source for the hard-cut and concatenation export checks. A screenshot does not establish playable video, synchronized audio or successful output.

For autonomous editor stability measurements, a harness-enabled build accepts `--auto-edit-qualify --auto-edit-media <path> --auto-edit-report <json>`.
This mode forces the offscreen platform before application creation, rejects other diagnostic/Qt platform switches, isolates configuration and preview caches, suppresses capture preview, input meters, global hotkeys and shell integration, and sets edit playback volume to zero.
It imports real media through the application adapters, repeats five open/play/seek/navigation/close cycles, checks edit/undo round trips, and seeks through 32 linked splits in the final cycle. The first playback interval advances up to 30 seconds, limited to half the source duration; the other intervals advance up to eight seconds. With export enabled, use at least six seconds of media: the first cycle trims, splits, removes a suffix, checks undo/redo and exports the remaining two seconds for independent verification.
Use small, controlled inputs. For foreign media that requires monitoring confirmation, the test chooses only the first audio stream. Playback still opens the configured audio output endpoint with zero-volume samples.

The detached productive player item observes decoded mailbox frames after the adapter's generation filter.
Seek samples measure request to mailbox observation with a 1 ms event-loop poll and include scheduling overhead.
The poll sleeps on a high-resolution waitable timer; the default 15.6 ms system tick would otherwise quantize every sample.
Each cycle also reports its first seek after import with the worker counters accumulated since the closed, idle worker, so session open, queue wait and decoder reads of that cold seek are separated. The snapshot after it drains the worker before the next seek.
Nearest-rank p50/p95/p99 and the raw samples are reported separately from synchronous adapter-commit latency.
The final cycle also compares a seek without cuts, immediately after 32 cuts, after a worker-slot drain and across adjacent segments of one source.
Each comparison includes cumulative simple-player open/close, queue-wait, seek-dispatch and completed random-access read costs.
Counter differences after the cut burst include work performed during the edits, not only work after the final seek request.
Random-access read time includes engine synchronization and decoding; it is not isolated codec throughput.
Short inputs whose adjacent targets overlap the mailbox matching tolerance report the segment comparison as skipped.
Command samples separate model transaction, synchronous workspace publication and the playback adapter's workspace copy within publication.
Publication includes direct signal subscribers and QML work. None of these measurements establishes physical display latency.
`GetProcessMemoryInfo`, `GetProcessTimes` and `GetProcessHandleCount` provide process snapshots. Close samples follow a queued player-worker barrier that includes earlier work and decoder closure; they do not prove that every cache or GPU resource has been released. The report records that drain time separately. Missing measurements remain absent.
The mode does not establish Qt texture handoff, physical presentation, A/V synchronization, audio underruns or GPU/VRAM utilization. Those entries are `SKIP`.
Its report is separate from pointer-gesture tests and independently decoded export tests; success in one layer does not imply success in another.

Synthetic captures from `cargo exo-dev screenshot` use Qt's offscreen platform with the software scene graph, so they do not take desktop focus. Explicit live-preview and desktop-pattern captures retain the native renderer and require desktop coordination. Without a subcommand it takes every named set in the appearance and accent of the developer's own ExoSnap settings. Only those two values are read from there, and every shot still runs in a throwaway configuration. `sweep --set <names>` takes named sets (`list` shows them) in both appearances, and `shot` takes one screenshot from options that mirror the flags and seeds above. All three multiply every shot by `--appearance`, `--accent`, `--size` and `--scale` when those are given. Record states, Edit states and accents are checked against the product sources. The shared state-name headers prevent a retired scenario from silently becoming a plausible default.

By default, output goes to the local screenshot directory reported by the command, in a `<commit date>_<short hash>` subdirectory, with `-dirty` for a tree with uncommitted changes and `_<label>` for `--label`, unless `--out-dir` is given. Each page has a subdirectory holding `<state>_<appearance>-<accent>_<size>.png` and that shot's overlay grabs. A later run into the same directory replaces the shots it retakes and keeps the others, and `manifest.json` (binary path, build time, SHA-256, HEAD, dirty flag, version, exact invocation per shot) and the `index.html` contact sheet always describe the whole directory. Each shot preserves stdout, stderr and the application log before its temporary configuration is removed. If the application log cannot be preserved, the failed shot keeps its configuration and reports the retained path. A shot whose log shows a harness option that found nothing to act on, an unknown state or a QML runtime error is reported as `warn`, not `ok`. A requested desktop overlay that produces no overlay image is also reported as a warning.

The `settings` set captures the combined recording and application settings in the viewport and as a full page. Diagnostics captures collapsed and expanded disclosures, including issue and live-session fixtures. The `profile` set reveals each current recording-profile section in Simple and Expert mode, with existing Configure/Hide groups collapsed and expanded. It also includes full scroll content for the normal routing, expanded source details and legacy microphone arrangements. The `workspace` set captures the preview overlay menu, selected controller widgets with output included or hidden, and the controller's open scene-item context menu and device submenu, including active and paused states. `EXOSNAP_VISUAL_RECORD_PANEL` accepts `overlay-menu`, `gamepad`, `gamepad-hidden`, `gamepad-menu`, `gamepad-device-menu`, `review-audio` and `review-edit`; retired sidebar names are rejected. Hardware-dependent camera controls remain conditional on the declared device fixture. Expert mode reveals details in the recording settings cards. `EXOSNAP_VISUAL_PROFILE_SECTION` reveals these cards on Settings rather than opening a Record sidebar. The full-page capture stitches the window above and below the page's scroller around its complete content, so no card falls between two scroll positions.

The overlay windows are saved as separate files because they sit at their own desktop positions. For the `hud` set, the run also composes each theme and scale's overlay grabs into `overlays/sheet_<theme>_<size>.png`, one row per shot on a checkerboard. It compares windows side by side and does not reproduce their desktop placement. Overlays follow the display scale, not the window size, so vary `--scale` for them.

```powershell
cargo exo-dev screenshot
cargo exo-dev screenshot shot --page record --dialog preset-rename
cargo exo-dev screenshot shot --page record --popup source-picker-filters --sources many-windows --appearance dark,light --size 1120x850
cargo exo-dev screenshot shot --page record --record-state completed --record-panel review-audio --appearance dark,light --size 1600x1000,860x700
cargo exo-dev screenshot shot --page edit --edit edit-default --record-state completed --record-panel review-edit --appearance dark,light --size 1600x1000,860x700
cargo exo-dev screenshot sweep --set pages,record --size 1600x1000,860x700
cargo exo-dev screenshot sweep --set hud --scale 1,1.5,2
cargo exo-dev screenshot sweep --set all --appearance dark --accent aqua --scale 1 --size 2560x1440,1920x1080,1280x720 --out-dir "$env:TEMP\exosnap-review"
```

Prefer a unique directory under system temp, separated by PR and capture run, passed through `--out-dir`. Use a fresh output directory for each capture run so retired scenarios from an older run cannot remain in its manifest. Capture only states that need new evidence; a run does not replace the cumulative PR collection. Until cleanup, retained captures keep their original manifests and source identities. Preserve required provenance in the PR before removing temporary originals. The size axis is the logical application window size, not an observed native maximized desktop or an OS title/taskbar measurement. At scale 1, viewport PNG dimensions match that requested size. Full-page and overlay images have their own dimensions.

The run holds the build tree's lock, so a concurrent build cannot relink the binary mid-sweep. Scrolling and dialogs are applied only after the active page's loader is Ready, so the default four parallel jobs do not lose them to a slow page load.

### Native Controller3D pilot

Build `exosnap_controller3d` with the canonical Qt SDK and its `qtquick3d`, `qtquicktimeline` and
`qtshadertools` modules. The developer executable is not installed in the recording package.
Its post-build deployment includes the Quick3D runtime and QML plugins. Start
`build/windows-x64-ninja-debug/controller3d/exosnap_controller3d.exe` for live SDL3 input and the Normal/Ghost selector.
`EXOSNAP_CONTROLLER3D_DEVICE` optionally pins the same hexadecimal device identity accepted by the existing overlay.
No other process or input provider is opened by the 3D view.

```powershell
cargo exo-dev test --build-target controller3d_tests --build-target exosnap_controller3d --filter '^controller3d\.' --jobs 1
cargo exo-dev screenshot sweep --set controller3d --exe build/windows-x64-ninja-debug/controller3d/exosnap_controller3d.exe --appearance dark,light --size 240x160,320x214,480x320 --jobs 1
```

Pilot fixtures require `--exe`; they are excluded from the main application's `all` screenshot set.
They cover neutral, half/full presses, cardinal/diagonal sticks, clicks, disconnected/unsupported input,
side views, Create/Options close-ups and Ghost on a patterned background. Captures use native D3D11
with WARP by default. Override `QSG_RHI_PREFER_SOFTWARE_RENDERER=0` for the hardware adapter and retain
`QSG_INFO=1` logs to identify the selected adapter. These are scene captures, not desktop captures.
The capture window is fully transparent, non-focusable and input-transparent. The Qt offscreen platform
cannot supply this D3D11 swapchain readback. VSync is disabled in the standalone pilot for timed sampling.

Each PNG has a CSV trace and JSON receipt with capture/input/pose timestamps, pose/frame/ROI hashes,
actual geometry generation, frame count, elapsed time and requested cadence. No dynamic external texture
exists in this pilot, so texture generation is unavailable. Camera, light and pose remain fixed.
Capture time is the requested sample time, input time belongs to the source reading, and pose time is
sampled after applying that reading. All use the existing 100-nanosecond capture clock, not GPU timestamps.
Geometry generation counts successful mesh rebuilds. Each trace row describes one completed frame readback.
`EXOSNAP_CONTROLLER3D_FPS` accepts 30, 60 or 120. `EXOSNAP_CONTROLLER3D_SECONDS` accepts 1 through 30.
Thirty-second runs also require the requested frame census; stable frames at a lower achieved cadence
are not a passing hold test. Read both `stable` and `cadence_met`. The synchronous readback is a test
cost and does not establish recording-compositor throughput. Run timing checks serially.

The normal/ghost depth tests alternate a magenta probe entirely behind the shell and require identical
images. They then hide the shell and require the probe to become visible. This establishes the tested
occlusion path, not rear-hint exclusion, arbitrary translucent layering or final model acceptance.
The shell is a deliberately simple procedural approximation. Hardware input, physical button mechanics,
rear hints, outlines, Xbox geometry, HDR and recording integration require separate qualification.

### Push first, evidence second

Implement and run relevant local checks, then commit and push the change and create or update a Draft PR. Screenshots and clips may be created afterward. Evidence is not a pre-push gate, and CI starts independently under its existing event rules. Keep the PR Draft while evidence is missing or awaiting review.

After pushing, identify only affected states, retain valid evidence, capture missing or changed states and inspect the media and metadata. Update the central **Visual evidence** section using selective uploads below. Uploading evidence requires no source commit. Verify the saved URLs and rendered images or playable clips, then clean up disposable capture files. Prepare the PR for review only after visual checks are complete, required CI is green and existing review/approval requirements are met. This does not authorize merging or auto-merge.

### Incremental PR evidence

The PR description's **Visual evidence** section is the cumulative collection of relevant, currently valid evidence. PNG is the default for visual changes. Use a short GIF only for animation-critical changes when actual motion is essential and stills are insufficient. MP4 or WebM is an exception when GIF is unsuitable. An animated component does not automatically require a clip. For frame-critical defects, prefer targeted stills with a frame index or timestamp when available. Update it selectively throughout review:

| Revision | Evidence update |
|---|---|
| New visible state | Add the relevant screenshots or clips alongside existing valid states. |
| Documented state visibly changes | Recapture only affected views and replace their references in the PR. Remove superseded references, including any in earlier comments. |
| State remains unchanged | Keep its captures and attachment URLs without recapturing or uploading them again. |
| Purely technical revision with no relevant visible change | No new capture is required. |
| Small local UI correction | A targeted crop is sufficient if enough surrounding context remains to identify and judge the change. Identify the crop and retain the original only while review or retry needs it. |
| Animation, transition or moving UI | Keep PNG as the default. Add a short GIF only when actual motion is essential and stills are insufficient, using MP4/WebM only if GIF is unsuitable. Replace only changed clips and retain unchanged ones. |

Preserve **Dark and Light** coverage for changed Qt Quick surfaces. Check regular and narrow logical window sizes when responsive geometry changes, and capture interaction states such as focused, disabled, open or error when affected. Use the closest named fixture and the smallest sufficient `shot` or scoped `sweep`, without an unrelated gallery or full-screen sweep. A clip shows visible motion, not frame-exact timing, native presentation or audio correctness. Follow the desktop-coordination and privacy rules above for any capture outside synthetic offscreen fixtures.

Organize entries by **surface, state, theme and logical window size**, with one current entry for each relevant combination. Each entry identifies its actual capture revision, any dirty-tree status, the fixture or interaction and whether it is a crop, still or clip. Different relevant states remain side by side. A deliberate before/after comparison labels its baseline and source separately from current-state evidence, so an old design is not presented as current. Keep temporary media, manifests and clip capture details untracked until upload and review complete. Before cleanup, record the source build/revision, dirty-tree status, scenario, theme, logical size, rendering backend, any crop/conversion and relevant warnings or capture limitations in the PR entry. Full media and verbose logs need no permanent local archive.

Unchanged evidence need not come from the latest commit. Check whether each revision affects the depicted state, preserve the original source metadata, and never relabel an earlier capture as taken from the latest head. If validity is uncertain, verify the state again or recapture it. A newer technical commit alone does not invalidate an image or clip.

Keep the description current instead of appending comments or image collections for each commit. Remove superseded evidence references from the PR, retaining unrelated discussion and still-valid states. Historical captures may remain locally untracked. Required evidence must be reviewable directly in the PR. If an upload is unavailable or a required state remains unverified, disclose the gap and keep the PR in Draft.

### Attaching screenshots to a pull request

GitHub CLI 2.99 or newer supports image and video uploads through the repeatable `--attach` flag. Check `gh --version` before reporting that command-line uploads are unavailable. Upgrade an outdated installation or use a verified official portable release without changing the system installation.

Before editing an existing PR, read its stored description into an untracked body file. The following commands use the current branch's PR and assume an untracked working directory for `pr-body.md` and media:

```powershell
gh pr view --json body --jq .body | Set-Content -LiteralPath pr-body.md -Encoding utf8NoBOM
```

Edit only the affected entries in its **Visual evidence** section, preserving the rest of the description and every still-valid GitHub attachment URL. For each new or replacement screenshot, use a local Markdown image reference such as `![Source picker, filters open, Dark, 1600x1000](screenshots/picker-dark.png)`. Include the capture revision in the entry. Paths are relative to the body file's directory, with the working directory as a fallback. Remove the old reference for a replaced capture, then attach **only the new or replacement files**:

```powershell
gh pr edit --body-file pr-body.md --attach screenshots/picker-dark.png --attach screenshots/picker-light.png
```

In this example only the source-picker Dark/Light pair is new or replaced. Other entries retain their remote URLs and need no `--attach` flag. The CLI uploads the supplied files and rewrites their local references in place. Every attached file must have its intended reference in the section, otherwise the CLI appends it to the body. For a video, put `![](clips/picker-transition.mp4)` alone in its paragraph beneath the entry's labels and attach that file. GIFs use ordinary image references.

If only labels or other text change, keep the remote URLs and omit attachments entirely:

```powershell
gh pr edit --body-file pr-body.md
```

Read the stored PR body back after each update and save that resolved body for the next edit:

```powershell
gh pr view --json body --jq .body | Set-Content -LiteralPath pr-body.md -Encoding utf8NoBOM
```

Inspect it to confirm that new media uses remote URLs, unchanged URLs remain, superseded references are gone and each relevant surface/state/theme/size combination has one current entry. Open the rendered PR, inspect images and play clips. Do not reuse a body file containing already-uploaded local paths as the next upload list. If an upload fails, read back the stored description, preserve any successful remote references and retry only missing media. Keep the PR in Draft until required evidence is accessible and reviewed. Temporary media and logs follow the cleanup contract below. See [GitHub's attachment documentation](https://docs.github.com/en/github-cli/github-cli/attaching-files-with-github-cli) for supported formats and limits.

### Temporary capture cleanup

The screenshot runner already accepts `--out-dir`; its default workspace output is not an automatic upload or cleanup service. Prefer a newly created, unique system-temp directory named for the PR and capture run. Do not reuse an older directory. Record the exact resolved root and files created by this run so cleanup ownership is explicit. Existing fixtures, manifests and output structure remain usable without a new capture pipeline.

After uploads succeed, read back the PR body and verify remote URLs, rendered images and clip playback. Transfer provenance and limitations needed after cleanup into the relevant PR entries. When no review or retry still needs the files, the contributor or agent automatically removes its own temporary media, PNG frames, contact sheets and capture logs as part of completing the workflow, without waiting for a separate cleanup request. No permanent local originals are required. This is workflow cleanup, not a claim that `screenshot` or `gh` deletes files automatically.

Before deletion, verify that the resolved run root is the exact directory created for this run beneath system temp. Remove only an explicit inventory of this run's files and empty owned directories, without following reparse points or using unbounded recursive deletion. Never sweep the workspace scratch tree, system temp, another run or pre-existing files. Retain files needed for review or re-upload. On failed, partial or unverified uploads, preserve affected files, report what remains and retry only missing media. Do not treat a successful command exit as completed visual review. A successful cleanup leaves only remote media references in the PR, never local paths.

## Recording harness

`--auto-record` runs the real coordinator inside the normal Quick frontend. `--auto-record-bare` requires it and uses a `QCoreApplication` without a QML engine, window or preview. The bare form measures recorder work without the competing GUI, not the complete user's experience.

```powershell
$env:EXOSNAP_OUTPUT_DIR = Join-Path $env:TEMP 'exosnap-measurement'
exosnap.exe --auto-record --auto-record-bare --target monitor --duration 30 --frame-rate 60 --container mkv --video-codec av1 --audio-codec opus --audio-rows sys
```

Use isolated configuration as well when the selected mode does not provide it. Harness outputs are scratch artifacts, not source files or normal user recordings. `--cq <n>` selects a canonical quality value for a measurement, independently of the named ladder.

The auto-record audio rows are migrated into an explicit AudioIntent and passed through the coordinator's ordinary resolver and AudioRuntimeGraph. Shared physical capture and logical DSP fan-out therefore use the product path. No implicit legacy audio initialization is selected by the harness. Fake-source routing tests prove graph behavior, not physical endpoint timing, CPU usage or the number of encoders in a live run.

`--capture-backend default|wgc` is a developer override for a monitor target. Default chooses the product's display duplication path; `wgc` measures WGC on the same monitor. It is refused for other target kinds and is never persisted. It is not a supported end-user backend selector.

Harness-only modes require a non-Release configuration or a Release configured with `EXOSNAP_BUILD_BENCHMARK_HARNESS=ON`. Do not use a shipping release binary as though the harness was enabled. The presence of an option string in the executable is insufficient evidence that its guarded implementation is available. Official release recording checks use [Live Verify](live-verify.md).

The capture log records requested/effective WGC minimum update interval, support and target rate. Hub preview can report an unspecified target rate because it is producer-driven rather than the recorder's cadence.

## Crashes and memory lifetime

Build the ASan preset described in [Build and test](build-and-test.md) for suspected use-after-free or invalid-memory access. Keep the report, allocation/free stacks and exact binary identity together. Release PDBs and crash dumps are separate artifacts; neither belongs in the portable user package.

[Soak and recovery drills](soak-and-recovery-drills.md) cover endurance and interruption. [Encoder-quality measurement](encoder-quality-matrix.md) covers objective file comparisons. Neither a visual fixture nor a short launch smoke substitutes for those tests.

## Focused performance measurements

Set `EXOSNAP_PERFORMANCE_TRACE=1` for aggregate decode readback, normalization, CPU conversion, GPU upload, shader compilation, resource creation, queue wait, decoder/presentation drops, mailbox replacement and audio polling counts. Durations measure host-side calls, including any blocking, rather than fenced GPU execution. GPU stage timings separately cover luminance, tone mapping, composition and conversion. Unmeasured stages remain unavailable.

`EXOSNAP_STAGE_A_CACHE=1` opts newly created visual scene consumers and compositors into experimental scene-plan and sprite-pipeline binding reuse. Unset it for the baseline. `VisualSceneConsumer::PlanRebuildCount()` and `PlanReuseCount()` count completed plan preparations and compatible publication refreshes over the consumer lifetime. `GpuCompositor::PipelineBindCount()` and `PipelineReuseCount()` count overlay pipeline binding groups and skipped groups over the compositor lifetime; they are not GPU timings or allocation counters. These internal accessors are available to tests and probes, not automatically included in recording telemetry. Compare native timing and output separately before enabling the path by default. This switch does not change composition resolution.

The `probe_edit_playback` target accepts `--decode-only` or `--decode-upload` after the media path and start timestamp arguments. The latter uses the real GPU frame converter without Qt presentation. `EXOSNAP_EDIT_SOFTWARE_DECODE=1` permits a software comparison. These runs measure callback throughput, not display cadence or seek-to-visible latency. Use representative codecs, resolutions and bit depths and retain the exact fixtures with the measurement.

The opt-in `WaveformCacheService.RealSourceBoundedStreamingBenchmark` case runs the production waveform service against the file named by `EXOSNAP_WAVEFORM_BENCHMARK_FILE`. Use separate 30-minute, 2-hour and 8-hour sources and the existing Debug tree, with no competing builds. The case reports elapsed analysis time, decoded sample count, decode calls, actual seeks, decoded audio frames, owned analysis buffer bytes and persisted cache bytes. It checks eight service reopen cycles with zero decoder work and bounded block reads. Owned buffer bytes exclude codec internals and process overhead; record process peak working set and private bytes separately. These measurements do not establish visible timeline cadence, A/V synchronization or VRAM use.

From the configured Qt/MSVC developer environment, build the existing `media_cache_service_tests` target and select only this case for a long run. Direct binary runs are diagnostics; the canonical `cargo exo-dev test` receipt still qualifies the complete selected test programs.

```powershell
cmake --build build/windows-x64-ninja-debug --target media_cache_service_tests --parallel 4
$env:EXOSNAP_WAVEFORM_BENCHMARK_FILE = 'D:\recordings\long-source.mkv'
.\build\windows-x64-ninja-debug\app\media_cache_service_tests.exe --gtest_filter=WaveformCacheService.RealSourceBoundedStreamingBenchmark
Remove-Item Env:\EXOSNAP_WAVEFORM_BENCHMARK_FILE
```

`--preview-benchmark <report.json> --benchmark-seconds <seconds>` measures preview publication and consumption plus process CPU. Optional `EXOSNAP_PREVIEW_BENCHMARK_STATE=record|settings|minimized`, `EXOSNAP_PREVIEW_BENCHMARK_RATE=0|15|30|60|120` and `EXOSNAP_PREVIEW_BENCHMARK_ANIMATION=0|1` select the isolated measurement state. Reports include logical persistent surface bytes. Keep the source stimulus fixed and avoid competing builds during measurements.

The overlay presentation experiment uses the real HUD and dock during recording.
Build Release with `EXOSNAP_BUILD_BENCHMARK_HARNESS=ON`, then run
`exo-verify overlay-presentation --product <exosnap.exe> --mpv <mpv.exe> --media <clip> --presentmon <PresentMon.exe> --out <fresh-directory>`.
Coordinate desktop use first: this command shows a borderless D3D11 flip-model workload on Display 1,
places ExoSnap on the secondary display and restores focus to the workload.
It checks foreground ownership, monitor coverage and bottom-edge occlusion during each sample.
The primary matrix requires two displays and ETW rights (administrator or Performance Log Users).
An OFF baseline below 90% independent flip, or more than one target swapchain, stops the matrix without an architectural verdict.
MPV is a synthetic presentation workload, not commercial-game corroboration.

The default matrix retains one warm-up round, then ten samples per variant with alternating forward/reverse order.
Each sample uses fresh processes, configuration and recording output, five seconds of recording warm-up and thirty measured seconds.
`--environment-notes` records operator-known VRR/power facts. `--cadence-only` omits ETW and cannot establish presentation neutrality.
Artifacts retain binary hashes, native overlay visibility/exclusion, per-window `afterRendering`/`frameSwapped` counts,
pipeline snapshots, recording benchmark reports and PID/swapchain/QPC-bound PresentMon v1 statistics.
Qt signals measure render/submission cadence, not displayed overlay frames.
The probe also counts changed visible telemetry publications separately from rendered frames.
Missing external latency/GPU values remain unavailable.

`--continue-from <manifest.json>` imports and revalidates completed sample evidence into a fresh output directory,
then executes only missing round/variant slots. It preserves the interrupted manifest and its error.
Timing, variant order, workload/tool hashes and effective recording configuration must match.
A reviewed equivalent product rebuild requires the exact old hash in `--previous-product-sha256`; each sample retains its product identity.
Continuation is explicit and does not automatically retry or classify a failed attempt. Inspect the retained failure before continuing.

Only the auto-record benchmark reads `EXOSNAP_OVERLAY_PRESENTATION_VARIANT`:
`off`, `hud-minimal`, `hud-health`, `hud-full`, `dock-only` or `hud-full-dock`.
It requires isolated configuration and one unpaused benchmark cycle, disables toasts,
and selects the real Minimal, Health or Technical content presets. The full-dock variant combines Technical with quick controls.
The default matrix uses these six variants; `--variants` selects a focused subset containing its own OFF baseline.
The probe observes the production static indicator and change-driven telemetry without forcing updates or freezing values.
Retired animation and synthetic paint/text cadence variants are rejected rather than mapped to a different workload.
Ordinary launches do not read the variant override, and the harness uses isolated settings.

Development harness builds accept `EXOSNAP_QML_PROFILE=1` to expose only QML profiler services. `--navigation-lifecycle-test` records first and warm page-ready times and can run with `QT_QPA_PLATFORM=offscreen`. Profiled construction costs include instrumentation overhead. Real click-to-visible latency and idle preview resource measurements require the actual desktop. Compare the application closed, Record visible, another page, minimized, preview Off/limited and recording without inferring game-FPS impact from preview activity alone.

### Experimental SDR composition demand

`EXOSNAP_DEMAND_COMPOSITION=1` enables the output-sized composition experiment for same-aspect, uncropped BGRA8 SDR recording without cursor capture. HDR, tone mapping, 10-bit, 4:4:4, incompatible aspect and unknown Preview demand retain source-size composition. The default is disabled. Compare identical workloads with the variable absent and set, independently of the Stage A cache gate. Record the `Experimental composition demand resolved` dimensions and reason, compositor CPU/GPU diagnostics, output decode checks and texture reservations. Initialization failure retains the source-size compositor.

Preview publishes its physical viewport demand, including device pixel ratio and normalized source crop. A hidden Preview requests no pixels. A visible Preview that fits the recording composition can share it. A larger or unknown demand retains or promotes to the source-size path. Promotion is one-way during a recording; shrinking is deferred until the next recording to prevent resize oscillation. Preview editing geometry remains in original source pixels even when the shared texture is smaller. Replacement shared textures are populated before announcement. This experiment does not qualify HDR or arbitrary multi-destination rendering, and performance or output parity must be established on the measured workload before enabling it by default.
