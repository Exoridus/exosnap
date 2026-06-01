# ExoSnap UX Final Direction (R3 Refresh from Latest v2 Prototype)

Last refreshed: 2026-06-01

## Scope and source of truth
This UX contract is refreshed from the current local prototype and style sources:

- `.workspace/design/exosnap-v2-prototype/ExoSnap.html`
- `.workspace/design/exosnap-v2-prototype/ExoSnap Handoff.html`
- `.workspace/design/exosnap-v2-prototype/ExoSnap Design System.html`
- `.workspace/design/exosnap-v2-prototype/styles.css`
- `.workspace/design/exosnap-v2-prototype/v2.css`
- `.workspace/design/exosnap-v2-prototype/ds-system.css`
- `.workspace/design/exosnap-v2-prototype/app.jsx`
- `.workspace/design/exosnap-v2-prototype/record.jsx`
- `.workspace/design/exosnap-v2-prototype/modal.jsx`
- `.workspace/design/exosnap-v2-prototype/settings.jsx`
- `.workspace/design/exosnap-v2-prototype/pages.jsx`
- `.workspace/design/exosnap-v2-prototype/ui.jsx`
- `.workspace/design/exosnap-v2-prototype/icons.jsx`

This is a product and UX contract for Qt implementation slices. It does not change backend capabilities.

## 1. Current target IA
Primary navigation target:

- Record
- Settings
- Hotkeys
- Diagnostics
- Logs

Advanced and Webcam are secondary/detail surfaces reachable from Settings, not primary sidebar destinations.

Clarifications:

- Advanced remains available internally as expert controls.
- Webcam setup remains available internally.
- Real webcam setup preview must not be removed.
- No fake PiP or fake overlay controls.
- No new top-level destinations are added in this contract.

## 2. Record cockpit contract
Record is the operational cockpit. Target composition:

- Preview-dominant layout.
- Source chip above/near preview.
- `Change source` is available when editable.
- `Source locked` appears while recording or paused.
- Preview keeps `16:9` and fills available frame as much as technically possible.
- Right rail is fixed around `360px` on wide screens.
- Right rail is state-specific.
- Readiness/result/status strip is below the main cockpit body.
- No fake runtime controls.

State contract:

| State | Primary action | Secondary action | Timer behavior | Source behavior | Readiness/status behavior | Result/output behavior | Runtime-control honesty |
|---|---|---|---|---|---|---|---|
| READY | `Start Recording` | `Change source`, optional `Diagnostics` route | Idle `00:00:00` | Editable | Clear ready strip (`go/no-go` visible) | Preset/source/output context visible, no result card | Show only controls that are really wired; planned controls must be disabled/labeled planned |
| RECORDING | `Stop` | `Pause` | Running, prominent mono timer | Locked | Readiness collapses to concise live summary (`target locked`) | Live stats shown (size, dropped, encoder load, output fps) | Live toggles may claim instant apply only when true end-to-end |
| PAUSED | `Resume` | `Stop` | Frozen timer | Locked | Paused status is explicit and concise | Output file is held open; no fake progression | If a control is inert in pause, render it inert/disabled clearly |
| DONE / RESULT | `Record again` | `Open folder`, `Play` | Timer replaced by result summary | Unlocked/editable | Ready strip is replaced by result state | Saved file card shows filename, duration, size, format | No fake post-processing controls |
| BLOCKED / ERROR | Start remains disabled (or routes via blocker path) | `Open Diagnostics` | Timer hidden/demoted | Editable | Red blocker summary with concrete causes | No success/result claims | Do not show active controls for blocked features |

## 3. Settings contract
Settings is a wide, product-like composition, not a timid centered form column.

Layout contract:

- Two-column card rhythm on desktop.
- One-column only when width requires it.
- Preserve meaningful page width so cards breathe and labels do not clip.

Content contract:

- Preset and Format card.
- Capture Quality as product-card direction (named quality options), not merely a dropdown target.
- Capture Behavior is separate from Quality.
- Audio Sources remain readable and source-specific.
- Webcam Setup module remains in Settings.
- Output module remains in Settings.
- Advanced/Expert Settings access remains in Settings (collapsed expert section is acceptable).

