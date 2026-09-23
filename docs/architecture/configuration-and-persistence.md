# Configuration and persistence

This document owns model/store boundaries, validation, identity and compatibility on load. [Product behavior](../product-spec.md#3-recording-defaults-and-profiles) owns preset interactions and settings semantics.

## Authorities

The live recording configuration is the active state and is persisted continuously. A named preset is a snapshot for comparison and application, not an unsaved project. The registry owns stable preset IDs, reserved built-ins, case-folded name uniqueness and valid selection. QML neither serializes a preset nor repairs registry identity.

Recording configuration uses a human-readable TOML store. Application preferences use `settings.ini`. Recording history, recovery records, session reports and update transaction documents have their own owners and lifetimes; they are not extra settings backends. A local storage format being JSON does not make its schema interchangeable with another file's.

The serialized recording configuration can carry environment/target fields for restoring the **live state**. Applying or comparing a named preset deliberately excludes capture identity, bit depth and HDR environment state where the product's preserve-environment policy applies. Do not confuse fields supported by the common serializer with fields a preset is allowed to override. H.264's required depth clamp remains a compatibility rule.

## Edit and save flow

`SettingsAdapter` holds the editable view and delegates changes to `ReconcileContainerCodecs` and `SanitizePresetConfig`. `QuickApplication` propagates the accepted value into the coordinator and store. A snapshot crosses the preparation thread boundary; the recording worker never reads a partly edited UI model.

Unknown or invalid field values follow the store's explicit repair policy. Supported schema transitions use their targeted migrations. A damaged preset entry is repaired or dropped without automatically discarding all surviving entries. A repair that loses meaningful data is reported; a mere schema version transition need not create a warning. Pre-1.0 does not imply every load performs a destructive reset or that backward compatibility is guaranteed.

Atomic save uses a temporary sibling and commit/rename. A failed save is a reported outcome, not an ignored boolean. Imports and exports use the same serialization/validation rules instead of a second partial format. Import name collisions receive a suffix; built-in names remain reserved.

`settings.ini` load distinguishes absence, successful read and an existing unreadable file. Automatic housekeeping must not overwrite an unreadable file with defaults. The first explicit user change may move the unreadable file aside to `settings.ini.corrupt` and write a fresh state. Window geometry or a one-time startup flag is not such consent.

## Display identity

Saved displays carry a composite `StableDisplayId`: device path, EDID vendor/product, optional serial, and fallback/display labels. Resolution is ranked: exact device path, matching nonempty serial with model identity, an unambiguous model match, and only for a degraded identity lacking a path, the runtime GDI-name fallback.

This is stronger than persisting `DISPLAYn`, but it is not a universal physical-panel identity guarantee. A connector path can remain identifiable while identical serial-less panels exchange cables, and a degraded GDI-only save has weaker identity. The matcher must refuse ambiguous fallback matches rather than guessing; users should confirm the preview after ambiguous hardware rearrangement. The documented algorithm, not an unconditional "never the wrong physical panel" claim, is the support boundary.

Regions store normalized coordinates relative to their anchor display's **physical** rectangle. Restoring recomputes and clamps the physical-pixel crop for the current anchor. Normalizing against Qt logical geometry would fail at mixed DPI. A missing anchor does not place a stale absolute rectangle on some other monitor. Resolution changes intentionally restore proportionally rather than pixel-exactly.

Native enumeration is done at selection/discovery boundaries and cached. Dirty comparison and save getters must not perform a fresh DXGI/DisplayConfig enumeration on each call.

## Reactive device discovery

Audio, webcam and display notifiers deliver typed snapshots. Native callbacks marshal to the owner thread, and a debounce coalesces event bursts before enumeration/comparison. Availability is not user intent: a missing device remains an unavailable selection with its stored identity intact and must not emit a settings edit or dirty a preset.

An unresolved saved display can be restored when it returns, unless the user deliberately chose another source. A semantic default microphone remains the semantic default rather than being rewritten as today's physical endpoint. Engine mid-recording loss is handled by its source HRESULT/timing path, not by turning an idle UI snapshot into a second runtime loss detector.

## Other identity and local state

Hotkey storage distinguishes never-configured from deliberately cleared. A sentinel preserves an unset binding across launches instead of reintroducing the default. Startup conflicts can clear a binding; user-chosen losses are reported, while an unavailable shipped default can be logged without a first-run alarm.

Appearance is a pair of appearance/accent preferences, migrated by defined mappings. Unknown input has a deterministic default. Session-only state, including in-depth diagnostics and verification reinstall, is not saved as a setting. Update-check results also belong to a channel/request generation and are invalidated when the channel changes.

Tooling can isolate configuration and recording output through explicit environment overrides. These overrides are not written back into user settings and are not evidence that ordinary launches use temporary storage.

## Implementation and tests

See [preset model](../../app/models/RecordingPreset.h), [preset store](../../app/settings/RecordingPresetStore.cpp), [application store](../../app/settings/AppSettingsStore.h), [display identity](../../app/models/StableDisplayId.h), [hotkeys](../../app/services/GlobalHotkeyService.cpp), and their [application tests](../../app/tests). Persisted schema versions and field-by-field migration code remain the authority for exact file compatibility.
