# ExoSnap UX Final Direction (Refreshed from Claude v2 Prototype)

## Scope and source of truth
This document is regenerated from:

- `.workspace/design/exosnap-v2-prototype/ExoSnap.html`
- `.workspace/design/exosnap-v2-prototype/ExoSnap Handoff.html`
- `.workspace/design/exosnap-v2-prototype/record.jsx`
- `.workspace/design/exosnap-v2-prototype/modal.jsx`
- `.workspace/design/exosnap-v2-prototype/settings.jsx`
- `.workspace/design/exosnap-v2-prototype/ui.jsx`
- `.workspace/design/exosnap-v2-prototype/styles.css`
- `.workspace/design/exosnap-v2-prototype/v2.css`
- `.workspace/design/exosnap-v2-prototype/ds-system.css`
- `.workspace/screenshots/exosnap-v2-prototype/record_ready.png`
- `.workspace/screenshots/exosnap-v2-prototype/record_recording.png`
- `.workspace/screenshots/exosnap-v2-prototype/record_paused.png`
- `.workspace/screenshots/exosnap-v2-prototype/record_done.png`
- `.workspace/screenshots/exosnap-v2-prototype/record_blocked.png`

Current implementation snapshot used for obvious gap summary:

- `.workspace/screenshots/claude-design-current/`

## 1. Product thesis
ExoSnap should feel like a confident recording flow in two clicks:

1. pick/confirm source
2. start recording

Record is the operational cockpit. Configuration moves to Settings. Troubleshooting moves to Diagnostics. Logs remain raw.

## 2. Target IA (v2)

| Order | Destination | Role |
|---|---|---|
| 01 | Record | Operate capture live |
| 02 | Settings | Configure format/quality/inputs/output |
| 03 | Hotkeys | Shortcut management |
| 04 | Diagnostics | Troubleshooting and health |
| 05 | Logs | Raw event stream |

Removed as top-level pages in target IA:

- Webcam page (setup folds into Settings; placement is in Record preview)
- Advanced page (collapsed section in Settings)

## 3. Page responsibility model

### Record
- Source chip + explicit `Change source`.
- Dominant live preview.
- Right rail with stateful transport/timer/status.
- Runtime input controls near preview (not buried in settings cards).
- Readiness and blocker messaging in-context.

### Settings
- Unified scroll of cards:
  - Preset & Format
  - Capture Quality
  - Capture Behavior
  - Audio Sources
  - Webcam Setup
  - Output
  - Advanced (collapsed)

### Hotkeys
- Active bindings plus clearly marked planned items (no fake-active controls).

### Diagnostics
- Actionable recommendations first.
- Pipeline/budget analysis as near-term extension.

### Logs
- Structured raw logs with filtering.

## 4. Record cockpit contract

### Layout stack (top -> bottom)
1. Source bar (`source chip`, `Change source`, preset summary)
2. Main row (`preview` + fixed-width right rail)
3. Runtime control bar (wrapping row)
4. Readiness/live summary strip

### Key placement rules
- Preview remains the dominant visual element.
- Source controls stay close to preview, not in global chrome only.
- Right rail is state-driven and concise (status, timer, actions, summary/stats).
- Readiness is compact and visible; not buried under unrelated setup content.

## 5. Record state behavior contract

| State | Primary action | Secondary action | Source edit | Runtime controls | Rail content |
|---|---|---|---|---|---|
| READY | `Start Recording` | none required | Enabled | Visible as setup controls | Ready pill, idle timer, start CTA, format/source summary |
| RECORDING | `Stop` | `Pause` | Locked | Visible live | REC status, prominent timer, live file/drop/encoder/fps stats |
| PAUSED | `Resume` | `Stop` | Locked | Visible | Paused status, frozen timer, resume/stop |
| DONE / RESULT | `Record Again` | `Open folder` / `Play` | Enabled | Hidden | Saved-result card with filename, duration, size, format |
| BLOCKED / ERROR | Disabled start (or routed) | `Open Diagnostics` | Enabled | Hidden until resolved | Blocker list with concrete causes |

