# Qt Quick frontend migration — remaining-surface audit

> **Historical.** This audit was written while the Qt Widgets frontend still shipped, and it is
> kept for the reasoning behind each surface's classification. The migration is done: Qt Quick is
> the production frontend and `exosnap.exe` is built from it (ADR 0064). Present-tense statements
> below about `exosnap_quick_spike`, `EXOSNAP_BUILD_QUICK_SPIKE` or "the shipping Widgets
> frontend" describe the migration-era tree, not the current one.

Snapshot of every user-facing surface in the shipping Widgets frontend, classified for the
Qt Widgets → Qt Quick/QML migration, plus the non-surface work the cutover depends on.

Classification:

| Status | Meaning |
|---|---|
| `ALREADY QUICK` | Migrated and validated; Widgets version retained only as parity reference |
| `TO MIGRATE` | User-facing UI that must become QML |
| `SHARED NON-UI` | App-layer logic; no framework dependency either way |
| `NATIVE — KEEP` | Platform integration that must stay native C++ |
| `OBSOLETE AFTER CUTOVER` | Removed once the Quick frontend ships |
| `SEPARATE EXECUTABLE` | Its own migration unit, not coupled to the main cutover |

---

## Navigation areas

| Surface | Source | Status | Notes |
|---|---|---|---|
| About | `pages/AboutPage.*` 17 KB | `ALREADY QUICK` | ADR 0057. Shared `models/AboutInfo` feeds both frontends. |
| Record | `pages/RecordPage.*` 286 KB | `ALREADY QUICK` | ADR 0058. Cutover-ready incl. D3D11 preview, ready-frame capture, region/webcam editing. |
| Settings | `pages/ConfigPage.*` 357 KB | `ALREADY QUICK` | ADR 0059. Twelve sections incl. webcam, hotkeys (real Win32 registration), audio meters, all four themes. `scrollToSection` deep-links have no Quick caller yet. |
| Device | `pages/DevicePage.*` 40 KB | `ALREADY QUICK` | `DeviceAdapter` + two `QAbstractListModel`s + 5 QML files. Full parity incl. active-encoder badge rules, LUID re-match, lazy async scan. |
| Diagnostics | `pages/DiagnosticsPage.*` 100 KB | `ALREADY QUICK` | ADR 0060. Policy extracted to `DiagnosticsController`; blocking probes moved off the GUI thread. |
| Logs | `pages/LogsPage.*` 44 KB | `ALREADY QUICK` | ADR 0060. `LogEntryModel` + filter proxy over the existing `AppLog` deque; four history copies collapsed to one. |

## Editor (overlay over Record, ADR 0022 — not a nav item)

| Surface | Source | Status | Notes |
|---|---|---|---|
| Edit/Export page | `pages/EditExportPage.*` 64 KB | `ALREADY QUICK` | ADR 0061. Four adapters own what the page owned. |
| Timeline | `ui/widgets/EditTimeline.*` 30 KB | `ALREADY QUICK` | ADR 0061. Declarative QML; markers thinned in C++. |
| Player surface | `ui/widgets/EditPlayerSurface.*` 13 KB + `services/EditPlayerRenderer.*` 34 KB | `OBSOLETE AFTER CUTOVER` | Replaced by `ExoEditPlayerItem`. Zero child HWNDs verified by `--hwnd-audit`. |
| Details rail | `ui/widgets/EditDetailsRail.*` 9 KB | `ALREADY QUICK` | Never hidden; carries the export controls. |
| Export panel | `ui/widgets/ExportPanel.*` 22 KB | `ALREADY QUICK` | Options/running/done/failed, plus a `Cancelling` state that did not exist before. |
| Export overlay | `ui/dialogs/EditExportOverlay.*` 8 KB | `ALREADY QUICK` | `EditOverlay.qml` in the shell. |

**Handoff wired (2026-08-10).** `QuickApplication::openEditorForCurrentRecording()` builds the context
through the shared `models/EditContextFactory` and hands it to `EditSessionAdapter`, driven both by the
result row's Edit button and by `open_editor_when_finished`. Gates live in one place: no editor over a
live capture, no clobbering a running export, no split recording. The D3D11 player path has **still not
been exercised against a real decoded clip** — that remains the open editor blocker.

Lifting the factory out of `RecordPage.cpp` exposed a latent defect: `EditContext` was **defined twice**
(`models/EditContext.h` and `pages/EditExportPage.h`), field-identical. It survived only because no
translation unit ever saw both. The copy is gone.

## Shell and chrome

