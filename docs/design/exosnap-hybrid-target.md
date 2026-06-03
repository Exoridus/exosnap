# ExoSnap Hybrid v3 — Canonical Design Target

Source: `.workspace/design/exosnap-hybrid-v3/` (spec.jsx, hybrid-shared.jsx, hybrid-record.jsx, hybrid-pages.jsx, ExoSnap - Design Spec.html)

Last refreshed: 2026-06-03

---

## 1. Status

Hybrid v3 is the approved design target direction.

- Studio-based, preview-first app.
- Compact Cockpit-style bottom transport dock on the Record page.
- Telemetry limited to Diagnostics, Logs, recording-health metrics, and audio meters.
- Dark mode only.
- Qt Widgets + QSS is the recommended native build stack.

---

## 2. Brand / Identity

### App icon

A quiet aperture mark — concentric rings reducing to a single dot:

- Outer ring: accent color, 0.45 opacity, 1.5px stroke.
- Inner ring: accent (or coral while recording), 1.6px stroke.
- Center dot: filled accent (or coral while recording).

Recording state: inner ring and dot turn coral. Taskbar icon shows a live recording state. Clear space >= dot diameter on all sides. App icon ships as a rounded-square tile on dark.

### Wordmark

Lowercase Hanken Grotesk, weight 600, letter-spacing -0.4:

```
exosnap
```

- `exo`: neutral (ink color, `#F1F1EF` on dark).
- `snap`: accent color (mint-cyan default `#9BD9D2`).

Never recolor the wordmark except the live coral recording state.

### Accent system

Primary accent: mint-cyan `#9BD9D2` (Studio Mint).

Curated accent candidates (selectable by user):

| ID | Name | Base | Ink |
|---|---|---|---|
| mint | Studio Mint | `#9BD9D2` | `#08130F` |
| amber | Warm Amber | `#E2B473` | `#1A1206` |
| coral | Soft Coral | `#F2B3A2` | `#1E0D08` |
| azure | Electric Azure | `#63B5F2` | `#04121E` |
| violet | Violet | `#AB9DF2` | `#0E0A1E` |
| lime | Signal Lime | `#BFE36E` | `#121A06` |
| graphite | Graphite | `#CBCFCC` | `#0E1110` |

Semantic colors are always separate from the primary accent:

- Recording / Stop / Error: coral red `#E0786C`.
- Success / Healthy / Ready: green `#84CBA2`.
- Warning / Paused / Needs attention: amber `#E6C57C`.

---

## 3. Shell / Navigation

### Structure

- Frameless `QMainWindow` with a custom title-bar `QWidget`.
- Title bar hosts: app icon + wordmark (left), top navigation tabs (left of center), flexible spacer, status pill (right), min/max/close (far right).
- Navigation tabs: **Record**, **Settings**, **Hotkeys**, **Diagnostics**, **Logs**, **About**.
- Active tab shows a 2px accent underline, 600 weight; inactive tabs are muted, 500 weight.

### Title-bar / Status rule

| State | Pill | Details shown |
|---|---|---|
| Ready | Green dot + "Ready" | — |
| Recording | Red pulsing dot + "Recording" | Drop count, Frame time (ms), Encoder load (%) |
| Paused | Amber dot + "Paused" | — |
| Completed | Green dot + "Saved" | — |

Metrics shown during recording are recording-health only: dropped frames, frame time, encoder load. No generic CPU / GPU / RAM / Disk global stats.

---

## 4. Record Page Target

### Layout

- Preview-first: `QVideoWidget` / `QOpenGLWidget` fills available space, 16:9 ratio.
- Stable bottom transport dock (`QFrame`, 3-zone grid: left | duration | action).
- WYSIWYG webcam PiP overlay in Record only (draggable, corner-resizable, mirror support).
- Page padding: 20px around preview and dock.

### Transport dock — state layouts

**Ready:**
```
[System] [Mic] [Webcam] [App]    Duration     [3s ▾] [Record]
```

**Recording:**
```
[System] [Mic] [Webcam] [App]    Duration     [Pause] [Stop]
```

**Paused:**
```
[System] [Mic] [Webcam] [App]    Duration     [Resume] [Stop]
```

**Completed (Result):**
```
[Filename link] [Open Folder] [Size]    Duration     [Record again]
```

Rules:

- Left zone: circular icon toggles (System, Mic, Webcam, App). On = accent-dim fill + accent border; Off = transparent + muted icon.
- Center: monospace timer, 30px, 500 weight, tabular-nums, always in the same position.
- Right zone: countdown select (Off / 3s / 5s / 10s) + Record button (ready); Pause + Stop (recording); Resume + Stop (paused); Record again (completed).
- Completed: filename is a clickable link, underlined in accent; Open Folder is an icon button; file size shown in muted mono.
- Stable geometry: all zones maintain position across state transitions. No layout shift.

### Preview overlay elements

