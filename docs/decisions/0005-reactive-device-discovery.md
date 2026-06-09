# ADR 0005: Reactive Device Discovery

## Status

Accepted

## Context

ExoSnap needs to detect hardware changes at runtime without requiring the user to restart the application. Three device categories require reactive handling:

- **Audio endpoints**: microphone inputs and render outputs tracked via WASAPI `IMMNotificationClient`.
- **Webcams**: USB camera connect/disconnect events tracked via Win32 `WM_DEVICECHANGE` on a message-only window with `RegisterDeviceNotification(KSCATEGORY_VIDEO)`.
- **Displays**: monitor connect/disconnect, resolution/DPI/orientation changes tracked via Qt screen signals (`screenAdded`, `screenRemoved`, `geometryChanged`, ...).

Before this feature the UI re-enumerated devices only on explicit user action (Refresh button) or on app startup. Hot-plug events were silently missed unless the user happened to click Refresh at the right time.

## Decision

### Three typed QObject notifier services

One `QObject` notifier per device category lives on the Qt main thread:

- `AudioDeviceNotifier`: wraps `IMMDeviceEnumerator` + `IMMNotificationClient` (MTA COM thread → `QMetaObject::invokeMethod(QueuedConnection)` → debounce timer → enumerate → emit).
- `WebcamDeviceNotifier`: wraps a message-only `HWND` + `RegisterDeviceNotification` (WNDPROC main thread → `QMetaObject::invokeMethod(QueuedConnection)` → debounce → enumerate → emit).
- `DisplayDeviceNotifier`: connects to `QGuiApplication` screen signals (already on main thread → debounce timer → enumerate → emit).

All three follow the same pattern:
1. A 200 ms debounce timer coalesces rapid burst events from the OS into a single refresh.
2. The refresh builds a typed value-object snapshot (no COM/native handles), sorts deterministically by stable id, and compares against the last published snapshot.
3. If the snapshot changed, `snapshotChanged(snapshot, reason)` is emitted. No-op otherwise (dedup).

### Typed snapshots

- `AudioDeviceSnapshot`: vectors of `AudioInputDeviceInfo` sorted by `device_id`, plus `default_input_id` / `default_output_id` strings.
- `WebcamDeviceSnapshot`: vector of `WebcamDeviceInfo` sorted by `id`.
- `DisplaySnapshot`: vector of `DisplayInfo` sorted by `id` (GDI device name on Windows).

The `DiscoveryReason` enum (`Startup`, `Rescan`, `DeviceAdded`, `DeviceRemoved`, `DeviceStateChanged`, `DefaultChanged`, `PropertyChanged`, `GeometryChanged`) is passed alongside the snapshot so consumers can show context-appropriate UI.

### Stable-ID selection preservation

When a snapshot arrives at a UI page:

- The configured stable ID is read from the current audio/webcam/display state.
- If the ID is present in the new snapshot, the selection is restored to that exact entry.
- If the ID is absent, a `(unavailable)` placeholder is shown and the ID is **not** changed.
- Semantic Default (`nullopt` for mic) stays at index 0 regardless of which physical device is now the Windows default.
- **No `audioSettingsChanged` / `webcamSettingsChanged` / `recordingConfigChanged` signal is emitted** for availability-only changes. Preset dirty state is not set.

### Honest invalidation

When the configured capture target (Display, Window, Region) becomes unavailable:

- `selected_target_index` is set to `-1` (unresolved).
- The preview stops; no stale frame is shown.
- **No silent fallback to another target is performed.**
- The Rescan button (and future reconnect logic) is the explicit recovery path.

### Rescan fallback

`AudioDeviceNotifier::rescan()`, `WebcamDeviceNotifier::rescan()`, `DisplayDeviceNotifier::rescan()` provide immediate synchronous refresh using the same compare/emit path as the debounced refresh. `MainWindow` connects the Settings Audio "Rescan" button to `audio_notifier_.rescan()`.

### Dependency injection for tests

All three notifiers accept an `Enumerator` functor (`setEnumeratorForTest()`) and a `simulateNativeEvent()` method that drives the same debounce path a real OS callback would follow. Tests run without real hardware and without COM registration.

## Consequences

- Audio, webcam, and display changes propagate to the UI within ~200 ms without user interaction.
- Configured selections survive topology changes without dirty-ing the preset.
- The preview surface does not show stale frames after the source device is removed.
- The stable-display-identity limitation (see Known Limitations below) defers EDID-based persistence.

## Deferred / Known Limitations

- **Stable display identity** uses `QScreen::name()` = GDI device name (`\\.\DISPLAYn`). GDI names are reassigned on topology change; they are not hardware-stable across reboots or monitor swap. EDID/DISPLAYCONFIG-based stable identity is deferred to a future revision.
- **No hot-swap during recording**: device changes are detected and the UI updates, but the active recording pipeline is not retargetable while a session is in progress. An honest degradation state is shown.
- **Audio/webcam page handlers re-enumerate** on the snapshot trigger rather than consuming the snapshot directly; this avoids introducing a new snapshot-forward path through MainWindow at MVP complexity.