| Surface | Source | Status | Notes |
|---|---|---|---|
| `MainWindow` | 249 KB | split by responsibility | Both the Widgets shell *and* the app orchestrator. See the responsibility split below. |
| Operational title bar | `ui/chrome/OperationalTitleBar.*` 23 KB | `TO MIGRATE` | 40 px, `ExoSnapMetrics.h` `kTitlebarHeight`. |
| Notification hub panel | `ui/chrome/NotificationHubPanel.*` 18 KB | `ALREADY QUICK` | `NotificationHub.qml` + `NotificationBell.qml` over `NotificationsAdapter`. Severity ranking extracted as `SHARED NON-UI`. |
| Global recording bar | `ui/chrome/GlobalRecordingBar.*` 4 KB | `OBSOLETE` | Dead code: the only references app-wide are its own test. Never reachable from `MainWindow`. |
| `WindowGeometryPolicy` | `ui/WindowGeometryPolicy.*` | `SHARED NON-UI` | Pure, toolkit-free. Port unchanged, including its call-site subtleties. |
| `RecordingStatusGuards.h` | | `SHARED NON-UI` | Three pure predicates. |

## Dialogs and overlays

Every `*Overlay` is a child `QWidget` of `centralWidget()`, not a `QDialog` — a deliberate recipe so
the backdrop composites over the native preview HWND. Modality is *simulated* (focus + Escape), never
an input grab. That constraint disappears with the Quick frontend: the preview is a scene-graph item,
so ordinary QML dialogs/popups composite correctly.

| Surface | Status | Verdict |
|---|---|---|
| Source picker (dialog/overlay/panel) 70 KB | `ALREADY QUICK` (UI) | `RecordSourcePicker.qml`. `SourcePickerWindowRules.h` reclassifies as `SHARED NON-UI`; the `SourceOption`/`Section` types must be lifted out of `SourcePickerDialog.h` before it is deleted. |
| Recovery overlay 31 KB | `TO MIGRATE` | QML dialog in shell |
| Crash report overlay + panel 37 KB | `TO MIGRATE` | QML dialog in shell |
| Recording error overlay + panel 19 KB | `TO MIGRATE` | QML dialog in shell |
| What's new overlay 10 KB | `TO MIGRATE` | QML dialog in shell |
| Finalizing overlay 6 KB | `TO MIGRATE` | QML dialog in shell; non-dismissable |
| `QFileDialog` uses | `ALREADY QUICK` | QuickDialogs2 (`SettingsOutputFolderDialog.qml`, `SettingsPresetFileDialog.qml`) |
| `QMessageBox` uses (24 in `MainWindow.cpp` alone) | `TO MIGRATE` | No Quick equivalent wired yet. Needs one shared confirm/alert primitive. |

## Capture-excluded overlay windows

Separate top-level windows with `WDA_EXCLUDEFROMCAPTURE`, not in-window UI. All become QML `Window`s
with the native affinity call retained.

| Surface | Status | Click-through | Placement follows |
|---|---|---|---|
| Notification toast 54 KB | `TO MIGRATE` | chrome yes, action pills no | the **app's** screen |
| Diagnostics overlay window 20 KB | `TO MIGRATE` | yes | the **recorded** monitor |
| Quick control pill 19 KB | `TO MIGRATE` | **no — interactive by design** | the **primary** screen — see below |
| Countdown overlay 12 KB | `TO MIGRATE` | yes | the **recorded** monitor |
| Recording overlay 10 KB | `TO MIGRATE` | yes | the **recorded** monitor |
| `SetWindowDisplayAffinity` + fail-closed guard | `NATIVE — KEEP` | — | — |

**Pre-existing divergence — record, do not silently fix.** The quick control pill positions itself
on `QGuiApplication::primaryScreen()` (`QuickControlPillWindow.cpp:448-466`) and has no
`setMonitorGeometry` at all, unlike the other three recorded-monitor overlays. Recording a secondary
monitor therefore puts the pill on the wrong screen. Moving it to the recorded monitor is a visible
behaviour change for every multi-monitor user and belongs in the spec, not in a migration change.

**Fail-closed ordering — only one of the five gets it right.** `QuickControlPillWindow::updateVisibility`
(419-444) forces the HWND and applies the affinity *before* `show()`. The other four rely on
`showEvent` plus a `hide()` afterwards. Only the pill's ordering is portable to QML, where a `Window`
has no HWND until `create()`.

The fail-closed guard is a **correctness** property, not defensive polish: if capture exclusion fails,
the window hides and stays hidden for the session, because an overlay that would burn into the
recording must never show. It must survive the port, and the affinity result should be exposed to QML
so the hide is a binding rather than imperative code.

## Custom widgets (~50 classes)

Everything under `ui/widgets/` is `OBSOLETE AFTER CUTOVER` once its consumers are migrated, with
these exceptions:

- `ui/widgets/PreviewSurface.*` (90 KB) — superseded for Record by `ExoPreviewItem`; retained only
  while the Widgets Record page is the parity reference.
- `ui/widgets/RegionGeometry.*`, `ui/dialogs/SourcePickerWindowRules.h` — pure geometry/rule helpers;
  reclassify as `SHARED NON-UI` and move out of `ui/` at cutover.

## Theme

`ui/theme/ExoSnapTheme.cpp` (30 KB) plus the QSS token pipeline is `OBSOLETE AFTER CUTOVER`.
The theme **table** (`ui/theme/ExoSnapThemes.h`) is `SHARED NON-UI` and stays: `QuickThemeTokens`
resolves all four shipped themes from it, so the Quick frontend needs no QSS to be themed.
`app/tests/test_theme_token_resolution` becomes moot at cutover — it guards QSS token validity, and
there will be no QSS.

