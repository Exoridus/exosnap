# 0062 — The process bootstrap is shared by both frontends

- Status: accepted
- Date: 2026-08-10
- Supersedes: nothing
- Related: ADR 0017 (crash capture), ADR 0033 (elevation), ADR 0044 (support bundle),
  ADR 0055 (verification reinstall), ADR 0056 (parallel Qt Quick frontend foundation)

## Context

The Qt Quick migration gave the product a second executable entry point. `app/main.cpp` had grown
22 responsibilities that have nothing to do with which toolkit draws the window: loader hardening,
the interrupted-update self-heal, the startup clock, the log sink, crash-capture initialization,
the single-instance guard, application identity, the branded icon, the Sentry verify hook and the
elevated-relaunch handoff.

The Quick entry point implemented exactly one of them (`setApplicationName`). That is not a visible
defect — a frontend that is merely *missing* one of these looks perfectly healthy right up to the
launch where it matters: a stranded install tree after an interrupted swap, a second instance
fighting over the global hotkeys, a crash that never reaches the next-launch dialog because no
session sidecar was ever written.

Duplicating `main.cpp` into the second entry point would have made the two drift silently, and the
drift is invisible in review because both files keep compiling.

## Decision

`app/bootstrap/ProductionBootstrap.{h,cpp}` owns the process-level startup and shutdown contract.
Both entry points run it; neither reimplements it.

Framework boundary: QtCore/QtGui only. No `QApplication`, no `QWidget`, no window or page types.
The frontend owns its window; the bootstrap owns the process. Anything needing a window handle
takes it as an opaque native handle (`ApplyNativeWindowIcons(void*)`).

Three phases, because the ordering constraints are real rather than stylistic:

1. `RunPreApplicationPhase()` — must be the first statement of `main()`. The DLL-search hardening
   has to precede any `LoadLibrary` the Qt platform plugin triggers, and the swap repair has to
   precede anything that resolves a path inside the install directory.
2. `MarkApplicationConstructed()` / `ApplyApplicationMetadata()` — immediately after the
   application object. Kept separate from `InitializeLogging()` so harness config isolation and
   graphics-API selection are not folded into the reported construction cost.
3. The RAII `ProductionBootstrap` object — the destructor runs the normal-shutdown sequence
   (detach the engine log sink, mark a clean exit, flush crash capture, run a pending elevated
   relaunch), so an early `return` out of a harness path cannot skip it.

Two constants are shared rather than repeated as literals, because three independent places key off
each: `kAppWindowTitle` (single-instance activation, the updater's handoff window lookup,
`swap_engine`'s `FindTopLevelWindowForProcess`) and `kApplicationName` (the settings, log and crash
directories are derived from it, so two frontends that disagree read different configuration on the
same machine).

The window title is therefore **technical identity, not copy**. `Main.qml` sets `title: "ExoSnap"`
deliberately without `qsTr()`: a localized title breaks single-instance activation and the updater
handoff simultaneously, and does so only in the localized build.

## Consequences

The hardening has a consequence that is easy to misread as a regression. `SetDefaultDllDirectories`
removes `PATH` from the loader search order **by design**. Any build-tree executable that relied on
Qt being on `PATH` stops resolving its plugins the moment the bootstrap is active — and it fails
late, at `QQmlApplicationEngine::load`, not at startup. Every frontend target therefore has to stage
its Qt runtime next to the executable (`windeployqt`, with `--qmldir` for a QML module whose QML
lives in generated resources), which is what a deployed install looks like anyway.

Deliberately **not** wrapped: `services::ParseRelaunchArgs` and
`services::HasVerifyUpdateReinstallRequest`. Both are already pure, UI-agnostic argv parsers, and a
forwarding wrapper would only add a second name for the same thing. What *is* here is the part a
pure parser cannot do: executing the relaunch, which has to hand the single-instance mutex to the
elevated successor before it starts.

Harness runs stay out of the single-instance guard entirely. Beyond a second instance exiting
immediately, the guard calls `SetForegroundWindow` on the running window — a harness would yank
focus off whatever the developer is doing, which `CLAUDE.md` rules out. For the same reason
`IsolateHarnessConfigDir` is unconditional rather than an opt-in environment variable: a harness
runs the *real* application, which persists its live configuration while it runs, and forgetting the
variable is exactly the mistake that overwrites the developer's settings.

`BeginSession` is not called by the bootstrap. The frontend must first read the previous session's
crash context, which starting a new session would overwrite, so the ordering belongs to whoever owns
the crash overlay.
