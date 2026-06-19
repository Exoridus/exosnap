# ADR 0020: Config Serialization Format — TOML for Presets, INI for App Settings

## Status

Accepted — implemented in 0.5.0 (feat/0.5.0-preset-toml).

## Context

Prior to 0.5.0, all persistent configuration used `QSettings` with `IniFormat`:

- `presets.ini` — the recording-preset store (preset list, selected/default id,
  per-preset capture/output/video/audio/webcam config).
- `app/settings.ini` — flat app-level settings (window geometry, last-used
  output folder, update-check state, etc.).

The preset file grew substantially with 0.5.0: it now contains nested structures
(audio source rows as a flat `aud_row_N_*` key family, chroma-key sub-table,
rate-control parameters), and the export/import feature is expected to produce
files a user could inspect or hand-edit. The QSettings INI serialization of the
nested preset data was awkward: the flat `aud_row_N_*` key family does not read
as naturally as a structured list, and the format provides no mechanism to express
arrays of objects without key-mangling.

TOML (Tom's Obvious, Minimal Language) provides:
- `[[array-of-tables]]` syntax that maps cleanly onto the audio source-row list
  and any future list-shaped sub-config.
- Human-readable, hand-editable syntax — the explicit goal for preset export/import.
- Strong typing for booleans, integers, and floats, avoiding the stringly-typed
  coercions required by INI.

The `tomlplusplus` library (v3.4.0) was already vendored via `FetchContent` in
`third_party/CMakeLists.txt` and not yet used; this ADR activates it.

## Decision

### TOML for the recording-preset store and export/import

`RecordingPresetStore` is rewritten to read and write TOML instead of
`QSettings` INI:

- The live store file is renamed from `presets.ini` to `presets.toml`.
- Export files (single and all-presets) use the same TOML schema so there is
  exactly one serialization code path.
- `kPresetSchemaVersion` is bumped from 5 to 6. Old `presets.ini` files from
  0.4.x and earlier are not migrated — this follows the pre-1.0 reset policy
  (see below).

The TOML schema uses a top-level `[[presets]]` array-of-tables. Each element
contains nested sub-tables: `[presets.capture]`, `[presets.output]`,
`[presets.video]`, `[presets.audio]`, `[presets.webcam]`. The audio source rows
are stored as `[[presets.audio.sources]]`, which is a nested array-of-tables
and replaces the former `aud_row_N_*` flat key family.

### QSettings / INI for flat app settings

`AppSettingsStore` (and any other flat key-value store) stays on `QSettings`
`IniFormat`. The split is deliberate:

- App settings are flat, homogeneous, and rarely hand-edited. QSettings is
  perfectly adequate.
- Preset config is structured, deeply nested, and intended to be human-readable
  (especially for export/import). TOML earns its place here.

Mixing serialization formats in a single project is not a code-smell when the
formats are chosen to fit the shape of the data.

### Pre-1.0 reset-not-migrate policy

ExoSnap is pre-1.0 and has no backward-compatibility obligation (`feedback_prerelease_breaking_changes`).
On load of an old `presets.ini`, an incompatible schema version, or any parse
error, `RecordingPresetStore::Load()` discards the file and returns a seeded
default state (`was_reset = true`). No migration shim is implemented.

`ImportPresetsFromFile` follows the same principle with schema version mismatch:
it attempts best-effort parsing and sanitization, but if no valid items survive
it returns an empty vector and sets the error string. No migration is attempted.

### Atomic write

Both `Save()` and the Export helpers use `QSaveFile`, which writes to a
temporary path and atomically renames it over the destination on `commit()`.
This ensures that a crash or power loss during a write cannot corrupt the
existing preset file.

### Parse-error robustness

`ParseTomlFile()` uses toml++'s non-throwing `toml::parse_file` API and wraps
any exceptions from older overloads in a try-catch. A parse error never
propagates out of `Load()` or `ImportPresetsFromFile()`; it always degrades
gracefully to a reset state or an empty import result.

## Consequences

- Users who upgrade from 0.4.x to 0.5.x will have their presets reset to the
  default on first launch. This is intentional and consistent with the pre-1.0
  policy; it is not a regression.
- The `tomlplusplus` INTERFACE target is linked into the `exosnap` executable
  and the three preset-related test targets
  (`recording_preset_store_tests`, `preset_export_import_tests`,
  `audio_encoding_preset_tests`). toml++ is header-only so this adds no
  compiled library; it increases the compile time of `RecordingPresetStore.cpp`
  by the cost of parsing the toml++ headers once.
- Preset export files now have a `.toml` extension and are valid, hand-editable
  TOML. The `export_kind = "single" | "all"` key allows tools and future code
  to distinguish single-preset exports from all-presets exports.
- The existing enum-to-string converters (e.g. `ContainerToString`,
  `AudioSourceKindToString`) are retained unchanged. The TOML boundary conversion
  is `QString::toStdString()` / `QString::fromStdString()`.