Implementation note:

- Capture Quality card UI is the visual target.
- Internal/objectName and test seams may continue to use existing controls (for example `videoQualityCombo`) until a dedicated implementation slice lands.

## 4. Source Picker contract
Source Picker remains modal-first with tabs:

- Screens
- Windows
- Region

Rules:

- Visual grid cards are the primary selection surface.
- Thumbnails are the primary recognition aid.
- Thumbnail states must be explicit: normal, loading, failed fallback.
- Selected state should be ring/check, not a redundant `Selected` badge when already obvious.
- Default Windows grid shows only useful, capturable user windows.
- System/helper/tool/overlay/noise windows are hidden from the default view.
- Minimized/protected/too-small windows are hidden behind `Show unavailable` disclosure.
- `Show unavailable` reveals non-selectable unavailable cards with reason.
- Too-small behavior is explicit and non-selectable.
- No noisy EnumWindows dump.
- No redundant `Window` badges on normal cards.
- No horizontal overflow in modal body, tabs, or footer actions.

## 5. Diagnostics contract
Diagnostics is a single-scroll troubleshooting page.

Contract:

- Action-first readiness header (clear status and next actions).
- Stat tiles for blockers, notices/recommendations, passes.
- Issue/recommendation cards are primary reading order.
- Capabilities/details are demoted (collapsed or secondary).
- Self-test actions are clear and explicit.
- Logs redirect remains obvious.
- Pipeline timing appears only when based on real instrumentation.
- If pipeline timing is not real in current slice, keep it planned/demoted and never fake precision.

## 6. Logs contract
Logs stays a raw log viewer, but in a contained product surface.

Contract:

- Raw log lines remain available.
- Log body is contained in a dedicated log surface.
- Strong monospace body.
- Readable line-height.
- Refresh and Open folder actions remain present.
- Optional filters are acceptable only if implemented.
- No raw text dump directly on page background.

## 7. Hotkeys contract
Hotkeys is split by capability truth:

- Active section and Planned/Unavailable section.
- `Start/Stop` and `Pause/Resume` are active.
- `Split` and `Mute`-like actions stay unavailable unless wired.
- No fake `Set/Unset` controls for unavailable actions.
- Keycap styling remains monospace and distinct from body text.

## 8. Advanced contract
Advanced is an expert/detail surface reachable from Settings.

Contract:

- Not a primary nav target.
- Read-only current/baseline summaries are acceptable.
- Experimental or developer controls are separated from regular configuration.
- Dangerous reset remains visually separated.

## 9. Webcam contract
Webcam setup remains reachable from Settings.

Contract:

- Real setup preview is preserved.
- Device/missing-feed states are honest and explicit.
- No fake overlay/PiP/chroma claims.
- Record PiP placement integration is a later slice unless fully implemented.
- If placement is not fully wired, copy must not overclaim runtime behavior.

## 10. Runtime-control honesty
Truthfulness requirements across the app:

- Do not show controls as active if they are not wired.
- Do not claim `changes apply instantly` unless true for the current path.
- Status chips are acceptable for planned vs active signaling.
- Disabled/planned states must be visually and textually honest.
- Blocked/error states must route to action, not cosmetic status only.

## 11. Redline and current repair priorities
Highest-priority repair order for current UI polish and follow-up slices:

1. Undo timid app-wide center/narrow layout.
2. Restore page-specific width and grid behavior.
3. Restore Settings/Advanced meaningful width.
4. Make Record preview fill and dominate.
5. Keep Logs as contained viewer.
6. Reinforce Diagnostics stat tiles and action-first hierarchy.
7. Push Capture Quality toward card direction.
8. Reduce redundant badges and labels.

Clarifications for this phase:

- Do not add collapsed sidebar yet.
- Do not move ExoSnap branding to footer yet.
- About popup is optional/later.
- Shell/sidebar rework remains separate from immediate layout repair.
