# Hotkeys View

## Actions

- Start/Stop Recording
- Pause/Resume Recording
- Split Active Recording
- Mute/Unmute Microphone
- Mute/Unmute App Audio
- Mute/Unmute System Audio
- Add Marker

## Suggested layout

```text
Hotkeys
────────────────────────

Start/Stop Recording      Alt+F9      [Set] [Unset]
Pause/Resume Recording    Unset       [Set] [Unset]
Split Active Recording    Unset       [Set] [Unset]
Mute/Unmute Microphone    Unset       [Set] [Unset]
Mute/Unmute App Audio     Unset       [Set] [Unset]
Mute/Unmute System Audio  Unset       [Set] [Unset]
Add Marker                Unset       [Set] [Unset]
```

## Capture mode

Clicking `Set` changes the row into:

```text
Enter hotkey now… [Cancel]
```

Accepted chord:
- optional Ctrl
- optional Shift
- optional Alt
- exactly one keyboard key

Behavior:
- `Esc` cancels capture
- duplicate internal assignment must be rejected or explained
- unavailable global combinations must be rejected or explained
- `Unset` clears the binding

## Default bindings

MVP recommendation:
- `Start/Stop Recording` = `Alt + F9`
- all others unset

## Recording-view interaction

When a hotkey starts recording:
- if the app is visible, activate the Record view
- if minimized, do not restore or steal focus
