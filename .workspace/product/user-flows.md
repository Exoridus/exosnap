# User Flows

## 1. First launch

1. App opens in dark mode.
2. `Record` page is active.
3. App shows:
   - current default profile
   - empty or default source state
   - readiness summary
4. User selects an application, monitor, or desktop source.
5. Live preview appears.
6. Audio meters show configured source activity.
7. If readiness is green or green-with-notices, `Start Recording` is enabled.
8. If blockers exist, the app points to them and keeps `Start Recording` disabled.

## 2. Standard recording flow

1. User selects source.
2. User confirms preview and audio bars.
3. User presses `Start Recording` or hotkey.
4. App enters recording state.
5. `Record` page displays:
   - `REC` duration
   - current file size
   - preview
   - technical live metrics
   - audio bars
6. User may:
   - add marker
   - split recording
   - stop recording
7. On stop:
   - session is finalized
   - log entry is created
   - output path is shown

## 3. Hotkey-start flow

1. User triggers `Start/Stop Recording` hotkey.
2. If app is visible:
   - switch to `Record` page
   - enter recording state
3. If app is minimized:
   - do not restore or steal focus
   - begin recording if readiness allows
4. If blockers exist:
   - do not start
   - log the failed attempt
   - surface reason in app UI when available

## 4. Audio reconfiguration flow

1. User opens `Audio`.
2. User sees three reorderable source rows:
   - APP
   - MIC
   - SYS
3. User can:
   - enable/disable rows
   - drag rows
   - toggle `Merge with above` on any active non-top row
4. Below rows, the app shows `Resulting tracks`.
5. The preview updates immediately as the user changes order/merge state.

## 5. Container-switch flow

### MKV → MP4
1. User opens `Output`.
2. User changes container from MKV to MP4.
3. Audio codec selection is reconciled automatically to a valid MP4-compatible codec.
4. Opus is no longer shown as selectable.
5. Calm info text appears explaining lower crash resilience than MKV.

### MP4 → MKV
1. User changes container back to MKV.
2. The app restores the last valid MKV audio codec if remembered; otherwise it selects Opus.
3. MP4-specific info text disappears.

## 6. Selftest flow

1. User clicks `Run system & pipeline check`.
2. App runs checks in groups:
   - OS
   - GPU / encoder
   - display
   - audio
   - storage
   - pipeline
   - settings compatibility
3. Result summary shows:
   - passes
   - notices
   - blockers
4. Each item is expandable.
5. Blockers disable recording start.
6. Session/log entry records the selftest result.

## 7. Hotkey binding flow

1. User opens `Hotkeys`.
2. Each action has:
   - current binding
   - `Set`
   - `Unset`
3. Clicking `Set` enters `Enter hotkey now…` mode.
4. Accepted chord:
   - optional Ctrl
   - optional Shift
   - optional Alt
   - one keyboard key
5. `Esc` cancels.
6. Conflicts are rejected or explained.
7. `Unset` removes the binding.
