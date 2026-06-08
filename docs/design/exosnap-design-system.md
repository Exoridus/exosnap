# ExoSnap Design System (R3 Refresh from Latest v2 Prototype)

Last refreshed: 2026-06-01

## Scope and source of truth
This design-system contract is refreshed from:

- `.workspace/design/exosnap-v2-prototype/ExoSnap Design System.html`
- `.workspace/design/exosnap-v2-prototype/ExoSnap Handoff.html`
- `.workspace/design/exosnap-v2-prototype/styles.css`
- `.workspace/design/exosnap-v2-prototype/v2.css`
- `.workspace/design/exosnap-v2-prototype/ds-system.css`
- `.workspace/design/exosnap-v2-prototype/ui.jsx`
- `.workspace/design/exosnap-v2-prototype/modal.jsx`
- `.workspace/design/exosnap-v2-prototype/record.jsx`
- `.workspace/design/exosnap-v2-prototype/settings.jsx`
- `.workspace/design/exosnap-v2-prototype/pages.jsx`

This document is implementation-facing for Qt Widgets and QSS.

## 1. Color tokens
Use resolved hex and rgba tokens in QSS.

| Token | Value | Intended use |
|---|---|---|
| `app-bg` | `#0E0C0A` | Main app background |
| `sidebar-bg` | `#141210` | Sidebar and top/elevated shells |
| `panel-card-bg` | `#191714` | Standard card/panel surfaces |
| `elevated-surface` | `#211E1B` | Inputs and inner wells |
| `surface-hover` | `#272420` | Hover and active neutral fills |
| `border-soft` | `#272421` | Subtle separators and card outlines |
| `border` | `#322E2B` | Control borders |
| `border-strong` | `#4B4741` | Hover-border lift and stronger rails |
| `text-primary` | `#EDE9E2` | Primary text and key values |
| `text-dim` | `#AEA8A0` | Body and secondary labels |
| `text-faint` | `#7A756E` | Metadata and helper text |
| `text-ghost` | `#54504B` | Quiet technical labels/disabled text |
| `amber` | `#E9B361` | Primary accent and selection |
| `green` | `#78DA95` | Ready/success/healthy state |
| `red` | `#F05B54` | Recording/stop/error state |
| `warning` | `#E9B361` | Warning tone (same hue family as amber) |
| `on-accent-ink` | `#1B1407` | Text/icon on amber or green fills |
| `amber-tint` | `rgba(233,179,97,0.14)` | Selected fills and focus-tint backgrounds |
| `amber-line` | `rgba(233,179,97,0.42)` | Selected borders/focus lines |
| `green-tint` | `rgba(120,218,149,0.13)` | Ready/success surface tint |
| `green-line` | `rgba(120,218,149,0.42)` | Ready/success border line |
| `red-tint` | `rgba(240,91,84,0.15)` | Recording/error surface tint |
| `red-line` | `rgba(240,91,84,0.48)` | Recording/error border line |
| `selected-line` | `#E9B361` (+ amber line/tint) | Selected-card ring and line |
| `selected-tint` | `rgba(233,179,97,0.14)` | Selected-card background tint |
| `focus-ring` | `0 0 0 3px rgba(233,179,97,0.14)` | Focus halo for combo/select-like controls |
| `disabled-alpha-button` | `0.40` | Disabled button opacity contract |
| `disabled-alpha-input` | `0.45` | Disabled combo/input opacity contract |

## 2. Typography roles
| Role | Family | Target size/weight | Notes |
|---|---|---|---|
| Page title | Hanken Grotesk | `27px / 600` | `-0.01em` tracking |
| Section title | Hanken Grotesk | `15px / 600` | Used in card headers and section labels |
| Card title | Hanken Grotesk | `13-15px / 600` | Short, dense titles |
| Body | Hanken Grotesk | `14px / 400` | Normal copy and descriptions |
| Metadata | JetBrains Mono | `10.5-12px` | Dim/faint technical metadata |
| Mono labels | JetBrains Mono | `9.5-11px` uppercase | Technical headings and tags |
| Keycaps | JetBrains Mono | `11px` | Key binding chips and shortcuts |
| Timer | JetBrains Mono | `46px / 500` | Record rail timer digits |
| Log body | JetBrains Mono | `12px / 1.7` | Dense but readable logs |
| Warning/help copy | Hanken Grotesk | `12.5px / 1.5` | Guidance and caution lines |

