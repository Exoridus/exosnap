# ADR 0056: Parallel Qt Quick Frontend Foundation

## Status

Accepted, and **superseded by ADR 0064**. The parallel `exosnap_quick_spike` target this ADR
introduced no longer exists: the Qt Quick frontend is the shipped application. Everything below
describes the arrangement while the migration ran and is kept as history.

## Context

ExoSnap's shipping frontend is a Qt Widgets application. `MainWindow` composes the Widgets pages,
and `RecordPage` currently owns both `RecordViewModel` and `RecordingCoordinator`. The recording
engine, capability resolver, compatibility registry, and track-resolution code already sit below
that frontend boundary and must not acquire Qt Quick or QML dependencies during a UI migration.

The migration needs an executable proof before any shipping page is rewritten: a real onscreen Qt
Quick window, a modern Qt 6 QML module, and a narrow way to bind existing app state. It must also
leave the DXGI preview path alone until native D3D11 texture import is designed and measured.

## Decision

Add an opt-in development target, `exosnap_quick_spike`, behind
`EXOSNAP_BUILD_QUICK_SPIKE` (default `OFF`). The target uses `QGuiApplication`,
`QQmlApplicationEngine`, and an `ApplicationWindow`; it does not embed Quick in Widgets and does
not use `QQuickWidget`. Its QML is packaged by `qt_add_qml_module` under the `ExoSnap.Quick` URI and
loaded with `QQmlApplicationEngine::loadFromModule`.

The QML boundary is an app-layer `RecordViewModelAdapter`. It is a `QObject` registered in the QML
module and exposes a small read-only snapshot of existing `RecordViewModel` fields through
`Q_PROPERTY`. It does not own the source model, mutate it, or derive policy. Because the existing
model deliberately has no Qt signal mechanism, the app composition owner calls `synchronize()`
after canonical state changes; the adapter compares snapshots and emits only the property signals
whose values changed.

The dependency direction is:

```text
recorder_core / capability (no UI framework)
                ^
                |
pure app models and RecordViewModel (unchanged)
                ^
                |
app-layer RecordViewModelAdapter (QObject/Q_PROPERTY)
                ^
                |
ExoSnap.Quick QML module + exosnap_quick_spike (QQuickWindow/ApplicationWindow)
```

The shipping `exosnap` target remains Widgets-only and does not link Qt Quick, Qt QML, the adapter,
or the spike. The option is off in all existing presets, so the normal build and packaging graph is
unchanged. Both frontends temporarily compile beside the same lower-layer headers; there is no
runtime embedding or mixed window hierarchy.

## Unchanged layers

- `recorder_core`, capture, encode, mux, diagnostics, and capability libraries
- `RecordViewModel`, `PresentationState`, and `PresentationStateBuilder`
- `RecordingCoordinator` and its callback/state-policy ownership
- `MainWindow`, `RecordPage`, and the shipping Widgets executable
- the QSS/theme implementation
- `PreviewSurface` and `DxgiPreviewRenderer`, including their native child HWND and D3D11 paths

No codec, container, capability, track-resolution, or recording-state rules are expressed in QML.
No preview texture is copied, read back, or imported in this slice.

## Follow-up

ADR 0057 replaces the fixture-backed entry point with a real composition root and migrates About as
the first complete, GPU-independent Quick area. The Record adapter remains as a boundary proof but
is not used by that production vertical slice; its update cadence will be decided when Record state
is migrated after the preview performance gate.

## Consequences

- Qt Quick/QML can be configured, compiled, and launched independently of the shipping frontend.
- QML sees typed, tool-visible properties rather than context properties or policy-bearing
  JavaScript.
- The manual synchronization seam is intentionally temporary. It is sufficient for the isolated
  proof, but the next slice must connect it to a shared app-layer owner before migrating real UI.
- QSS and Widgets theme tokens are not reused by the spike. A Quick-native theme layer remains a
  later migration concern rather than being improvised in this foundation.