## Native platform integration — keep as C++

Tray presence, global hotkeys, elevated relaunch, display/audio/webcam notifiers, capture hubs,
DXGI/WGC producers, D3D11 renderers, webcam service, thumbnail capture, update service, crash
capture, ETW/PresentMon, ZIP writing, atomic file ops, all stores. None of these become QML.

## Secondary executables

`apps/updater` links `Qt6::Widgets` and is a separate executable with its own lifecycle.
`SEPARATE EXECUTABLE` — a distinct migration unit that must not be coupled to the main cutover.

---

# Subsystem findings

These three sections replace guesswork with measured facts. They are what the implementation phases
are planned against.

## Diagnostics / Logs / StartupTrace / Support bundle

**The port is dominated by policy extraction, not by QML.** `DiagnosticsPage.cpp` carries genuine
product policy inside the widget: verdict tier counting, the `kMaxIssues = 6` cap, hotkey-notice
synthesis, seven readiness-tile value/sub/tone rules, and — critically — **stateful frame-drop delta
accounting across snapshots** that resets on `session_generation` change. Losing that state silently
corrupts capture-drop numbers. `MainWindow` additionally owns the entire support-bundle assembly and
the FixAction dispatch chain.

**Logs need no new logging subsystem.** `AppLog` already holds a mutex-guarded
`std::deque<LogEntry>` (cap 5000, FIFO) and emits `entriesAppended(QVector<LogEntry>, int evicted)`
queued onto the GUI thread. A `QAbstractListModel` layers directly over it: the signal's eviction
count makes rollover an exact `beginRemoveRows`, and filtering becomes a `QSortFilterProxyModel`
reusing the extracted predicate. This collapses today's **four** copies of the history
(deque → `entries_` → `visible_entries_` → `QTextDocument`) to one.

**Blocking I/O on the GUI thread is pervasive and must not be ported as-is:** a Win32 volume query on
every diagnostic data push, `CheckOutputPathWritable` (a real file create/write/delete) twice per
refresh and at 2 Hz *during recording*, `SelfTestRunner::Run()` (DXGI factory + `LoadLibraryW` + temp
file + COM audio enumeration), and the entire support bundle (up to 6 log files, two global regex
scrubs, adapter enumeration, miniz deflate). All of it needs a worker plus a `checking` property.

### Pre-existing divergences — record, do not silently "fix"

- **Blocker gating.** `docs/product-spec.md` states a Blocker prevents recording from starting. No
  `RecommendationEngine` result reaches any start gate; the only real gate is capability validation
  in `RecordingCoordinator`. The Diagnostics tier-1 set is presently advisory only. The new boundary
  must neither implement gating nor imply it.
- **`startup-trace.txt`.** Promised in the product spec, ADR 0044 and `StartupTrace.h`, but
  `CollectBundleEntries` never adds it and `FormatStartupTrace` has no production caller.
- **Self-test "not run" detection** works by substring-matching `"not executed in this build"`. The
  boundary must carry an explicit typed field instead.
- **2 Hz file I/O during recording** — fixing it during the move is right, but it is a behaviour
  change and is called out as one.

## Editor

**The Record preview bridge cannot be reused.** FFmpeg's D3D11VA decode creates its own internal
D3D11 device that the application never sees, and the engine immediately reads frames back:
`RawDecodedVideoFrame` carries **CPU planes only** (YUV 4:2:0/4:4:4, 8/10-bit, PQ flag, matrix,
range). There is no shared texture to open, so `ExoPreviewItem`'s NT-handle + keyed-mutex transport
has nothing to attach to. Three ownership differences, not one:

1. No shared texture exists — the producer hands over host memory.
2. Preview *pulls* at scene cadence and a miss is a non-event; the editor is *push*-driven and
   deliberately drops late frames against a media clock. That gate is playback policy and must
   survive into the item.
3. Conversion differs: preview needs BGRA8/RGB10A2→RGBA; the editor needs planar YUV with matrix,
   range and HDR10 PQ tone-mapping — i.e. `exosnap::engine::EditFrameGpuConverter`.

**Build `ExoEditPlayerItem` as a sibling that reuses the skeleton, not the transport:**
`updatePaintNode` with `QSGImageNode` under `QSGClipNode`, generation counters,
`beginExternalCommands()/endExternalCommands()`, scene-graph invalidation reconnection — all copied
from `ExoPreviewItem`; payload replaced by a mailbox of `RawDecodedVideoFrame`, whose
`backing_frame` shared_ptr already *is* the correct cross-thread ownership token.
`EditFrameGpuConverter::Init` takes a **borrowed** device, so it drops straight onto Qt's D3D11
device. Consequence: `EditPlayerSurface` and `EditPlayerRenderer` (~800 lines with their own render
thread, swap chain and child HWND) are **deleted, not ported** — which is what removes the editor's
child HWND.

