# AGENTS.md

## Project intent

Build a Windows-native, diagnostics-first recording application MVP with a high-performance C++ engine and a Qt 6 + Qt Quick/QML user interface.

The main frontend is Qt Quick (ADR 0064). `Qt6::Widgets` remains linked for
`QSystemTrayIcon` alone, and the separate updater executable keeps its own
Widgets UI; neither is a second frontend. Business and product policy stays in
C++ — QML owns presentation, layout and interaction only.

## Canonical product decisions

Product decisions (defaults, navigation, audio/video model, container/codec rules) are **not
duplicated here** to avoid the two drifting apart. `CLAUDE.md` and `docs/product-spec.md` are the
single authoritative source for user-visible product behavior — read them before any change that
could be visible to a user. This file only adds constraints on *how* agents implement that
behavior, not *what* the behavior is:

- UI must not duplicate track resolution logic. It submits editable source rows; the engine
  returns resolved tracks.
- When switching containers, the selected audio codec must be reconciled to a valid codec for the
  new container; reconciliation is engine logic, never duplicated in the UI.
- If a hotkey starts recording while the app window is visible, activate the Record view. If
  minimized, do not restore the window.

## Architectural rules

- Keep the recording engine independent from UI concerns.
- Keep capture, encode, mux, diagnostics, telemetry, and UI responsibilities separate.
- Use structured data models rather than ad hoc UI-bound state.
- Prefer explicit state machines for recording session lifecycle.
- Every live metric must have:
  - source
  - meaning
  - update cadence
  - UI consumer
  - log consumer
- Prefer isolated capability probes before production integration.
- Do not optimize speculatively; add profiling hooks and measure.

## Documentation rules

- Update specs when product behavior changes.
- Add an ADR for any cross-cutting architectural decision.
- Each milestone must end with:
  - implementation notes
  - tests
  - doc updates
  - explicit unresolved issues, if any

## Agent workflow

- Opus owns architecture, product decisions, cross-cutting reviews, and final approval.
- Sonnet implements substantial bounded features from approved specs.
- Codex handles repo bootstrap, mechanical work, tests, refactors, build fixes, and explicit task lists.
- No agent should silently expand MVP scope.

## Fast Iteration Policy

This policy applies to all future ExoSnap agent sessions.

### Scope

- Normal feature slices target 30-60 minutes.
- Keep each slice to one subsystem.
- Do not broaden scope without a blocking technical need.
- Record minor P2/polish findings for consolidated review instead of fixing everything immediately.

### Agent use

- Do not launch multiple Explore agents when direct repository inspection is sufficient.
- Use at most one architecture/exploration pass for normal slices.
- Use workers only for meaningful, clearly separated packages.
- Run independent workers in parallel worktrees only when their file ownership is disjoint.
- Never let workers concurrently edit high-churn files such as:
  - `MainWindow.cpp`
  - `RecordPage.cpp`
  - `ConfigPage.cpp`
  - shared stores/view models
  - shared CMake files

### Development validation

During implementation, workers should use the smallest sufficient validation:

1. Build only the affected target.
2. Run focused tests for the changed subsystem.
3. Do not run full Debug builds, full CTest, quality suite, and Release build after every worker or edit cycle.

Canonical principle: `Minimal validation during development; complete validation once at the final gate.`

### Running tests

`scripts/run-tests.ps1` is the standard entry point for running the suite — use
it instead of invoking `ctest` directly. It sets the required environment (a
throwaway `EXOSNAP_CONFIG_DIR`, `QT_QPA_PLATFORM=offscreen`, `QT_PLUGIN_PATH`, Qt
on `PATH`), writes the full log to `<BuildDir>/Testing/last-run.log`, and prints
only a compact summary plus the exact failing gtest cases.

- Whole suite: `pwsh scripts/run-tests.ps1`
- Focused: `pwsh scripts/run-tests.ps1 -Filter <regex>` (matches test **binary**
  names, e.g. `recorder_core.` — each CTest entry is one binary, not one gtest
  case; gtest_main runs all cases in-process and still prints the exact failing
  `Suite.Case`).
- No-GPU host: `pwsh scripts/run-tests.ps1 -ExcludeLabel live` skips binaries
  that issue real hardware queries (DXGI adapter enumeration, GPU capability
  probes).
- Build first: add `-Build` to do a full build before testing.

### AddressSanitizer

`windows-x64-asan` (MSBuild) and `windows-x64-ninja-asan` (Ninja, needs a VS
Developer shell) build the whole tree with `/fsanitize=address`. Use them when
chasing a crash whose stack makes no sense — a use-after-free surfaces as an
unrelated crash somewhere else entirely, and ASan turns that into a report at
the first invalid access with the allocation and free stacks attached.

- Build + test: `cmake --preset windows-x64-asan && cmake --build --preset windows-x64-asan`,
  then `pwsh scripts/run-tests.ps1 -BuildDir build/windows-x64-asan -Config Debug`.
- The sanitizer runtime (`clang_rt.asan*dynamic-*.dll`) ships next to `cl.exe`
  and is never on PATH; the build stages it beside every binary. A missing
  'C++ AddressSanitizer' VS component fails configure with an explicit message
  rather than at first launch with 0xC0000135.