## 3. Spacing, sizing, and radii
### Base spacing scale
`4, 7, 8, 12, 14, 16, 20, 24, 36`

### Layout sizing
- Page padding (desktop): `28px 36px`
- Page padding (compact): `22px 22px`
- Default page max-width: `1200px`
- Narrow page max-width: `820px`
- Wide page max-width: `1320px`
- Sidebar width: `244px` (compact icon rail at narrow breakpoint)

### Component sizing
- Card padding: `20px`
- Card gaps (grid rhythm): `16-20px`
- Control row target height: `38-40px`
- Chip/pill height: `~24px`
- Source card grid min width: `180px` (drops to `150px` at narrow)
- Source thumbnail ratio: `16:10`
- Record rail width: `360px`
- Preview ratio: `16:9`, min-height `300px`
- Modal width/height envelope: `min(880px, 100%)`, `max-height min(660px, 90vh)`
- Modal paddings: `16-18px` regions (head/body/footer)

### Radii
- Card radius: `12px`
- Control radius: `9px`
- Pill radius: `7px`
- Modal outer radius: `14px`

## 4. Component rules
- Primary button: amber fill, amber border, `#1B1407` text, one primary intent per view.
- Secondary button: neutral surface fill (`surface-2`) with border hover lift.
- Danger/stop button: red fill/border, white foreground.
- Disabled button: non-interactive and visibly disabled via reduced opacity.
- Utility/icon button: neutral ghost/secondary style, no accidental primary emphasis.
- Status chip: mono uppercase, tone by state (`green`, `amber`, `red`, `ghost`).
- Metadata chip: subdued mono styling for technical tags.
- Keycap chip: monospace keycap style with strong border contrast.
- Source chip: compact source identity with icon, title, metadata; lock style when not editable.
- Card: warm neutral surface, soft border, consistent padding.
- Selected card: amber border plus visible selected ring/check.
- Unavailable card: non-selectable, dimmed, explicit overlay reason.
- Warning card: amber-tinted surface with warning line/border.
- Result card: green-tinted success surface with output metadata.
- Readiness/status strip: single concise strip below cockpit body, clear go/no-go semantics.
- Log viewer surface: dedicated dark contained surface, monospace lines, fixed scroll region.
- Segmented tabs/picker tabs: use segmented/tab semantics, not top-level navigation tabs.
- Combobox/select: custom dark popup and caret behavior; avoid native OS mismatch.

## 5. Combobox and select spec
Contract for select-like controls:

- Caret is visible and right-aligned.
- Closed control uses dark surface and strong border contrast.
- Popup menu is dark (`bg-elev`) with rounded corners and internal padding.
- No default Windows blue selection styling.
- Item spacing supports readability (`~34px` row rhythm with internal padding).
- Hover and selected states are explicit and palette-consistent.
- Focus uses amber line plus tint ring.
- Disabled state is visibly muted and non-interactive.

Guidance:

- Use combobox/select when the choice is dense or technical.
- Use card groups when visual differentiation matters (for example Capture Quality direction).

## 6. Card group vs dropdown guidance
- Capture Quality should visually move toward selectable quality cards.
- Dropdown can remain as an internal/test seam temporarily.
- Cards must not fake unsupported custom controls.
- Selected card state uses amber ring/check, not only text color.
- `Custom` quality can appear as a direction card, but should be clearly marked if not live.

## 7. Layout width guidance
Do not apply one global centered max-width everywhere.

Page-specific behavior is required:

- Record should use meaningful width and preserve preview dominance.
- Logs should use wider content and a contained log panel.
- Settings may be capped but must keep two-column desktop rhythm where space allows.
- Diagnostics must keep enough width for stat tiles and readable details.
- Hotkeys can be narrower but should not feel like an awkward floating strip.

## 8. QSS vs C++ boundaries
QSS responsibilities:

- Tokens and palette usage.
- Borders, radii, typography presets, and static state colors.
- Basic control/chip/button visuals.

C++ responsibilities:

- Responsive layout switching by window width.
- Grid reflow and column changes.
- Source thumbnail lifecycle states and fallback transitions.
- Filtering and disclosure behavior in Source Picker.
- Dynamic record-state composition and rail/page state changes.

Boundary rule:

- Do not try to solve all layout dynamics in QSS alone.

## 9. Implementation priority checklist
1. Repair width and grid regressions first.
2. Repair Record preview dominance and right-rail state honesty.
3. Re-card downgraded components where direction already exists.
4. Then consider shell/sidebar/about polish changes.
5. Then continue feature work.