**The timeline is far cheaper than its widget suggests.** Measured data scale: ~8–20 thumbnail tiles
(width-dependent, *not* duration-dependent), 0–3 label-only audio rows, typically <20 markers
(hard cap 10 000), and **zero waveform points — waveforms are forbidden by the product spec**. That
is plain declarative QML: a `Repeater` of `Image` delegates for tiles, a `Repeater` of `Rectangle`s
for markers. A custom `QQuickItem` scene-graph node would be pure cost. Two guards: never instantiate
Qt Quick **Controls** for the trim handles (`MouseArea` + `Rectangle` only), and filter markers in
C++ so the 10 000 cap can never become 10 000 `Rectangle`s. There is no multitrack model, no zoom, no
scroll model and no keyboard interaction in today's editor.

Thumbnails have **no cache at all** — every run re-decodes. Expose tiles through a
`QAbstractListModel` + `QQuickImageProvider` keyed by `runId/index`; never put a `QImage` in a role.
The "Generating previews…" state is currently derived *inside* `paintEvent`, which QML cannot do, so
it must become explicit `tilesExpected`/`tilesReady` properties.

### Two semantics to fix in the contract before any fan-out

- **Trim exists twice**: authoritative in µs and snapped on the page, transient in ms and unsnapped
  in the timeline widget, with the page overwriting the widget after release. In the contract, trim
  lives **once**, in µs, snapped, on `EditSessionAdapter`; QML gets ms accessors.
- **`exportRunning` exists twice and the copies diverge**: cancel declares the run over while the
  remux thread is still live, and the `join()` is deferred to the next export — so a Retry
  immediately after a Cancel blocks the GUI thread. `exportRunning` lives once, on
  `EditExportAdapter`, with the panel state derived from it.

Also blocking on the GUI thread today and not to be ported: `ExtractKeyframeTimestamps` (a full
container index read), `EditPlayerSession::Open`, and the export `join()`. Export progress fires
per video packet and currently posts one queued event each — throttle to whole-percent changes.

Minimum size: the only hard constraint is 860×700. The overlay's 20 px band per side means the page
sees 820 px, which lands on the narrow rail breakpoint. The rail is **never** hidden — it carries the
export controls — and compacts instead, without dropping any of its seven facts.

## Shell, chrome and window integration

`MainWindow` splits by responsibility as follows. The orchestration half is what `QuickApplication`
already replaces area by area; the presentation half becomes QML; the platform half stays C++.

| Responsibility | Content |
|---|---|
| Application orchestration | The god-constructor (settings, crash session, preset store, update service, ~60 signal wirings, tray, overlays, async caps probe), preset operations, startup overlay ordering, notification manager + action router, update check, elevation handoff, support bundle, diagnostics fan-out, close guards, and `onRecordChromeStateChanged` — a single slot with 15 downstream effects and the biggest orchestration hotspot in the file |
| Navigation | `navigateToPage` (with an overlay **veto** that can cancel navigation), lazy page build, the 6-entry `kPageDescriptors` table |
| Platform/window integration | `nativeEvent` (WM_HOTKEY, WM_NCACTIVATE ×2, WM_SIZE, WM_GETMINMAXINFO, WM_NCCALCSIZE, WM_SETCURSOR, WM_EXITSIZEMOVE, WM_DISPLAYCHANGE, updater handoff), resize-zone hit testing, DWM themed border, geometry save/restore, `WM_SETICON`, fullscreen |
| Presentation | Title-bar status, notification hub toggle, unread bell, the four overlay-window updaters |
| Legacy-only widget plumbing | `hydrateSecondaryPages` and the five `build*Page` functions, `paintEvent`, all `*_placeholder_` members, the nine `applyVisual*` scenarios |

**`hydrateSecondaryPages` is an artifact worth noting:** staged post-first-paint page construction
exists because `ConfigPage` took ~1.6 s under global QSS. QML has no equivalent cost, so the whole
mechanism disappears rather than being ported.

### Entry-point parity — closed 2026-08-10

`app/bootstrap/ProductionBootstrap.{h,cpp}` is now compiled into and executed by the Quick entry
point, which closes cutover blocker 4 below: DLL-search hardening, the interrupted-swap self-heal,
`StartupClock`/`StartupTrace`, `AppLog::init` + the engine log bridge, the single-instance mutex
(suppressed for harness runs), crash-capture `Initialize`/`MarkCleanExit`/`Shutdown`, the Sentry
verify hook, application metadata, the branded icon incl. the native `WM_SETICON` slots, and the
elevated-relaunch execution path. `exosnap.rc` + the generated VERSIONINFO are compiled into the
Quick target, so it carries icons and version metadata.

The hardening had one measurable consequence worth recording: `SetDefaultDllDirectories` removes
`PATH` from the loader search order **by design**, and the Quick target had no `windeployqt` step —
it had always relied on Qt being on `PATH`. The first hardened run therefore failed at
`QQmlApplicationEngine::load` with an unresolvable `qtquickcontrols2plugin` dependency rather than
at startup. Staging the Qt runtime + QML imports next to the executable (`--qmldir` is required;
the QML lives in generated resources, so scanning the binary alone finds no imports) is now part of
the target, which is what a deployed install looks like anyway.