- `/RTC1` is stripped from the Debug flags — MSVC rejects it alongside
  `/fsanitize=address`. ASan subsumes what it checked.
- Expect the suite to run noticeably slower than a plain Debug run. This is a
  diagnostic preset, not a replacement for the normal test gate.

### Window-ownership and chrome auditing

`exosnap.exe --hwnd-audit` reports three things about the real top-level window
and exits 0 only when all three hold:

```
quick-hwnd-audit: child_hwnds=0
quick-hwnd-audit: style=0x96040000 exstyle=0x00000100 caption=0 thickframe=1 border=0
quick-hwnd-audit: nonclient_inset=0,0,0,0 native_titlebar=0
```

1. **`child_hwnds=0`** — no native child windows. Tests and `--visual-test` are
   both blind to this: they see objects and pixels, never which WINDOW owns a
   pixel — and a native child never lets the top-level window see a
   `WM_NCHITTEST`, so it silently breaks drag, resize and Snap over whatever it
   covers.
2. **No non-client area** — the 40 px title band is the product's own, so Windows
   must reserve nothing outside the client rect. A non-zero top inset is a native
   caption drawn ABOVE ours, i.e. two title bars.
3. **`WS_THICKFRAME` present** — a frameless window has no caption to offer the
   system, so this bit is the only thing keeping the native resize drag, Aero
   Snap and Win+Arrow alive. Qt drops it when it makes the window visible unless
   `QuickWindowChrome` re-asserts it; nothing about the window LOOKS wrong when
   it is missing.

Run it after any work on window chrome, hit-testing or overlays. The window is
never activated.

Zero is the expected result and is the point of the Qt Quick migration: the
preview and the editor player are scene-graph items, not child windows. The
richer "does a child cover a region the top-level must hit-test" report belonged
to the Widgets shell and went with it; a non-zero count is now the whole signal,
because in a Quick build any native child at all is the regression.

### Startup window geometry

`exosnap.exe --window-trace` (or `EXOSNAP_WINDOW_TRACE=1`, for a launch that
cannot take extra argv) writes one line per startup geometry milestone to the
application log:

```
window-trace: persisted 400,120 1280x820 maximized=0
window-trace: resolved  400,120 1280x820 maximized=0
window-trace: pre-show  qt=400,120 1280x820 ... native_window=400,120 1280x820 native_visible=0 ...
window-trace: post-show qt=400,120 1280x820 ... native_window=400,120 1280x820 native_visible=1 ...
window-trace: first-frame ...
```

Each line carries both spaces at once — Qt's logical geometry and believed frame
margins next to the native window and client rects — because the whole class of
defect here is the two disagreeing about what a rect means.

The property to check is **not** "it ends up in the right place". It is that
`pre-show` already holds the final rect while `native_visible=0`, and that
`post-show`, `first-expose` and `first-frame` never change it. A window that
reaches the right rect a frame late reached it visibly.

`--window-trace` also logs the Win32 messages that decide the rect
(`window-msg: WINDOWPOSCHANGING/NCCALCSIZE/GETMINMAXINFO/STYLECHANGED`) until the
first frame. Those are *sent*, not posted, so they are invisible to any log
written from Qt signals: by the time `xChanged` arrives the decision is made and
its cause is gone.

Combine with `--hwnd-audit` for a run that measures all of this and exits without
ever activating the window.

### Preview presentation tracing

`EXOSNAP_PREVIEW_TRACE=1` writes one `preview-trace:` line per Record-preview
presentation lifecycle transition (window expose, screen change, scene-graph
re-initialisation):

```
preview-trace: screen-changed screen=\\.\DISPLAY2 dpr=1.00 exposed=1 visible=1 loop=1 owed=1 reissued=1 publishes=412 wakeups=409 updates=410 renders=409
```

`owed=1` means a producer published a frame that no render pass has followed —
i.e. the newest frame is sitting in the transport and the screen has not shown
it. `reissued=1` is this transition asking for the render that frame is owed.
The pair is the whole contract: a transition that finds `owed=0` does nothing,
and a frame that is owed one is never left waiting for an unrelated redraw.

Off by default and read once, because the point of the preview's redraw gate is
that a quiet desktop costs nothing. It exists because the defect it was written
for — the preview freezing when the window crosses a monitor boundary, until the
mouse moves — is invisible to every other instrument: a screenshot cannot show
that frames stopped arriving, and the metrics overlay reports rates rather than
the transition that broke them.

### Final validation

Run once after the integrated branch is complete:

- format check
- `git diff --check`
- full Debug build
- focused tests if still useful
- full CTest
- quality/static checks once
- Release build only at the final gate or when Release-specific behavior is in scope

Do not invoke `check-quality.ps1` after already running the same complete configure/build/test sequence unless the script provides additional required checks that cannot be invoked separately. Prefer invoking only the missing static-analysis step when possible.

### Test and visual budgets

For a normal feature slice:

- approximately 5-15 targeted new tests
- approximately 2-4 Visual Harness scenarios
- no exhaustive matrix unless the feature is inherently high-risk
- no fragile physical-device or multi-step external automation

### Acceptance

- Green automated tests and clean deterministic visual scenarios are sufficient for implementation waves.
- Physical hardware checks and broad visual/product review are deferred to consolidated final review rounds.
- One or two documented minor limitations do not block merge when core behavior is correct.

