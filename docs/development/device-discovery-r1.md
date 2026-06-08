# Device Discovery R1 — Architecture and Acceptance Guide

## Overview

DEVICE-DISCOVERY-R1 adds three reactive QObject notifier services that detect hardware changes at runtime and propagate typed snapshots to the UI without requiring app restart or user-initiated refresh. The feature covers audio endpoints (WASAPI), webcams (Win32 WM_DEVICECHANGE), and display topology (Qt screen signals).

---

## Architecture

### The three notifiers

| Service | File | Event source | Snapshot type |
|---|---|---|---|
| `AudioDeviceNotifier` | `services/AudioDeviceNotifier.{h,cpp}` | `IMMNotificationClient` (MTA COM thread → `QueuedConnection`) | `AudioDeviceSnapshot` |
| `WebcamDeviceNotifier` | `services/WebcamDeviceNotifier.{h,cpp}` | `WM_DEVICECHANGE` on message-only HWND (WNDPROC → `QueuedConnection`) | `WebcamDeviceSnapshot` |
| `DisplayDeviceNotifier` | `services/DisplayDeviceNotifier.{h,cpp}` | Qt `QGuiApplication` screen signals (already main thread) | `DisplaySnapshot` |

All three live in `MainWindow` as direct member fields (`audio_notifier_`, `webcam_notifier_`, `display_notifier_`). They are started after the capability probe completes and stopped first in `~MainWindow`.

### Threading and debounce

```
Native callback / Qt screen signal (main thread or MTA thread)
  -> QMetaObject::invokeMethod(QueuedConnection)  [marshals to main thread]
  -> scheduleRefresh(reason)                       [main thread; restarts 200 ms timer]
  -> debounce_timer_ timeout                       [main thread]
  -> refreshNow(reason)                            [main thread: enumerate, sort, compare, emit]
```

The 200 ms debounce window coalesces rapid OS bursts (e.g. multi-monitor mode-sets) into a single UI update. It can be set to 0 in tests via `setDebounceIntervalMsForTest(0)`.

### Selection policy

When a snapshot arrives at a UI page (`ConfigPage::onAudioDevicesChanged`, `WebcamPage::onWebcamDevicesChanged`, `RecordPage::onDisplaysChanged`):

1. Read the current configured stable ID (may be `nullopt` for semantic Default mic).
2. If the ID is present in the new snapshot → restore selection to that entry.
3. If the ID is absent → show `(unavailable)` placeholder; **do not change the stored ID**; **do not emit any settings-changed signal**.
4. Semantic Default (`nullopt`) stays at combo index 0 regardless of which physical device became the Windows default.

This ensures availability changes never dirty the recording preset.

### Persistence rules

The reactive handlers are explicitly non-persistent:

- `ConfigPage::onAudioDevicesChanged` → does NOT emit `audioSettingsChanged`; does NOT call `RecordingPresetStore::Save()`.
- `WebcamPage::onWebcamDevicesChanged` → does NOT emit `settingsChanged`.
- `RecordPage::onDisplaysChanged` → does NOT emit `recordingConfigChanged`.

Only explicit user gestures (combo selection, Enable toggle, explicit Rescan → then user confirms) produce emissions that propagate to the preset system.

---

## Visual Test Harness Scenarios (DEVICE-DISCOVERY-R1)

Ten new scenarios in `VisualScenario.cpp` (`kDeviceDiscoveryScenarios`) cover the key device states. All are non-persistent: `preset_dirty = false` in every scenario struct; the harness hook `applyVisualDeviceDiscoveryScenario()` (guarded by `EXOSNAP_ENABLE_VISUAL_TEST_HARNESS`) calls only non-persistent page methods.

| Scenario id | Page | State |
|---|---|---|
| `settings-audio-devices-normal` | Settings | Mic present & selected |
| `settings-audio-selected-missing` | Settings | Configured mic absent (placeholder shown, id preserved) |
| `settings-audio-default-changed` | Settings | Semantic Default mic, reason=DefaultChanged |
| `settings-webcam-devices-normal` | Settings | Webcam present & active |
| `settings-webcam-selected-missing` | Settings | Selected webcam absent (Unavailable state, no stale frame) |
| `settings-webcam-reconnected` | Settings | Webcam available again after removal |
| `source-displays-normal` | Source Picker | 2 displays, one selected & available |
| `source-display-selected-missing` | Source Picker | Selected display absent, unresolved |
| `record-display-unavailable` | Record | Configured display gone, no stale preview |
| `record-region-monitor-missing` | Record | Region invalidated (hosting monitor gone) |