Still open on the bootstrap: `crash_capture::BeginSession` is wired (with the previous-session read
ordered before it), but there is **no next-launch crash overlay to consume `pending_crash_`**, the
persisted crash-report consent policy never reaches
`crash_capture::GiveUserConsent/Revoke/ResetUserConsent`, and `ParseRelaunchArgs` /
`HasVerifyUpdateReinstallRequest` have no Quick caller — so `ProductionBootstrap`'s relaunch
mechanism exists with zero callers.

### What `QuickApplication` is still missing

Verified by grep — zero hits for each at the time of writing. Items struck through below were closed
on 2026-08-10; the rest still stand. This is the real remaining shell work, and it is larger than
the page migrations:

1. ~~Notifications entirely~~ — **done**: `NotificationsAdapter` owns the existing
   `NotificationManager`, the bell + hub are QML, and the action router lives in `QuickApplication`.
   The separate capture-excluded toast *window* is still unwired.
2. Tray — no `TrayPresence`, close-to-tray, unread badge. `keepRunningInTray` toggle likewise has no
   implementation behind it.
3. Crash capture — session and previous-crash context are wired; the **consent policy now reaches
   `GiveUserConsent`/`Revoke`/`ResetUserConsent`** at startup and on every Settings edit (it never did
   before, so an `AlwaysSend` user uploaded nothing and a `NeverSend` user kept whatever consent an
   earlier session had left behind). Still missing: the next-launch crash overlay that consumes
   `pending_crash_`.
4. ~~Update system~~ — **done**: `UpdateService` is owned by `QuickApplication`, the Settings card is
   driven through `ResolveUpdateCardState` (including the Scoop, pending, updater-running and
   verify-reinstall states the adapter previously rendered as a blank "Up to date"), the check is
   guarded against running mid-recording, an available update raises a hub notification, and
   `--verify-update-reinstall` is armed from argv. What's New is still unwired.
5. Recovery — the manifest store is held, but there is no service, scan or overlay.
6. ~~Window geometry save/restore~~ — **done**: `QuickWindowGeometry` resolves the persisted rect
   against the connected screens through the existing pure `ui/WindowGeometryPolicy` clamps and hands
   it to `Main.qml` as an initial property, so the window is created at its final placement rather
   than jumping there. Persisting tracks the last *windowed* rect explicitly, because `QWindow` has no
   `normalGeometry()` — without that, closing while maximized would have written the maximized rect as
   the restore rect. Minimized geometry is never sampled, writes are debounced, and the flush runs
   both on close-approval and in the destructor (a plain close never reaches `closeApproved`).
7. Elevation — the argv side is wired: `ParseRelaunchArgs` and `HasVerifyUpdateReinstallRequest` now
   have Quick callers, so a relaunch lands on the handed-off page and re-persists the
   present-diagnostics opt-in. Still missing: anything that *requests* an elevated relaunch, so
   `ProductionBootstrap::requestElevatedRelaunch` remains uncalled.
8. Diagnostics providers — no PresentMon, DPC latency, or window-evidence probe.
9. `DisplayDeviceNotifier` — HDR/topology re-probe on `WM_DISPLAYCHANGE` is gone.
10. ~~Close guards~~ — **done**: `models/CloseGuardPolicy` holds the ordering and wording,
    `ShellAdapter` routes it asynchronously (QML has no `Dialog::exec()`), `Main.qml` drives it from
    `onClosing`. Includes the STOPPING block and the debounced-persist flush.
11. ~~The capability probe is **synchronous on the UI thread**~~ — **done**: moved to an owned
    `QThreadPool` whose destructor joins it.
12. ~~Support bundle, window/taskbar icon, `AppLog::init`/`StartupClock`/`StartupTrace`,
    single-instance mutex, `SetDefaultDllDirectories`, updater self-heal~~ — **done** (support
    bundle via `DiagnosticsAdapter`; the rest via `ProductionBootstrap`). Fullscreen still absent.
13. No equivalent of `onRecordChromeStateChanged`'s 15-way fan-out. `synchronizeRecordState()`
    covers format text, device state, webcam, countdown, split and — since 2026-08-10 — the
    Settings control lock. Still missing: the recording/paused window icon swap, tray state, the
    title-bar pill, auto-navigation to Record on start, Edit-overlay dismissal on start, and the
    finalizing overlay.

## Correctness defects found on the Quick path (2026-08-10)

Each was verified against the code, not inferred, and each is fixed.

- **Ready-frame worker outlived `RecordingCoordinator`.** `RecordPreviewAdapter`'s pool is declared
  last *within its own class*, which protects that class's members but not the composition root: the
  worker runs a callback `RecordingCoordinator::CaptureFrame` built around a pointer to the
  coordinator's `snapshot_pool_`, and the coordinator is destroyed first. Clearing the requester only
  refuses new requests. `~QuickApplication` now joins explicitly via `waitForPendingReadyFrames()`
  before it tears anything down. Reachable by pressing Capture-Frame and closing the window.