- Ready: centered logo watermark + source name, "Change source" pill button (top-right), PiP (if enabled).
- Recording/Paused: top-left REC/PAUSED chip with elapsed time, red/amber pulsing dot.
- Completed: centered play button overlay + "Recording saved" label with filename, resolution, format.

---

## 5. Settings Target

Settings uses a two-column card grid on desktop (gap 18px), collapsing to one column when width requires.

### Preset card (span 2, top)

| Control | Type |
|---|---|
| Preset dropdown | Select |
| Save as preset | Ghost button |
| Manage | Quiet button |
| Hint text | "A preset stores sources, codecs, quality, audio, webcam & output." |

### Format & Encoding card

| Control | Type |
|---|---|
| Container | Segmented: MKV / WebM / MP4 |
| Video codec | Select: AV1 / H.264 / H.265 / VP9 |
| Audio codec | Select: Opus / AAC / FLAC |
| Quality | Segmented: High / Balanced / Small / Custom |
| Frame rate | Select: 30 fps / 60 fps / 120 fps |
| Timing | Segmented: CFR / VFR |
| Capture cursor | Toggle (row) |

### Audio card

| Control | Type |
|---|---|
| System audio | Toggle + L/R stereo dB meter (mono dBFS readout) |
| Application audio | Toggle + L/R stereo dB meter |
| Microphone | Toggle + L/R stereo dB meter |
| Microphone device | Select |
| Separate tracks per source | Toggle (row) |

Meters are live when source is on, idle (-∞) when off. Gain/volume controls are present only if real.

### Webcam card (span 2)

| Control | Type |
|---|---|
| Webcam preview | 16:10 placeholder, mirror transform if enabled |
| Record webcam | Toggle (row) |
| Camera device | Select |
| Resolution / FPS | Select |
| Mirror image | Toggle (row) |
| Chroma key | Color swatch picker: Off / Green / Blue / Magenta |
| Chroma tolerance | Slider (0–100%) |

No placement controls. Hint: "Position & size are set directly in the Record preview."

### Output card (span 2)

| Control | Type |
|---|---|
| Output resolution | Segmented: Native / 4K / 1440p / 1080p / 720p / Custom |
| Output folder | Read-only display + Browse button |
| Filename pattern | Select: `exosnap_{date}_{n}` / `{source}_{datetime}` / `{preset}_{date}` |
| Token reference chips | `{datetime}` `{date}` `{time}` `{source}` `{app}` `{title}` `{preset}` |

---

## 6. Presets

Presets represent full recording setups, not only codec/quality:

- Source preference (display/window/region + geometry)
- Container (MKV / WebM / MP4)
- Video codec (AV1 / H.264 / H.265 / VP9)
- Audio codec (Opus / AAC / FLAC)
- Quality (High / Balanced / Small / Custom CQ)
- FPS (30 / 60 / 120)
- CFR / VFR
- Cursor capture on/off
- Audio source toggles (system / app / mic)
- Separate tracks per source
- Webcam settings (device, resolution, mirror, chroma key, tolerance)
- Output resolution
- Output folder
- Filename pattern

Schema implications: implementation concern (not automatic in R0).

---

## 7. Source Selection Target

Source picker is an in-window overlay modal (translucent backdrop + centered `QFrame`). Three tabs via segmented control: **Displays** / **Windows** / **Region**.

### Displays tab

- 2-column grid of display cards.
- Each card: thumbnail stripe, name, resolution + refresh rate.
- Selected state: accent border, check badge.

### Windows tab

- 3-column grid of window cards.
- Each card: thumbnail, name, resolution + executable.
- No search at current list size (only added if a long Windows list genuinely needs it).
- System/helper/tool windows hidden from default view.

### Region tab

- 3-column grid of region preset cards.
- Each card: aspect-ratio thumbnail or draw icon, name, resolution.

Region presets:

| Name | Resolution | Aspect |
|---|---|---|
| Draw custom | select an area | — |
| 16:9 Landscape | 1920 × 1080 | 16:9 |
| 16:9 HD | 1280 × 720 | 16:9 |
| 9:16 Vertical | 1080 × 1920 | 9:16 |
| 1:1 Square | 1080 × 1080 | 1:1 |
| 4:5 Portrait | 1080 × 1350 | 4:5 |

Region overlay behavior:

- Manual draw: drag to define area.
- Corner resize handle.
- Drag/move within bounds.
- Preset selection changes the selected region rectangle.
- Confirm applies selection; tab switch or Escape closes/cleans up.
- Click-only start (no accidental drag-triggers), too-small cancels, Escape during selection cancels.

### Modal footer

- Selected source summary (mono).
- Cancel button.
- "Use selected source" primary button.

---

## 8. Diagnostics Target

Diagnostics is the home for telemetry.

### Pipeline cards (primary)

```
Source Capture → Frame Queue → Compositor → Encoder → Muxer → Disk
```

Each step card shows:

