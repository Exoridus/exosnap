# ADR 0031: Settings input language, cogwheel popover, and roadmap-dummy pattern

## Status

Accepted — implemented in 0.6.0 (feat/0.6.0-audio-format-model, "Settings UX redesign 2")
following a detailed product review of the Expert-mode Settings surface. Builds on the
Settings Default/Expert split (ADR 0010 / [[project_settings_redesign_d6]]).

## Context

The Settings page mixed input chrome inconsistently: `QComboBox` dropdowns next to native
`QSpinBox`/`QDoubleSpinBox` of different widths, slider controls without affordances, disabled
inputs explained by inline text labels, checkbox rows carrying sentence-length descriptions, and a
flat list of mic-DSP rows (high-pass, gate, AGC, RNNoise) that crowded the Audio card. Roadmap
features were shown as greyed, disabled "coming in vX" placeholder rows. A product review asked for
a calmer, consistent surface and three reusable conventions.

## Decision

Three cross-cutting UI conventions, all token-driven (no colour literals) and applied across the
Settings cards:

### 1. Unified input language

- **Spinboxes are styled to match comboboxes** (same border, radius, padding, height) and every
  standard row input shares **one uniform width (160 px)** so the input column has a clean edge.
  Segmented controls (container/quality/rate-control/resolution) keep their segmented style.
- **Comboboxes become editable with type-to-filter autocomplete only where it adds value**
  (long/free-ish lists — e.g. the microphone-device picker, via `setEditable(true)` + a
  `Qt::MatchContains`, `NoInsert` `QCompleter`). Short fixed lists (2–4 entries) stay non-editable
  but share the same chrome.
- **CQ** (range 1–51, 1=best) stays a number input — too many steps for a slider/enum combo — and
  shows the nearest preset **tier inline** (`19 · High`, `24 · Balanced`, `30 · Small`, else
  `Custom`). A bitrate/file-size estimate from CQ is **deliberately not shown** — CQ holds quality
  constant, so output size is content-dependent and any number would mislead.
- **Sliders** use a new reusable **`ExoSlider`** (`app/ui/widgets/`): a `QSlider` subclass that
  paints a gradient-filled groove plus tick markers under the track, with the default/recommended
  value (e.g. 0 dB unity for mic gain) marked in the accent colour.
- **Disabled inputs explain themselves via tooltip + cursor, not inline text.** When a control is
  inert for the current codec/mode (e.g. sample rate locked to 48 kHz for Opus; bitrate/Opus params
  for lossless codecs; anything while recording), its reason goes into `setToolTip(...)` and its
  cursor is set to **`Qt::ForbiddenCursor` in C++** (Qt QSS has no `cursor` property — setting it
  there only spams the log). No inline explanatory label clutters the column, so the disabled input
  still right-aligns with the rest.
- **Checkbox rows carry no trailing prose.** A row is `Label (i) ……… [toggle]`; the description
  lives in the row's info-hint tooltip, never as text after the checkbox. Every settings row has an
  **info-i** (`InfoHintIcon`) with concise hint text (`app/models/SettingsHintText.h`).

### 2. Cogwheel popover — `SettingsPopoverRow`

A reusable `app/ui/widgets/SettingsPopoverRow` collapses a group of related sub-settings behind one
row: `[ label ] [ info-i ] …… [ optional primary control ] [ ⚙ ]`. The ⚙ (a flat
`QToolButton#settingsPopoverCog`) opens a frameless `Qt::Popup` panel
(`QWidget#settingsPopoverPanelInner`, token-styled card) holding the sub-controls; the caller
fills `popoverContentLayout()` by **reparenting the existing, already-wired sub-controls** (never
recreating them — objectNames and signal connections are preserved). An optional `setStatusText()`
hints what is active when there is no primary control.

Applied to: **Microphone post-processing** (HPF/gate/AGC/RNNoise + params behind a status-text row,
grouped with the other microphone controls), **Brickwall limiter** (the enable checkbox is the
primary control, the ceiling spinbox lives in the popover), and **Automatic split** (time + size
controls). Future settings groups should prefer this over a long flat list.

### 3. Roadmap-dummy rows: debug-real, release-hidden

Roadmap features that are not yet implemented (HEVC codec, video bit depth, HDR10, chroma
subsampling, encoder preset, auto-open) were previously greyed disabled placeholder rows.
Instead: they are **compiled out entirely in Release** (`#ifndef NDEBUG`), and in **Debug** they
render as **real, enabled dummy inputs** (`roadmapDummy_*` combos/toggles, non-wired) so the roadmap
can be designed and laid out against real controls. Release users never see an inert "coming soon"
row; designers in a debug build see the future shape. (Distinct from `PlaceholderRow`, which remains
for any intentionally-shown roadmap hint.)

## Consequences

- The Settings surface reads consistently: one input width, one input chrome family, info-i on every
  row, descriptions in tooltips, disabled controls self-explaining, and dense groups behind a ⚙.
- Two new reusable widgets (`SettingsPopoverRow`, `ExoSlider`) are available for all future settings.
- The release build no longer ships inert roadmap rows; debug builds carry design-only dummy inputs.
- The reorder was kept conservative — after the uniform-width + popover changes the cards are already
  grouped by input type, so only the microphone controls were regrouped (gain · channel mode ·
  post-processing together); no logical pair was split for the sake of type-sorting.

## Notes

- All accent usage stays token-driven (`${accent}` etc.) per [[reference_design_palette_canon]];
  no mint literals at call sites.
- A stylesheet-themed `QToolButton` with **no** explicit `#objectName` rule falls back to the
  style's default tool-button panel (a dark filled capsule that can also swallow its icon) — every
  themed tool button (the titlebar bell, the popover cog) needs an explicit flat QSS rule.
