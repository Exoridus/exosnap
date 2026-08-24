# ADR 0057: Qt Quick First Production Area

## Status

Accepted, and **superseded by ADR 0064** where it describes the target layout. The vertical this
ADR established is now part of the shipped Qt Quick application; statements below about
`exosnap_quick_spike` and `EXOSNAP_BUILD_QUICK_SPIKE` describe the migration-era build and are
kept as history.

## Context

ADR 0056 proved that an isolated `ApplicationWindow`, a modern Qt 6 QML module, and an app-layer
adapter can coexist with the shipping Widgets frontend. The executable still used fixture Record
state and did not prove a complete product area, real application composition, shared product data,
or reusable Quick presentation primitives.

Logs was evaluated as the preferred first area. The existing `LogsPage` is not a small isolated
surface: it combines an incrementally updated bounded history, filtering, search, export,
`StartupTrace`, and support-bundle creation whose inputs currently come from `MainWindow` runtime
capabilities. Migrating it completely would couple the first production screen to Diagnostics and
support-bundle refactoring. About is the agreed low-risk fallback and is independently complete.

## Decision

Turn `exosnap_quick_spike` into the parallel Quick development frontend's first production vertical
slice while retaining its opt-in target name and default-off build option.

`QuickApplication` is the composition root. It owns the settings snapshot, the area adapter, and
the `QQmlApplicationEngine`, in that destruction-safe order. It reads the real update channel from
`AppSettingsStore`, resolves actual build and installation metadata, constructs an
`AboutViewModelAdapter`, and injects that typed adapter as the root's initial property. QML receives
no service object and no context property.

About's data and clipboard formatting live in the shared app-layer `AboutInfo` model. Both the
Widgets `AboutPage` and Quick adapter consume it, so release identity, commit availability,
installation mode, notices, and copied support text have one definition. The adapter owns only
presentation-facing `QObject` state and narrow actions: copy details and open the four About links.
Executable hashing remains asynchronous.

The dependency direction is:

```text
generated build metadata / update install facts / persisted app settings
                              |
                              v
                   app-layer AboutInfo
                      /               \
                     v                 v
          Widgets AboutPage    AboutViewModelAdapter
                                      |
                                      v
                              AboutPage.qml
                                      |
                                      v
                         AppShell / ApplicationWindow
```

The QML module also introduces only the primitives this area needs: `ExoTheme`, `ExoButton`,
`ExoNotice`, and `AboutMetadataRow`. The shell renders the canonical six navigation items in their
specified order. Only About is enabled in the development frontend until another area is migrated;
the shipping Widgets navigation remains fully functional.

## Coexistence

- `exosnap` remains the shipping executable and does not link Qt Quick or QML.
- `exosnap_quick_spike` remains opt-in through `EXOSNAP_BUILD_QUICK_SPIKE=ON`.
- Widgets `AboutPage` remains present and behaviorally unchanged.
- `engine`, recording policy, preview, DXGI rendering, Settings, Diagnostics, Logs, Record,
  and Editor are unchanged.
- The Quick target uses the existing brand and font resources without copying their assets.

## Validation contract

- Shared About formatting and the Widgets page retain their focused tests.
- The Quick adapter tests real asynchronous copy/hash behavior and immutable metadata exposure.
- A CTest smoke launches the packaged QML module offscreen from the real executable.
- The Quick development executable can save a deterministic About screenshot through
  `--visual-test <path>` for resize and visual review.
- All QML files pass deterministic lint, `qmllint`, and the six-pass `qt-qml-review`.

## Next migration slice

Prototype and benchmark native D3D11 texture presentation in a real Qt Quick scene graph against
the existing child-HWND renderer. Keep that work isolated from further page migration and use it as
the planned preview go/no-go gate. If the gate is green, migrate Record controls separately and then
return to higher-complexity areas such as Logs.

## Known limitations

- The Quick theme currently contains only the dark-default tokens required by About. Runtime theme
  selection and the complete control set remain later design-system work.
- Five navigation destinations are intentionally disabled in the development frontend.
- The retained Record adapter is no longer injected into the Quick root; it remains a tested
  boundary example until Record migration begins.
