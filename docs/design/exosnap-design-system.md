# ExoSnap Design System (Prototype v2 -> Qt/QSS Contract)

## Scope and source of truth
This document is regenerated from:

- `.workspace/design/exosnap-v2-prototype/ExoSnap Design System.html`
- `.workspace/design/exosnap-v2-prototype/ds-system.css`
- `.workspace/design/exosnap-v2-prototype/styles.css`
- `.workspace/design/exosnap-v2-prototype/v2.css`

It defines visual primitives and component contracts for Qt Widgets. It is intentionally implementation-oriented and does not redefine backend behavior.

## 1. Color tokens (resolved values for QSS)
QSS must use resolved hex/rgba values (not `oklch()` or `color-mix()`).

| Token | Value | Use |
|---|---|---|
| `--bg` | `#0E0C0A` | App canvas |
| `--bg-elev` | `#141210` | Sidebar, top bar, transport, modal shell |
| `--surface` | `#191714` | Default cards and panels |
| `--surface-2` | `#211E1B` | Inputs and secondary wells |
| `--surface-hover` | `#272420` | Interactive hover/active fill |
| `--border-soft` | `#272421` | Default separators and outlines |
| `--border` | `#322E2B` | Control outlines |
| `--border-strong` | `#4B4741` | Hover border lift |
| `--text` | `#EDE9E2` | Primary text |
| `--text-dim` | `#AEA8A0` | Secondary copy |
| `--text-faint` | `#7A756E` | Metadata/hints |
| `--text-ghost` | `#54504B` | Eyebrow/disabled labels |
| `--amber` | `#E9B361` | Selection/primary accent |
| `--green` | `#78DA95` | Ready/saved/healthy |
| `--red` | `#F05B54` | Recording/stop/error |
| on-accent ink | `#1B1407` | Text/icon on amber and green fills |

### Tint and line tokens (use rgba directly)

| Token | Value |
|---|---|
| `--amber-tint` | `rgba(233,179,97,0.14)` |
| `--amber-line` | `rgba(233,179,97,0.42)` |
| `--green-tint` | `rgba(120,218,149,0.13)` |
| `--green-line` | `rgba(120,218,149,0.42)` |
| `--red-tint` | `rgba(240,91,84,0.15)` |
| `--red-line` | `rgba(240,91,84,0.48)` |

## 2. Typography contract

| Role | Family | Size/weight | Tracking | Color |
|---|---|---|---|---|
| Page title | Hanken Grotesk | `27 / 600` | `-0.01em` | `--text` |
| Section/card title | Hanken Grotesk | `15 / 600` | `-0.005em` | `--text` |
| Body | Hanken Grotesk | `14 / 400` | `0` | `--text-dim` |
| Mono eyebrow label | JetBrains Mono | `10–11 / 500` | `0.12–0.18em`, uppercase | `--text-faint` / `--text-ghost` |
| Mono value | JetBrains Mono | `11–15 / 400` | `0.03em` | `--text` / `--text-dim` |
| Timer digits | JetBrains Mono | `46 / 500` | `0.01em` | State color |

Qt notes:

- Bundle fonts in app resources and load with `QFontDatabase::addApplicationFont`.
- Set global app font in code; do not rely on system install.
- Uppercase mono tracking must be set in code per-widget (`QFont::setLetterSpacing`).

## 3. Spacing, sizing, radii
Base spacing unit is `4px`.

### Spacing scale
`4`, `7`, `8`, `12`, `14`, `16`, `20`, `24`, `36`.

### Layout and control sizes

| Item | Target |
|---|---|
| Sidebar width | `244px` |
| Top bar / transport | `50px / 52px` |
| Page inner padding | `28px 36px` |
| Card padding | `20px` |
| Modal region padding | `16–18px` |
| Medium button height | `38px` |
| Small / large button heights | `32px / 46px` |
| Pill height | `24px` |
| Input / combobox height | `38px` |
| Source picker card min width | `180px` |
| Source picker thumbnail ratio | `16:10` |
| Record rail width | `360px` |
| Preview and webcam surface ratio | `16:9` (min height `300px`) |

### Radii

| Token | Value | Use |
|---|---|---|
| `--r-card` | `12px` | Cards, modals, preview surfaces |
| `--r-ctrl` | `9px` | Buttons, inputs, combobox |
| `--r-pill` | `7px` | Pills, chips, keycaps |

## 4. Global component state rules