- Status pill (OK / Hotspot / Over).
- Latency.
- Queue depth.
- Drops / throughput.
- Color-coded border and status bar (green / amber / red).
- Arrows between steps.

Bottleneck indicator displayed above the pipeline.

### Capability matrix (secondary, right column)

| Check | Status |
|---|---|
| GPU encoder | Available / unavailable |
| Display capture | DXGI duplication status |
| Audio loopback | WASAPI device count |
| Application audio isolation | Supported / unsupported |
| Output path | Writable / blocked |
| Color space | BT.709 / full range |

Pass/warn/fail with icon + count summary.

### Pipeline telemetry

Sparkline charts for:
- Encoder latency (ms) over 60s window.
- Bitrate (Mb/s) over 60s window.

### Recommendation card

Actionable guidance based on detected issues (e.g., "Application audio isolation unavailable for this target — use System audio or Window mode").

---

## 9. Hotkeys / Logs / About

### Hotkeys

- Table-like rows: action label (left), key combo chips (right).
- Monospace keycap styling with shadow.
- Click row to rebind.
- Conflict detection flagged inline.
- "Reset to defaults" button.
- Split by active vs. planned/unavailable sections.

Default bindings:

| Action | Keys |
|---|---|
| Start / Stop recording | Alt + F9 |
| Pause / Resume | Alt + F10 |
| Change source | Alt + S |
| Toggle microphone | Alt + M |
| Toggle webcam | Alt + W |
| Toggle system audio | Alt + A |
| Open diagnostics | Alt + D |
| Capture frame (screenshot) | Alt + P |

### Logs

- Contained log surface (monospace, 12.5px, 1.7 line-height).
- Columns: timestamp | level | message.
- Level coloring: info (muted), warn (amber), ok (green), error (red).
- Filter: All / Info / Issues (segmented control).
- Copy button.
- Footer note: full logs at `%LOCALAPPDATA%\ExoSnap\logs`.

### About

- Centered card dialog.
- Logo (56px) + wordmark (30px) + "Version 1.0 · for Windows".
- Description paragraph.
- Detail table: Version, Build, Commit, Encoder, Author.
- Action buttons: GitHub, Release notes, Copy diagnostics.
- Link to design spec.

---

## 10. MVP vs Later

### MVP (hybrid direction, resolves current feedback)

| Area | What's in |
|---|---|
| Shell | Frameless window, top nav, custom title bar, status pill |
| Tokens / accent | Single palette → QSS, bundled fonts (Hanken Grotesk + IBM Plex Mono), accent selection |
| Record | Preview surface, stable 3-zone bottom transport dock, all 4 states (ready/recording/paused/completed), countdown select, source-change pill |
| Settings | Two-column card grid, compact controls (segmented, select, toggle), Format & Encoding card, Audio card, Output card |
| Webcam | Settings card with preview, device/resolution/mirror/chroma controls (no PiP placement in MVP; position/size only in Record preview) |
| Source picker | In-window modal, three tabs (Displays / Windows / Region), card grids, region presets, basic region draw/resize |
| Diagnostics | Capability matrix, static pipeline cards with status labels (clearly labeled if metrics are not yet live) |
| Hotkeys | Table display with active bindings, reset button |
| Logs | Contained log viewer, mono body, level coloring |
| About | Clean centered dialog with version/build/commit/author/GitHub |

### Later (post-MVP polish and depth)

| Area | What's deferred |
|---|---|
| Region overlay | Full resize/move/preset behavior (if not fully implemented) |
| Webcam PiP | Drag/resize/full WYSIWYG in Record preview |
| Chroma key | Real-time processing (if not actually implemented) |
| Pipeline metrics | Live latency/throughput/drops gauges (static cards until instrumentation lands) |
| Sparklines | Qt Charts integration (ship static gauges first) |
| Hotkeys rebinding | Click-to-rebind + conflict detection |
| Logs filtering | All / Info / Issues filter |
| Presets | Full schema migration, save/manage/export |
| Audio meters | Live dBFS reading in Settings (static layout until real audio thread feed) |

---

## Design package files

Located at `.workspace/design/exosnap-hybrid-v3/`:

| File | Role |
|---|---|
| `spec.jsx` | Full design specification with brand, color, typography, spacing, components, implementation notes, and port plan |
| `hybrid-shared.jsx` | Shared toolkit: color tokens (`HT`), accent system, logo, buttons, toggles, segmented, select, field, card, modal, meters, app shell |
| `hybrid-record.jsx` | Record page: preview, PiP, transport dock (all states), source modal (displays/windows/region) |
| `hybrid-pages.jsx` | Settings, Hotkeys, Diagnostics (pipeline), Logs, About |
| `shared.jsx` | Icon set, window chrome, striped placeholders, audio meters, sparklines |
| `ExoSnap.html` | Interactive prototype (full app) |
| `ExoSnap — Design Spec.html` | Standalone spec page |
| `ExoSnap Design Exploration.html` | Exploration overview |
