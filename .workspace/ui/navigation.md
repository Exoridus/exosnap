# UI Navigation

## MVP pages

1. Record
2. Video
3. Audio
4. Output
5. Hotkeys
6. Diagnostics
7. Logs
8. Advanced

## Operational Chrome

- Custom frameless operational titlebar, height `56px`.
- Titlebar slots (left to right):
  1. Brand (`EXO·SNAP`)
  2. Page context (`01 · RECORD`)
  3. State chip (`READY` / `BLOCKED` / `SUB-OPT` / `●● REC HH:MM:SS`)
  4. Capture info text
  5. Runtime metrics
  6. Window controls
- Titlebar bottom border:
  - Idle: hairline
  - Recording: amber
- Sidebar no longer has a separate brand header block. Navigation item `01` is the first visible sidebar element.

## Theme

- Default app theme: Dark
- Use native WinUI theme resources
- Do not turn dark mode into a custom visual-design project in MVP

## Page roles

### Record
Operational home. Shows source preview, readiness, recording controls, and live metrics.

### Video
Video configuration:
- codec
- output frame rate
- resolution
- cursor
- quality
- advanced encoder options

### Audio
Source ordering, enabling, device selection, `Merge with above`, resulting tracks, meters.

### Output
Container, destination, naming, storage estimate, container-specific info.

### Hotkeys
Global action bindings with set/unset capture flow.

### Diagnostics
Readiness checks, selftest, blockers, expandable explanations.

### Logs
Sessions, errors, checks, fixes, markers, split history.

### Advanced
Explicit expert/developer settings only.

## Navigation behavior

- App opens on `Record`.
- Starting recording from a hotkey while the window is visible activates `Record`.
- Starting recording while minimized does not restore or focus the app.
- Navigation remains available during recording, but `Record` is the preferred live-monitoring page.