- **`QuickHotkeyEventFilter` was never removed** from `QCoreApplication`. `QGuiApplication` in `main()`
  outlives `QuickApplication`, so any Win32 message dispatched during Qt's teardown — or by the
  `ShellExecuteEx` in the pending elevated relaunch, which pumps — reached a freed filter. This is the
  repo's known `nativeEvent` teardown shape; `QuickWindowChrome::detach()` had it right all along.
- **`WebcamService::frame_callback_` was reassigned under a live capture thread.** The callback is
  read lock-free at ~30 fps and `control_mutex_` does not cover it, so clearing it before stopping the
  camera destroyed the closure out from under a reader. The two teardown lines are now in the other
  order (stop, which joins, then clear).
- **The capture sink chose its `invokeMethod` receiver by dereferencing a cross-thread `QPointer`.**
  `Unsubscribe()` is explicitly documented as *not* a barrier, so the item could be freed between the
  null test and the call. The receiver is now unconditionally the application object; the in-lambda
  guard already gave the intended drop semantics, on the thread where the answer cannot change.
- **Both scene-graph items emitted `QSGGeometry::DrawTriangleFan` for their rounded-rect clip.** The
  Qt Quick batch renderer has no case for a fan, so it logged `Primitive topology 0x6 not supported`
  and silently built the pipeline as a triangle *list* — 38 fan vertices consumed as 12 unrelated
  triangles, degrading the stencil mask into slivers with no scissor fallback. Both now share
  `RoundedRectClipGeometry.h`, which emits an explicit triangle list. Verified: the warning is gone
  from a `QSG_INFO=1` run of both the Record and Edit surfaces.
- **`AboutPage.qml`'s `implicitHeight` binding loop** (measured, not inferred). The wrapper's height
  read `scrollView.availableHeight`; that value becomes the ScrollView's `contentHeight` and therefore
  its `implicitHeight`, so the page's implicit size depended on its assigned size — a recursive
  rearrange inside the shell's `StackLayout`. Fixed by reading the page root's height (handed down by
  the layout, uninfluenceable from inside) minus the now-constant gutters, plus an opt-in
  `reserveScrollBarGutters` on `ExoScrollView` for any surface whose content width feeds back into its
  own height. Verified clean at 1600×1000 and 860×700.
- **`RemuxToProgressiveMp4` could not be cancelled on a durationless input.** The cancel flag was
  tested only inside the progress block, behind three further conditions — a positive container
  duration, stream index 0, and a valid PTS. `matroska_stream_writer` writes `KaxDuration` as 0.0 and
  back-patches it at finalize, so exactly the files a cancel matters for (an unfinalized master from a
  crash or an abrupt stop) were the ones that could not be cancelled — while `~EditExportAdapter`
  joins that work on the GUI thread during window close. The probe is now once per packet,
  unconditional; progress additionally moved off "stream 0 is typically video" onto the real video
  stream index.
- **No WinHTTP session in the repo set timeouts.** The default resolve timeout is INFINITE, and the
  receive timeout is per read operation, so a stalled or drip-feeding server blocked `CheckForUpdate`
  indefinitely — on a worker that `~UpdateService` joins from the GUI thread at shutdown, i.e. an
  application that will not close, with no cancel affordance. Both sessions now set explicit timeouts
  and the update check additionally carries a wall-clock budget for the body read, which is what
  bounds the drip case.
- **The WGC capture thread entered an STA and never pumped messages.** `WgcSourceProducer`'s contract
  says frames *and* the `GraphicsCaptureItem.Closed` callback arrive through that pump; every other
  WGC caller in the tree drains it. Consequence: a destroyed capture window was never reported as
  lost and the preview held a stale frame forever. The loop now drains the queue, and the early-return
  path no longer strands the apartment.

## Duplicate sources of truth found after the area migrations (2026-08-10)

Two of these lose user data outright, and neither is visible to any existing test — both need one
frontend to write a field and the *other* surface to be touched afterwards.

- **`PersistedAppSettings` had two mutable owners.** `QuickApplication::settings_` and
  `SettingsAdapter::app_settings_` are both whole-struct copies. The hotkey paths mutated `settings_`
  and saved without republishing to the adapter, so the next Settings toggle ran
  `settings_ = settings_adapter_.appSettings()` and wrote the adapter's stale bindings back over the
  rebind — **and persisted the revert**. Window geometry had the identical shape. All writes now go
  through one `saveAndPublishAppSettings()`.
- **`RecordingPresetConfig` had two mutable owners, and clobbered in both directions.** The adapter
  mirrors the whole struct but the Settings surface exposes only part of it, so
  `applySettingsConfigEdit` taking `settings_adapter_.config()` wholesale reset the capture target,
  the region, the countdown and the webcam overlay rect the moment the user changed a codec. The
  reverse also held: the audio source rows are editable from *both* surfaces, and a Settings-side
  audio edit never reached `record_view_model_.audio_ui_state`, so the Record page kept — and
  recorded with — the old plan. Record-owned fields are now restored from the owner on read-back, and
  the shared fields are mirrored in both directions through `syncConfigMirrors()`.
