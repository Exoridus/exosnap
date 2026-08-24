# 0064 — Qt Quick is the ExoSnap main frontend

- Status: accepted
- Date: 2026-08-11
- Supersedes: ADR 0056 (the `exosnap_quick_spike` development target it introduced no longer exists)
- Related: ADR 0057 (first production area), ADR 0058 (D3D11 preview scene bridge),
  ADR 0059 (Settings area), ADR 0060 (diagnostics/logs app-layer boundary),
  ADR 0061 (editor player and timeline), ADR 0062 (shared process bootstrap),
  ADR 0063 (preview frame published edge)

## Context

ADR 0056 built the Qt Quick frontend as a second, opt-in executable
(`exosnap_quick_spike`, behind `EXOSNAP_BUILD_QUICK_SPIKE=OFF`) so it could be
developed against the shipping Qt Widgets application without risking it. That
arrangement did its job and then became the problem: two frontends meant two
places for every product decision to live, duplicate compilation of the shared
application layer, and a shipping artifact that was still the older of the two.

The evidence needed to end it was collected before this decision, not assumed by
it: a deterministic Widgets-vs-Quick A/B recording benchmark (frozen, archived
under `.workspace/benchmark-results*/`) showed no recording-correctness
regression, zero problematic frame drops in both, equivalent A/V drift, and —
after the producer-driven preview scheduling fix — CPU cost no worse than the
same-day Widgets control. A per-area runtime-parity audit and a systematic
visual comparison against the Widgets frontend closed the remaining functional
and design gaps.

## Decision

There is one main frontend and one production executable.

```text
exosnap.exe
    → ProductionBootstrap        (ADR 0062)
    → QuickApplication
    → Qt Quick / QML frontend
    → narrow QObject adapters and models
    → shared application services
    → engine
```

`exosnap` is the CMake target defined in `app/quick/ExoSnap/Quick/CMakeLists.txt`.
The name `exosnap_quick_spike`, the option `EXOSNAP_BUILD_QUICK_SPIKE` and the
idea that Quick is an experiment are all gone.

**No runtime frontend selector.** Nothing — not an argv flag, not an environment
variable, not a build option — chooses a toolkit at startup. A product with a
switchable frontend has two products to test; this one has one.

**The Qt Widgets main frontend is removed**, not preserved in parallel: the main
window, the seven pages, the custom widgets, the dialogs, the out-of-window
overlays, the QSS stylesheet and icon resource, the Widgets-only services, and
the 61 test binaries that covered them. The archived benchmark results are the
record of the frontend comparison; re-running it is not a supported workflow and
does not justify keeping a second frontend compiling. `tools/benchmark`'s
`-Frontend widgets` now refuses with an explanation rather than measuring the
Quick binary under the wrong label.

Three files that lived under Widgets directories are **kept**, because they are
toolkit-free and the Quick application uses them:
`ui/theme/ExoSnapThemes.h` (the semantic colour table `QuickThemeTokens` reads),
`ui/theme/ExoSnapMetrics.h` (the spacing/size constants the Quick shell keys its
minimum window size and title band off) and `ui/WindowGeometryPolicy` (pure
`QRect`/`QSize` clamping). `models/FrameRateLimits.h` and
`models/RecordingErrorDetailText.h` moved out of `pages/` and `ui/dialogs/` for
the same reason: neither was ever a Widgets file, they merely sat next to one.

**`Qt6::Widgets` stays linked, for exactly one reason.** Qt provides no Quick or
QML system-tray type, and `QSystemTrayIcon` is a `QtWidgets` class that requires
a `QApplication`. `app/ui/tray/TrayPresence` is therefore the one Widgets-bound
file the shipping application compiles, and `QApplication` is the application
object. Nothing the product draws is a `QWidget`. This is a documented, bounded
remainder — not a residual dependency nobody got round to cutting. A native
`Shell_NotifyIcon` replacement is post-1.0 and not required by this cutover.

**The updater stays a separate Widgets executable.** `exosnap-updater.exe` is its
own product surface with its own validated behaviour; the main-app cutover does
not migrate it, and its Widgets code and QSS are not "leftovers" of this ADR.

**Deployment uses `qt_generate_deploy_qml_app_script()`**, not the plain
`qt_generate_deploy_app_script()` the Widgets target used. The distinction is not
cosmetic: the plain variant discovers dependencies by scanning the binary's
import table, which for a QML application misses everything the engine loads at
runtime — `QtQuick`, `QtQuick.Controls` and its style plugin, `QtQuick.Dialogs`,
`QtQuick.Shapes`, `QtQml.Models`. Only the QML variant runs `qmlimportscanner`
over the module's recorded imports. The application's own QML is compiled into
the executable's resources by `qt_add_qml_module` and is not part of the deployed
tree; what gets deployed is the Qt QML runtime the engine needs to load it.

## Consequences

A build that merely links is no longer evidence that the product ships. The
failure mode a QML application adds is a binary that builds, launches from the
build tree, and then fails at `QQmlApplicationEngine::load` on a user's machine
because an import did not resolve. This compounds with the DLL-search hardening
in ADR 0062: `SetDefaultDllDirectories` deliberately drops `PATH` from the loader
search order, so "it works here" can mean nothing more than "the developer has Qt
installed". The acceptance evidence for this ADR is therefore a staged install
tree launched from outside the build tree with a sanitized `PATH` — not a build
log.

CI treats qtdeclarative and qtshadertools as mandatory archives. Omitting them no
longer degrades to a Widgets build; it produces no application at all, which
`app/CMakeLists.txt`'s Qt probe reports explicitly rather than failing deep
inside a Quick macro.

The packaged artifact grows a `qml/` subtree. The MSI harvest (ADR 0036) is
depth-agnostic and picks it up without a manifest change; the portable-package
audit in `scripts/build-release-artifacts.ps1` asserts its presence, so a
deployment that silently loses the QML runtime fails packaging instead of
failing on a user's first launch.

`app/quick/ExoSnap/Quick/` is not a migration-era path. The directory mirrors the
QML module URI `ExoSnap.Quick` under the import root `app/quick`, which is a Qt
requirement for module resolution.
