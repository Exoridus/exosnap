# ADR 0059: Qt Quick Settings Area

## Status

Accepted.

## Context

ADR 0057 migrated About and ADR 0058 established the D3D11 preview scene bridge that made Record
the first cutover-ready Quick area. Settings is the largest remaining user-facing surface: the
Widgets `ConfigPage` is roughly 318 KB of source across ten embedded sections, with capability
gating, an Expert tier, preset management, and two further embedded panels (webcam setup, hotkeys).

Almost none of its size is policy. Container/codec reconciliation lives in
`ReconcileContainerCodecs`, per-field clamping in `SanitizePresetConfig`, support gating in
`capability::CapabilitySet`, container compatibility in `ContainerCompatRegistry`, path and pattern
rules in `OutputPathPolicy` / `OutputPathValidator` / `FilenameBuilder`, and preset identity in
`RecordingPresetRegistry`. The bulk of `ConfigPage` is widget construction, manual signal blocking,
and re-seeding controls from the model after every change.

That makes Settings a mapping problem rather than a policy problem, and the mapping is what a
declarative frontend removes.

## Decision

Add a `SettingsAdapter` as the single narrow QML boundary for the Settings area, and compose the
page from Quick-native section components.

The adapter owns the live `RecordingPresetConfig` the user is editing plus the
`PersistedAppSettings` snapshot, and exposes them as typed `Q_PROPERTY` values. Every mutating
setter funnels through one `applyConfigEdit()` step that runs `ReconcileContainerCodecs` followed by
`SanitizePresetConfig` before notifying. A combination QML can produce but the recording side would
reject therefore cannot survive an edit, and the reconciliation rule has exactly one implementation.

Capability-gated dropdowns are exposed as option lists of
`{ value, label, selectable, reason }`. `selectable` and `reason` come verbatim from the capability
owner (`QueryCombo`, `QueryChroma444`, `QueryHdr10Native`, `QueryRateControlMode`) or from
`ContainerCompatRegistry`. An unavailable option stays visible and explains itself instead of
disappearing, and QML never decides availability.

The dependency direction is:

```text
capability::CapabilitySet / ContainerCompatRegistry / SanitizePresetConfig
OutputPathPolicy / FilenameBuilder / RecordingPresetRegistry
                              |
                              v
                        SettingsAdapter
                              |
                              v
        SettingsPage.qml + per-section components
                              |
                              v
                    AppShell / ApplicationWindow
```

`QuickApplication` remains the composition root. It seeds the adapter, listens for `configEdited()`
and `appSettingsEdited()`, and owns persistence (`RecordingPresetStore`, `AppSettingsStore`) and
propagation into `RecordingCoordinator`. QML sees no store, no coordinator, and no service.

Preset state moves to `RecordingPresetRegistry` inside `QuickApplication`, replacing the ad-hoc
`user_presets_` / `selected_preset_id_` pair. Selection, save-as, rename, delete, reset, and import
now use the registry's existing invariants (built-ins reseeded, ids deduped, names fold-deduped,
selection repaired) rather than a second implementation.

### Two deliberate deviations

**File dialogs use `QtQuick.Dialogs`, not `QFileDialog`.** A `QFileDialog` would relink Qt Widgets
into the frontend the migration exists to remove. `FolderDialog` / `FileDialog` are native on
Windows and require only `Qt6::QuickDialogs2`. The chosen URL is handed to the adapter, which
converts it to a filesystem path — QML never assembles one.

**Custom output resolution needs an explicit pending state.** `SanitizeOutputResolution` snaps an
incomplete custom size back to `Native`, so "Custom selected, dimensions not yet entered" is not
representable in the model, and a naive binding makes Custom unreachable. The adapter holds that
transient editing state (`custom_resolution_pending_` plus the stashed dimensions) in the
presentation layer. The sanitizer stays the single owner of which sizes are valid; the adapter only
remembers what the user is in the middle of typing.

## Coexistence

- `exosnap` remains the shipping executable and is unchanged.
- Widgets `ConfigPage` remains present and behaviourally unchanged as the parity reference.
- `engine`, recording policy, preview, and the Record area are unchanged.
- `app/CMakeLists.txt` gains `QuickDialogs2` to the Quick-only `find_package` component list; the
  shipping target's dependency graph is untouched.

## Validation contract

- The adapter's focused tests cover delegated reconciliation, option gating with reasons, the
  edit-notification contract, quality/frame-rate mapping, codec-gated relevance, preset-name
  folding, update-status presentation, and the custom-resolution pending state.
- All packaged QML passes `qmllint` with zero warnings and zero informational notes.
- The existing CTest QML smoke launches the packaged module, which instantiates `SettingsPage`.

## Follow-up work completed after acceptance

The four gaps this ADR originally listed are now closed:

- **Theme.** `QuickThemeTokens` resolves the four shipped themes from the shared
  `ui/theme/ExoSnapThemes.h` table and exposes them as a QML singleton; `ExoTheme.qml` delegates its
  colour tokens to it and keeps only metrics and fonts. The theme table's `rgba(...)` line tokens are
  parsed into alpha-carrying colours, and the two tinted notice surfaces (which the QSS pipeline
  composes with alpha and the table therefore does not name) are derived per theme instead of
  hardcoding one palette's values. An unknown id falls back to the shipped default. The picker now
  reads its option list from the same table, which removed a real defect: the hand-written list
  offered `dark-contrast` / `light-default` / `light-contrast`, none of which exist.
- **Hotkeys.** `Win32HotkeyRegistrar` moved out of `MainWindow.cpp` into
  `services/Win32HotkeyRegistrar.h`, so both frontends register through one implementation. The
  Quick composition root installs a native event filter for `WM_HOTKEY` on the Quick window's HWND
  and routes actions into the existing record commands. Validation, conflict detection, blocked
  combinations and defaults stay entirely inside `GlobalHotkeyService`; the capture field only
  reports a raw key plus modifier bits.
- **Audio VU meters.** The adapter receives the same dock-level values the Record page gets, from
  the one computation in `updateMeters()`, so the two areas cannot disagree.
- **Webcam.** Device, capture format, mirror, overlay opacity and the chroma-key chain are migrated.
  Placement stays in the Record preview, matching the shipped product copy. Percentage-based
  chroma parameters convert at the adapter boundary; the engine keeps its normalized `[0,1]` form.

## Known limitations

- Deep-link scrolling to a named section (`scrollToSection`) has no Quick equivalent yet. Nothing in
  the Quick frontend currently raises such a link, so this is only needed once Diagnostics gains its
  "fix this in Settings" affordances.
- The webcam card has no live preview thumbnail. The shipped copy already directs placement and
  framing to the Record preview, which owns the shared capture, so a second preview consumer was not
  introduced.