- **Codec labels were re-implemented locally** in `QuickApplication.cpp` despite `ui/CodecLabels.h`
  being the documented single source of truth, and had already drifted: the local audio fallback was
  `Opus` where the canon says `AAC`, and the video labels were hardcoded rather than the shared
  `capability::VisibleVideoCodecLabel` spelling.
- **`UpdateChannelFromString`/`ToString`** and **`DeveloperLogLevelFromString`** were private copies in
  `MainWindow.cpp` that the Quick frontend needed verbatim; both moved next to what they convert
  (`services/UpdateService.h`, `diagnostics/AppLog.h`).
- **Startup hotkey-conflict policy** existed only in `MainWindow`. Quick discarded `SetRegistrar`'s
  return value entirely, so a shortcut Windows refused stayed listed in Settings and silently did
  nothing. The rule (drop the dead binding; log a default collision, notify about a custom one) is now
  `models/HotkeyStartupConflicts`.

Two defects found while auditing this list, both fixed on 2026-08-10 and both invisible to every
existing test:

- **`SetDiagnosticsCallback` is single-occupancy** and was registered twice — once for the Record
  view model, once for the Diagnostics adapter. Whichever initializer ran last won, so
  `dropped_frames`, `av_drift_*` and the Edit report badge stayed at zero for the entire session.
  Every consumer now fans out from one registration.
- **The shell landed on About.** `AppShell.qml` derived its start page from a migration-era
  `startOnRecord` flag that production passed as `false`, so the shipping application opened on its
  own version numbers. Record is the landing destination; the flag is gone rather than inverted.

Subsystems whose **UI exists and whose backend exists, but which are connected to nothing** — the
worst category, because they look finished in both a file listing and a screenshot:

- ~~The Updates card renders state nobody sets~~ — **closed** (see item 4 above).
- `DiagnosticsAdapter` has setters for PresentMon, DPC latency and window evidence with no callers;
  `DiagnosticsPage.qml` renders cards that can never receive data. Worse than inert: the readiness
  copy still claims *"Elevated — PresentMon ETW present diagnostics available"*, which an elevated
  Quick user is told while nothing samples it. That line has to become conditional on a provider
  actually being installed before cutover — it is an overclaim, and the diagnostics rule is that only
  measured facts are stated.
- All five capture-excluded overlay windows exist as QML with their `CaptureExclusion` block, and
  none is instantiated. `QuickWindowChrome` likewise compiles and is never constructed.
- `keepRunningInTray`, `show_notifications`, `show_recording_overlay`, `show_diagnostics_overlay`,
  `show_quick_controls` and `present_diagnostics_optin` are persisted toggles with no implementation
  behind them, i.e. settings that lie. Two more from that list have since been closed:
  `developer_log_level` now reaches `AppLog::setMinSeverity` (it was persisted, displayed, and never
  applied, so the filter stayed at "record everything" whatever the user chose) and
  `crash_report_policy` now reaches the SDK consent gate.
- The HDR-handling row was permanently invisible: `setHdrDisplayPresent` had no caller, so its gate
  sat at the `false` default and an HDR user could not reach the setting at all. Now derived from
  `capabilities_.runtime.displays` on every capability delivery.

### Custom chrome — the blocker is genuinely gone, but must be proven

The 40 px native chrome was withdrawn because **`WM_NCHITTEST` is only asked of the window that owns
the pixel under the cursor**. `PreviewSurface` and `EditPlayerSurface` set `Qt::WA_NativeWindow`; Qt
then promoted native ancestors and siblings, and the measured HWND tree showed the title-bar band
owned by a child HWND. The top-level window's answers were correct — nobody was asking it.

A `QQuickWindow` is **one** top-level HWND, so `WindowFromPoint` returns it for every pixel and
`WM_NCHITTEST` becomes governable again. That makes the withdrawn design viable and lets the
`WM_SETCURSOR` / `WM_EXITSIZEMOVE` / drag-from-maximized machinery be **deleted rather than ported**,
along with Snap Layouts hover (`HTMAXBUTTON`) becoming reachable for the first time.

**The premise is conditional and must be verified, not assumed.** Any `QQuickWidget`, native-window
overlay, or remaining `QWindow` child reintroduces the exact failure — which is another reason the
editor player must not keep a child HWND. `quick/.../main.cpp` already re-implements `--hwnd-audit`
as a zero-child-HWND assertion. **Wiring that into CI is the cheapest high-value action available**:
it is the only mechanical proof of the premise the chrome design rests on.

Native behaviour that stays native regardless of frontend: `WS_THICKFRAME` (Aero Snap and Win+arrow
depend on it), `WM_NCCALCSIZE` clamping to the work area so a maximized window does not cover the
taskbar, `WM_GETMINMAXINFO` (Windows only honours the minimum size during a native drag), and the
themed DWM border with its no-NC-repaint activation trick.