The new manifest `device_discovery` JSON object is emitted for every scenario with these fields:
`audio_input_count`, `audio_output_count`, `selected_mic_stable_id`, `selected_mic_available`, `selected_output_semantic_default`, `webcam_count`, `selected_webcam_stable_id`, `selected_webcam_available`, `display_count`, `selected_display_stable_id`, `selected_display_available`, `current_target_resolved`, `rescan_enabled`, `last_discovery_reason`.

Sentinel values (`-1` for counts, empty string for IDs) indicate "not applicable" for scenarios that do not exercise that device category.

---

## Deferred Manual Acceptance Checklist

The following scenarios require a real machine with physical hardware and should be executed before the first production release of DEVICE-DISCOVERY-R1.

### Audio

- [ ] Connect a USB microphone → mic appears in Settings Audio combo within 1 second.
- [ ] Disconnect connected USB microphone → placeholder `(unavailable)` shown; stored id unchanged; no preset dirty.
- [ ] Change Windows default microphone (Control Panel or tray) → explicit selection NOT replaced; semantic Default stays at index 0.
- [ ] Configure semantic Default (index 0) then change Windows default → still shows index 0, no dirty.
- [ ] Disconnect mic during Ready state → Rescan button restores it; no recording started.
- [ ] Verify VU meter stops after disconnecting mic; resumes after reconnecting and Rescan.

### Webcam

- [ ] Connect a USB webcam → camera appears in Settings/Webcam within 1 second; live preview starts.
- [ ] Disconnect connected webcam → Settings/Webcam shows Unavailable state; preview clears (no stale frame).
- [ ] Reconnect same webcam → camera reappears; if it was previously selected, selection is restored; preview restarts.
- [ ] Disconnect webcam during Webcam page open → state updates without crash.
- [ ] Disconnect webcam during Record page Ready → PiP area clears or shows placeholder; no crash.

### Displays

- [ ] Disable secondary monitor (Windows display settings) → display removed from Source Picker within debounce window.
- [ ] Re-enable disabled secondary monitor → display reappears in Source Picker.
- [ ] Change resolution of an active display → DisplaySnapshot updates; existing selection preserved.
- [ ] Change orientation (portrait/landscape) → snapshot updates; no crash.
- [ ] Mixed-DPI setup: add/remove a high-DPI monitor → `device_pixel_ratio` field updates correctly.
- [ ] Select a display in Source Picker then disable it → selected display becomes unresolved (no silent fallback to another monitor).
- [ ] Select Region on a secondary monitor then disable that monitor → Region state shows Invalid; no stale preview.

### Recording

- [ ] Start a recording → disconnect the configured display immediately → recording continues (engine is not affected; only the UI preview stops).
- [ ] The Recording page shows an honest degraded state (no stale preview frame) when the configured display is disconnected.
- [ ] After recording stops → reconnect display → Rescan restores the source; next recording starts normally.
- [ ] Disconnect configured mic during recording → recording continues with the remaining audio tracks; no crash.
- [ ] Reconnect mic after recording completes → mic appears in combo on next Ready state.

---

## Known Limitations

### Stable display identity

Display IDs use `QScreen::name()` which maps to the GDI device name on Windows (e.g. `\\.\DISPLAY1`). GDI device names are **not hardware-stable**: they can be reassigned when display topology changes (disconnect/reconnect, reboot, GPU mode-set). A configuration that selects `\\.\DISPLAY2` may silently point to a different physical monitor after a reboot or topology change.

A hardware-stable identity based on EDID serial number or `DISPLAYCONFIG_TARGET_DEVICE_NAME` (SetupAPI / `QueryDisplayConfig`) is deferred. Users who rely on a specific secondary monitor for region capture may need to re-select it after a topology change.

### No active hot-swap during recording

Device changes during an active recording session are detected and the UI updates, but the recording pipeline is not retargetable while a session is in progress. The pipeline was locked to the capture device at start-recording time. If the configured device is removed mid-recording:

- Display: the preview surface stops updating; the file continues to encode the last available frames until the session ends naturally or the encoder stalls.
- Mic/webcam: audio/webcam tracks may be silent or absent from the remainder of the file.

An explicit stop-and-restart is required to switch to a different device. This matches the honest-error design principle: no silent fallback, no silent track drop.

### Audio and webcam page handlers re-enumerate on snapshot trigger

`ConfigPage::onAudioDevicesChanged` and `WebcamPage::onWebcamDevicesChanged` re-run their full combo-population logic when a snapshot arrives (rather than consuming the snapshot vector directly). This keeps the snapshot-forwarding path through MainWindow minimal at MVP complexity, but means the page performs a lightweight enumeration call in addition to processing the snapshot. At the scale of USB device events (< 1 per second typical) this is not a performance concern.