- Selected state is always amber border plus amber ring (`box-shadow: 0 0 0 1px --amber`) and check badge where space exists.
- Hover is a border lift to `--border-strong`; hover must not look like selected.
- One primary accent action per view (amber or green according to semantic intent).
- Status semantics:
  - amber = chosen/primary
  - green = safe/ready/saved
  - red = recording/stop/error

## 5. Combobox spec (`QComboBox`)

| Property | Target |
|---|---|
| Closed height | `38px` |
| Closed padding | `0 38px 0 13px` |
| Closed bg/border | `#211E1B`, `1px #322E2B`, radius `9px` |
| Hover | border `#4B4741`, bg `#272420` |
| Focus/open | border `rgba(233,179,97,0.42)` plus amber tint ring |
| Disabled | `opacity: 0.45` |
| Caret | 12px chevron asset, right-aligned |
| Popup | `#141210` bg, `1px #322E2B`, radius `9px`, `5px` padding |
| Popup item | `34px` min-height, `0 11px` padding, radius `7px` |
| Selected popup item | amber tint bg, amber text, trailing check |

Qt/QSS implementation notes:

- Style `QComboBox QAbstractItemView` (not native popup only).
- Set custom list view (`QListView`) for predictable item styling.
- Ship caret icon as a resource; do not rely on native arrow.
- Soft focus glow may need a small helper effect in code.

## 6. Segmented controls vs picker tabs

### Picker mode tabs (`Screens / Windows / Region`)
- Underline-tab pattern, not pill segmentation.
- Active tab uses amber text/icon and amber bottom border.
- Count badges are mono and subdued.
- Tabs remain visible even when empty; empty state belongs in body.

### Form segmented controls (`2–3 choices`)
- Shared rounded frame (`9px`) with checkable button children.
- Checked segment uses raised/hover surface fill and brighter text.

## 7. Source picker card-state spec
From `modal.jsx` and v2/ds-system CSS:

### Screens tab
- Visual monitor cards with primary badge.
- Click selects, double-click confirms.

### Windows tab
- Default grid contains capturable windows only.
- Search filter applies live.
- Refresh action present in toolbar.
- Window cards include thumbnail, app glyph, title, and metadata.

### Region tab
- Region canvas + draggable rectangle semantics.
- Readout fields (`W/H/X/Y`), snap-to-16:9, recent region chips.

### Thumbnail and availability states

| State | Visual contract | Selectable |
|---|---|---|
| `ok` | Real thumbnail area | Yes |
| `loading` | Shimmer placeholder (`.sc-preview.is-loading`) | Yes |
| `failed` | Icon fallback (`.sc-preview.is-failed` + `.sc-fallback`) | Yes |
| `unavailable` | Dimmed card + overlay + badge | No |
| `minimized` | Overlay + amber minimized badge | No |
| `too-small` | Overlay + too-small badge | No |

### Disclosure and empty-state rules
- Unavailable windows are collapsed behind a `Show unavailable (N)` disclosure row.
- Expanded unavailable section is explicit and non-selectable.
- Filter misses show an explicit empty-state block with guidance text.
- Filtering/sorting/dedupe/noise suppression is a C++ model responsibility, not QSS logic.

## 8. Readiness, status, and result components

### Record readiness
- Full-width readiness strip under runtime controls in ready state.
- Green “go/no-go” summary in ready.
- Red blocker summary in blocked/error.
- Live recording states demote readiness into concise “target locked / live summary” messaging.

### Rail status cards by recording state
- READY: ready pill, zero timer, primary start action.
- RECORDING: red timer, stop/pause actions, live stats.
- PAUSED: amber timer, resume/stop actions.
- DONE: saved-file card with filename/metadata + `Open folder` and `Record again`.
- BLOCKED: blocker list, disabled start, explicit `Open diagnostics` path.

### Status chip style
- Pill pattern with mono uppercase labels.
- Optional live pulsing indicator for recording.

## 9. Qt implementation boundaries

### QSS-only
- Palette colors, borders, radii, spacing, static hover/pressed/disabled states.
- Button/pill/input/checkbox/radio/toggle visual primitives.

### QSS + object name/property wiring
- Picker tabs and segmented controls by role/state properties.
- Record rail tinting by dynamic `recState` property.
- Source-card states by dynamic availability properties.

### Requires C++/custom widgets
- Runtime audio meter animation.
- Source picker thumbnail pipeline and fallback transitions.
- Flow/wrap runtime bar behavior.
- Combobox popup view setup and optional focus glow helper.

### Explicitly unsupported in QSS
- CSS variables, `oklch()`, `color-mix()`.
- Full CSS box-shadow behavior on generic widgets.
- Native popup restyling without custom item view configuration.