State-specific details from prototype files:

- Source lock is explicit in recording/paused (`locked` chip treatment).
- Ready state shows a green readiness strip.
- Live states collapse readiness to a concise capture summary (`target locked`).
- Blocked rail includes concrete blockers and diagnostics route.

## 6. Source picker contract (modal)
Source selection is modal-first (from Record `Change source`), not inline card clutter on Record.

### Tabs
- `Screens`
- `Windows`
- `Region`

### Screens
- Visual monitor cards with primary marker and monitor metadata.
- Click select, double-click confirm.

### Windows
- Default grid is capturable windows first.
- Search filter in toolbar plus refresh action.
- Thumbnail states: normal, loading shimmer, failed icon fallback.
- Non-capturable windows are collapsed behind `Show unavailable (N)`.
- Unavailable/minimized/too-small cards are visible when expanded but not selectable.

### Region
- Region pane with selection canvas.
- `W/H/X/Y` readout blocks.
- `Snap to 16:9`.
- Recent region chips.

### Footer and exit behavior
- Footer always shows selected summary.
- `Cancel` and `Use this source` actions.
- `Esc` and backdrop-click close.

## 7. Runtime-control honesty rules
Do not present controls as active unless behavior exists end-to-end.

- If a runtime toggle is not wired, render it disabled with explicit explanatory copy.
- Planned features must be visually differentiated (`near-term`/`post-MVP`) and not styled as live controls.
- Keep source picker “availability truth” honest:
  - capturable now in default grid
  - unavailable states behind disclosure with reasons

## 8. Presets and Settings model

### Presets
- Preset is a saved configuration bundle, not a capability mode.
- Built-ins are read-only defaults (e.g. `AV1 Opus MKV`, `H.264 AAC MP4`, `VP9 Opus WebM`).
- Editing fields diverges into `Custom (unsaved)`.
- Save and reset actions are explicit and local to Preset & Format.

### Capture Quality
- Named tiers are first-class (`Small`, `Balanced`, `High Quality`, `Visually Lossless`).
- `Custom` exists as near-term panel shell; not falsely editable before wiring.
- FPS/CFR/cursor remain under Capture Behavior, not quality.

### Advanced
- Collapsed expert section in Settings (backend override/logging/etc).
- Avoid returning to separate Advanced page in target direction.

## 9. Responsive rules

| Window width | Target behavior |
|---|---|
| >= 1920 | Full cockpit: preview + fixed right rail |
| <= 1180 | Region pane stacks; cockpit can stack for readability |
| <= 1000 | Runtime bar wraps; source grid min width shrinks; compact sidebar behavior |

Additional constraints:

- Source picker remains bounded (`min(880px, 100%)` and `min(660px, 90vh)`) with internal scroll.
- Preview keeps `16:9` and remains visually prioritized.

## 10. Current Qt implementation gaps (obvious from screenshot set)
From `.workspace/screenshots/claude-design-current/`:

1. IA still includes `Advanced` and `Webcam` as top-level destinations; target IA is 5 destinations.
2. Record source treatment is still broad row-style, not the tighter chip-first cockpit treatment shown in v2 prototype states.
3. Readiness in Record remains a detailed multi-row panel in steady view; target state model uses a compact readiness strip and live-state collapse.
4. Runtime controls are not yet expressed as the single v2 runtime chip bar directly under preview across ready/recording/paused.
5. Top-right chrome still shows generic CPU/GPU/RAM telemetry in current captures; target chrome prioritizes recording-context metrics in live states.
6. Separate Webcam page remains present in current IA; target folds setup into Settings and uses Record preview for placement behavior.

These gaps are directional and visual; they do not imply backend regressions.

## 11. Implementation guardrails for next slices
- Do not regress current capture/encode/mux/audio behavior while porting UX contracts.
- Keep source picker and runtime controls behaviorally honest at every step.
- Prefer state-driven Record composition over ad-hoc conditional widgets.
- Keep Diagnostics routing explicit for blocked/error states.
