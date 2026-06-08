# ADR 0003: Complete Recording Presets

## Status

Accepted

## Context

ExoSnap v0 stored partial "profiles" that covered only container, codecs, and quality.  Capture target
selection, audio source toggles, webcam settings, and countdown were global application state kept
separately in `AppSettingsStore`.  This led to:

- Settings that were scoped to a "profile" in the UI but not actually persisted as part of it.
- A misleading hint text ("Sources and audio are saved separately").
- Webcam state that could not vary between recording setups.
- No concept of a startup default — the last-used partial profile was restored but other settings
  floated independently.

The MVP requires a coherent "preset = complete setup" model so the user can switch between fully
configured recording workflows without manually re-adjusting every control.

## Decision

### Canonical preset schema (schema version 1)

`RecordingPreset` is the unit of storage.  Its embedded `RecordingPresetConfig` holds:

| Sub-struct | Content |
|---|---|
| `PresetCaptureTarget` | `kind` (Display/Window/Region), description-based `display_key` / `window_key`, optional `region` in virtual-screen coordinates, `region_display_key`. Raw platform handles (HWND, HMONITOR) are never stored. |
| `OutputSettingsModel` | Container, video codec, audio codec, quality, FPS, CFR/VFR, cursor-capture flag, output folder, naming pattern. |
| `VideoSettingsModel` | CQ value, frame rate, timing mode, cursor toggle. |
| `capability::AudioUiState` | Per-source enabled/separate-track flags, mic device id. |
| `WebcamSettings` | Device, resolution, FPS, mirror, overlay placement (`WebcamOverlay`), enabled flag. |
| `int countdown_seconds` | One of {0, 3, 5, 10}. |

`RecordingPresetConfig` is validated and sanitized by `SanitizePresetConfig` before storage.

The canonical **built-in default preset** (id `preset.default`) is:

- Capture: Display (primary, empty key = "any").
- Format: MKV + AV1 (Nvenc) + Opus, quality High, CFR 60 fps, cursor ON.
- Audio: System ON, App OFF, Mic OFF, separate tracks OFF.
- Webcam: OFF.
- Countdown: 0.

### `RecordingPresetStore` (persistence)

Presets are serialized to `presets.ini` in the user profile directory using Qt's `QSettings` INI
format.  The file contains:

```
[store]
schemaVersion = 1
defaultId     = preset.default
selectedId    = preset.<hex16>

[preset.<hex16>]
name = ...
capture.kind = display
...
```

On load, if `schemaVersion < 1` or the key is absent, the store performs a **hard reset** to a
single factory-default preset.  No migration of v0 partial profiles is performed (pre-v1.0
breaking changes are acceptable per project policy).

### `RecordingPresetRegistry`

`RecordingPresetRegistry` is a Qt-independent in-memory registry.  It enforces:

- `presets_` is always non-empty.
- `selected_id_` and `default_id_` always point to existing presets.
- Mutations (Add, Save, Duplicate, Rename, Delete, ResetAll) maintain these invariants.

`IsSelectedDirty` compares the live working config against the selected preset's saved config using
`ConfigDirtyEquivalent`.

### Atomic apply + re-entrancy guard

Applying a preset is handled by `MainWindow::applyPreset` behind a boolean re-entrancy guard
(`applying_preset_`).  The apply sequence is:

1. Reject if recording is active.
2. Set guard.
3. Call `config_page_->setOutputSettings`, `setVideoSettings`, `setAudioUiState`,
   `setWebcamSettings`, `setPresetOptions`.
4. Propagate to `RecordPage` via the existing settings-changed signals.
5. Clear guard.

No UI signal loops can occur because the guard prevents re-entry from settings-changed callbacks.

### Dirty-state design (`ConfigDirtyEquivalent`)

`ConfigDirtyEquivalent` is identical to `NormalizedConfigEquals` EXCEPT that the capture
sub-struct (`kind`, `display_key`, `window_key`, `has_region`, `region`, `region_display_key`) is
**excluded** from comparison.

Rationale: capture identity is transient.  The default preset stores `display_key = ""` (meaning
"primary/any").  Once applied, the live policy holds a concrete resolved key.  Including capture in
the dirty comparison would make every preset appear dirty on startup, on monitor replug, and after
auto-resolution.  This is explicitly a design choice — temporary availability changes must not mark
the preset dirty.

`NormalizedConfigEquals` is preserved for persistence round-trip verification.

### Startup boots to the default preset

On first show (`showEvent`), `MainWindow` reads the `default_id` from the registry and calls
`applyPreset` for that preset.  The user sees the complete default setup immediately without
needing to select anything.

### `AppSettingsStore` reduction

`AppSettingsStore` now stores only:

- Global hotkey bindings.
- Window geometry (size + position).

All codec, quality, audio, webcam, and output settings are owned by `RecordingPresetStore`.
`settings.json` version is bumped to **6**; older files are discarded.

## Consequences

- A user can create named recording presets and switch between them with a single combo-box
  selection.  Each preset carries the complete setup.
- The startup default is explicit and user-controllable ("Set as default preset").
- Dirty state is meaningful and excludes transient capture identity changes.
- Old partial profiles (v0) are discarded on first launch — no migration, consistent with the
  pre-v1.0 breaking-changes policy.
- Import/export actions are removed.
- Webcam settings are now per-preset (previously app-global).
- The preset card UI exposes: selector, Save (dirty-gated), Save As…, and a Manage overflow menu
  (New, Duplicate, Rename, Delete, Set as default, Reset changes, Reset all to factory defaults).
- Visual-test harness scenarios drive the preset card with synthetic `ProfileOption` data only;
  `RecordingPresetStore` and `RecordingPresetRegistry` are never touched in the harness.

## Unresolved Issues

- Capture target restore (matching `display_key` / `window_key` at apply time) is best-effort:
  if the stored key does not match any live target the preset applies but the capture target
  selection falls back to the first available target.
- Region geometry is stored in virtual-screen coordinates; multi-monitor layout changes can render
  a stored region invalid.  The store does not yet detect or warn about this.