---

# Cutover blockers

Ordered. Items 3, 4, 6 and 7 are *not* user-facing surfaces and are easy to under-plan.

1. **All six nav areas are now Quick.** What remains unmigrated: the editor, all chrome, 9
   dialogs/overlays and 5 capture-excluded windows.
2. **`QuickApplication` is missing 13 whole subsystems** (list above).
3. **The Quick target is a spike, not shippable.** Partly closed: it compiles `exosnap.rc` + the
   generated VERSIONINFO, stages the Qt runtime and QML imports via `windeployqt --qmldir`, stages
   `crashpad_handler.exe`, and now carries the same `/DELAYLOAD` of the Media Foundation DLLs that
   lets the Widgets build run on Windows-N without the Media Feature Pack. Still open, and all of it
   deliberately deferred until the Widgets target is actually removed (two targets producing
   `exosnap.exe` would collide): the target is still named `exosnap_quick_spike`, has no
   `OUTPUT_NAME`, no `install(TARGETS)`, and no `qt_generate_deploy_app_script`. The release script's
   required-file allowlist is also still Widgets-only (`Qt6Widgets.dll`, no `Qt6Quick*`/`Qt6Qml*`, no
   `qml/` tree), so packaging would pass on a broken artifact the moment the install rule lands.
4. ~~**Entry-point parity gap**~~ — **closed** by `ProductionBootstrap`, and as of 2026-08-10
   `ParseRelaunchArgs` / `HasVerifyUpdateReinstallRequest` have Quick callers too. What remains dead
   is the *producer* half: nothing in the Quick tree calls
   `ProductionBootstrap::requestElevatedRelaunch`, so `NotificationAction::RelaunchElevated` is still
   a deliberate no-op.
5. **`Qt6::Widgets` survives exactly one legitimate dependency once the pages are gone:
   `QSystemTrayIcon`**, which lives in QtWidgets. Every other anchor is in code slated for deletion.
   Either keep the Widgets link solely for the tray, or reimplement on `Shell_NotifyIcon`. Non-UI
   code (`services/`, `models/`, `notifications/`, `diagnostics/`, `settings/`, `viewmodels/`,
   `libs/`) is genuinely Widgets-clean — verified by grep, zero includes.
6. **Build and CI** — mostly closed. `qtdeclarative`/`qtshadertools` are installed on the runners,
   `EXOSNAP_BUILD_QUICK_SPIKE=ON` is passed by the Debug and Release configure steps, and a blocking
   `exosnap_quick_spike_qmllint` step runs in `build-test-debug` (the qmllint target is generated by
   `qt_add_qml_module`, which is why it appears nowhere in the CMake sources). Still open, and both
   matter: `packaging-smoke` and `release-candidate.yml` install the Qt Quick archives but never pass
   the flag, so **nothing ever packages or smoke-tests the Quick binary**; and no Quick harness is
   CI-invoked — `quick.qml.record_preview_smoke` and `record_preview_lifecycle` carry the `live`
   label, which every CI test step excludes, so the zero-child-HWND assertion the custom-chrome design
   rests on is still unproven by anything automated.
7. **52 shared app-layer `.cpp` are compiled twice** — 34 listed directly in both target source
   lists, plus 18 more reaching the Quick binary through the three `*_test_support` static libraries
   it links as production dependencies. They are Widgets-free, so this is not a link blocker, but a
   real `exosnap_app_core` static library is needed before cutover, and a library named
   "test_support" should not be a shipping dependency. Note also that
   `exosnap_record_viewmodel_test_support` declares `PUBLIC Qt6::Widgets`, which is how a Quick
   *adapter test* ends up linking Widgets and muddying the "Quick is Widgets-free" claim.
8. **~55 of 127 tests retire or port**, plus three harnesses. `--visual-test` and `--hwnd-audit` have
   already been re-implemented in the Quick `main.cpp` in an ad-hoc, incompatible form and should be
   reconciled rather than left forked. `--auto-record` is Widgets-bound in preview mode only; its
   headless mode is UI-agnostic. Several UI-agnostic tests link `Qt6::Widgets` only via their support
   library — one-line link edits, not ports.

---

## Ordering rationale

1. **Device** — smallest, self-contained, read-only; grows the list/matrix primitives.
2. **Diagnostics + Logs + StartupTrace + support bundle** — one cohesive subsystem behind one shared
   app-layer owner. Dominated by policy extraction and de-blocking I/O, not by QML.
3. **Shell/chrome + the missing `QuickApplication` subsystems** — the largest remaining block, and
   the one whose size is least visible from the surface inventory.
4. **Editor** — contracts first (trim once, export state once), then player → timeline → export in
   parallel.
5. **Dialogs and overlay windows.**
6. **Build/packaging/CI**, then global cutover and Widgets/QSS removal.
7. **`apps/updater`** — separately, after the main application is cut over.

`MainWindow` is not a migration step of its own: each area migrated moves another slice of its
orchestration into `QuickApplication`, and what is left at the end is the shell.
