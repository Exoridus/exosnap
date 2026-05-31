# ExoSnap UX Final Direction (v2)

## Core thesis
ExoSnap should feel like "confident recording in two clicks."

- `Record` is the operational cockpit.
- Configuration belongs outside the cockpit.
- Troubleshooting belongs in Diagnostics, not hidden in capture controls.
- The UI stays technical but product-like: warm near-black, restrained amber, terminal-green status accents.

## Target IA
Target destination model from the v2 design direction:

1. Record
2. Settings
3. Hotkeys
4. Diagnostics
5. Logs

Implementation note:
- Current shipped MVP navigation remains `Record`, `Video`, `Audio`, `Output`, `Hotkeys`, `Diagnostics`, `Logs`, `Advanced`.
- This document defines final UX direction; migration to the 5-destination model is phased.

## Page responsibilities
`Record`
- Source context and source changes.
- Live capture preview.
- Recording transport and runtime state.
- High-confidence readiness signal before start.

`Settings`
- Unified configuration surface for format, quality, behavior, audio routing, webcam setup, output, and advanced controls.
- Vertical card flow, not tabbed settings.

`Hotkeys`
- Active shortcuts and clearly marked planned items.

`Diagnostics`
- Environment health and actionable recommendations.
- Pipeline-centric troubleshooting over static capability dumping.

`Logs`
- Raw structured technical event stream.

## Presets model
- Treat preset/profile as a saved configuration bundle, not a capability mode.
- Default working state is editable "custom unsaved."
- Built-ins are read-only defaults (for example AV1/Opus/MKV).
- Editing config fields diverges from saved preset and enters unsaved state.
- "Save preset" and "reset to active preset" should be explicit actions.

## Record cockpit model
- Keep preview as the dominant surface.
- Replace large source mode cards with:
  - compact selected source chip
  - explicit `Change source` action
- Preserve existing transport lifecycle:
  - Ready
  - Recording
  - Paused
  - Result (done/error)
  - Blocked
- Runtime controls stay in a single predictable strip while preserving real behavior.

## Source picker modal model
- Source selection moves to a dedicated modal/shell opened from Record.
- Sectioned navigation:
  - Screens
  - Windows
  - Region
- Selection and confirmation are explicit (`Use selected source`), with cancel support.
- Region section is allowed to be an honest shell in R1:
  - show current/last region
  - describe current supported flow
  - do not present fake draw/thumbnail behavior.
- During active recording, source changes are locked.

## Settings model
- Consolidated configuration cards in one scroll:
  - Preset & format
  - Capture quality
  - Capture behavior
  - Audio sources
  - Webcam setup
  - Output
  - Advanced (collapsed)
- No settings tabs.
- Clear ownership per card; avoid duplicated control semantics across pages.

## Capture quality model
- Named quality tiers map to concrete encoder intent.
- Keep quality orthogonal to capture behavior.
- Frame rate, CFR/VFR, and cursor policy belong to behavior, not quality.
- Custom quality remains staged and explicitly surfaced as future/near-term where not implemented.

## Webcam model
- Webcam setup belongs with configuration (`Settings` in target IA).
- Webcam placement/manipulation belongs in Record preview flow.
- In this phase:
  - keep real webcam setup preview and current webcam page behavior intact
  - no fake webcam controls
  - no PiP manipulation implementation unless explicitly scoped.

## Diagnostics model
- Start with actionable environment recommendations.
- Explain blockers clearly when start is prevented.
- Grow toward pipeline/budget diagnostics without replacing raw logs.

## Global chrome / transport model
- Recording state should remain legible across pages.
- Transport semantics stay state-aware and consistent with Record.
- Record start is blocked when diagnostics blockers exist.
- If a hotkey starts recording while app window is visible, activate Record view; if minimized, do not force restore.

## Responsive layout rules
- Desktop-first, resizable window; fluid behavior over fixed canvas.
- Keep preview readable first; allow secondary panels to stack below when narrow.
- Source picker should remain bounded and scroll internally for smaller windows.
- Runtime controls should wrap cleanly rather than overflow.

## Implementation roadmap
1. UX-DOC-R1 (this document)
   - Persist final direction and phased boundaries.
2. UX-FINAL-R1
   - Record cockpit source chip + source picker shell.
   - Reuse existing target enumeration/selection data flow.
   - Keep preview/recording backend unchanged.
3. UX-FINAL-R2
   - Deepen source picker fidelity (thumbnails/region affordances where safe).
4. UX-FINAL-R3
   - Settings consolidation steps and IA migration prep.
5. UX-FINAL-R4
   - Diagnostics v2 presentation and advanced troubleshooting view.

## Explicit non-goals (for this direction doc and R1 slice)
- No capture backend changes (DXGI/WGC/session/encoder/mux/audio timing).
- No fake live previews or fake source thumbnails presented as real capture.
- No overlay/HUD feature work.
- No runtime webcam PiP manipulation in this slice.
- No custom quality implementation in this slice.
- No removal of current Webcam page in this slice.
- No Advanced-to-Settings fold-in implementation in this slice.
- No broad IA navigation rewrite in this slice.
