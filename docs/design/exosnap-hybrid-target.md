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
- Accent variants exist in the design system. The first Qt port uses the default Studio Mint accent. A user-facing accent switcher is optional and should not block the initial token/shell work.

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

- Preview-first: the preview fills available space, 16:9 ratio. Use the existing `PreviewSurface` / DXGI preview integration where possible. The design target is visual/layout behavior — preserve the existing preview backend unless a later dedicated capture/preview slice requires replacement.
- Stable bottom transport dock (`QFrame`, 3-zone grid: left | duration | action).
- WYSIWYG webcam PiP overlay in Record only.
- Page padding: 20px around preview and dock.

### Webcam PiP — MVP vs Later

**MVP:**
- PiP visible in Record preview if webcam recording is enabled and a preview path exists.
- Settings contains webcam preview/device/mirror/chroma controls.
- No placement controls in Settings.

**Later:**
- Free drag/resize/placement in Record preview if not already technically supported.
- Advanced PiP styling/borders/shapes.
- Real-time chroma processing if not already implemented.

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
- Center: monospace timer, 30px, 600 weight, tabular-nums, always in the same position. **The countdown digit during pre-roll uses the same font size and weight as the recording timecode** — it is the primary user focus signal during the 3-second countdown (DF-03).
- Right zone: countdown select (Off / 3s / 5s / 10s) + Record button (ready); Pause + Stop (recording); Resume + Stop (paused); Record again (completed).
- Completed: filename is a clickable link, underlined in accent; **Open Folder is an icon-only 38×38 ghost button with border-radius 10, tooltip "Open folder"** (DF-04). File size readout is owned by the metadata bar (D4) — the dock does NOT show a size label.
- Stable geometry: all zones maintain position across state transitions. No layout shift.

### Preview overlay elements

- Ready: centered logo watermark + source name, "Change source" pill button (top-right), PiP (if enabled).
- Recording/Paused: top-left REC/PAUSED chip with elapsed time, red/amber pulsing dot.
- Completed: centered play button overlay + "Recording saved" label with filename, resolution, format.

---

## 5. Settings Target

Settings uses a two-column card grid on desktop (gap 18px), collapsing to one column when width requires.

### Preset card (span 2, top)

The preset card represents a complete recording setup stored and selected by name.

| Control | Type |
|---|---|
| Preset dropdown | Select (profileCombo) |
| Save | Ghost button — visible and enabled only when dirty |
| Save As… | Ghost button — always enabled |
| Manage | QToolButton with overflow menu (presetManageButton) |
| Dirty indicator | Amber "● Unsaved" label (presetDirtyIndicator) — visible only when dirty |
| Default badge | "Default" label (presetDefaultBadge) — visible when selected == startup default |
| Hint text | "A preset stores the complete recording setup: source, video, audio, webcam, countdown & output." |

The Manage overflow menu contains: Save preset, Save as new preset…, New preset from default…, Duplicate preset, Rename preset…, Delete preset, Set as default preset, Reset changes, Reset all presets to factory defaults….

Nothing in the preset card is "saved separately": the entire setup — capture target, format, audio, webcam, countdown — is the preset. The old hint ("Sources and audio are saved separately") is incorrect and has been removed.

### Format & Encoding card

| Control | Type |
|---|---|
| Container | Segmented: MKV / WebM / MP4 |
| Video codec | Select: AV1 / H.264 / H.265 / VP9 |
| Audio codec | Select: Opus / AAC / FLAC |
| Quality | Segmented: High / Balanced / Small / Custom |
| Frame rate | Select: 24 fps / 25 fps / 30 fps / 50 fps / 60 fps, 120 fps only when active capability is proven |
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

**Chroma key — MVP vs Later:**
- **MVP:** UI may show chroma key controls only if the feature exists or is clearly disabled/planned. Do not present chroma key as active if the capture/compositor path does not process it.
- **Later:** Real-time chroma key processing, tolerance pipeline integration, preview parity with final recording output.

### Output card (span 2)

| Control | Type |
|---|---|
| Output resolution | Segmented: Native / 4K / 1440p / 1080p / 720p; Custom remains schema-ready until the Settings UI exposes validated dimensions |
| Output folder | Read-only display + Browse button |
| Filename pattern | Select: `exosnap_{date}_{n}` / `{source}_{datetime}` / `{preset}_{date}` |
| Token reference chips | `{datetime}` `{date}` `{time}` `{source}` `{app}` `{title}` `{preset}` |

**Runtime output-directory override:** When the `EXOSNAP_OUTPUT_DIR` environment variable is set to a non-empty path at startup, all recordings and capture-frame outputs are written to that path instead of the configured output folder. The override is never written back to settings and does not appear in the Output settings UI. It is a tooling/CI isolation mechanism complementing `EXOSNAP_CONFIG_DIR` (DF-HISTORY). Implementation: `RecordingCoordinator::EffectiveOutputFolder()` in `app/services/RecordingCoordinator.h`.

---

## 6. Presets

Presets store the **complete** recording setup (schema version 2). A preset is the single authoritative source of the current recording configuration.

### Preset schema (`RecordingPreset` / `RecordingPresetConfig`)

| Field group | Fields |
|---|---|
| Identity | `id` (stable unique key), `name` (user-visible label) |
| Capture target (`PresetCaptureTarget`) | `kind` (Display/Window/Region), `display_key`, `window_key`, `has_region`, `region` (virtual-screen coords), `region_display_key` |
| Output (`OutputSettingsModel`) | Container (MKV/WebM/MP4), video codec (AV1/H.264/H.265/VP9), audio codec (Opus/AAC/FLAC), quality (High/Balanced/Small/Custom CQ), FPS (24/25/30/50/60; 120 only when capability-proven), CFR/VFR, cursor capture, output folder, naming pattern |
| Video (`VideoSettingsModel`) | CQ value, frame rate, timing mode, cursor |
| Audio (`AudioUiState`) | System / app / mic toggles, separate tracks, mic device |
| Webcam (`WebcamSettings`) | Device, resolution, FPS, mirror, overlay placement, enabled flag |
| Countdown | `countdown_seconds` ∈ {0, 3, 5, 10} |

### Persistence

Presets are persisted to `presets.ini` by `RecordingPresetStore`. The file stores `schemaVersion`, `defaultId`, `selectedId`, and one section per preset. Schema version 1 is the initial persisted format.

Startup boots to the **default preset**: the startup-default id is read from the store and the corresponding preset is applied atomically before the UI is shown.

### Canonical default preset

The single built-in default preset (id `preset.default`) is:

- Capture: Display (primary)
- Format: MKV + AV1 (Nvenc) + Opus
- Quality: High / CFR 60 fps
- Audio: System ON, App OFF, Mic OFF
- Webcam: OFF
- Countdown: 0 (off)

### Dirty state

The dirty indicator compares the **live working config** against the **selected preset's saved config** using `ConfigDirtyEquivalent`. Capture identity (target kind, display key, window key, region) is excluded from the dirty comparison because it is transient: device availability and auto-resolution at startup would otherwise make the preset spuriously dirty on every launch.

### Breaking changes vs partial profiles (v0)

Old partial profiles (pre-schema-v1) stored only codec/quality. When `schemaVersion` is absent or less than 1, the store resets all presets to the factory default rather than migrating. Import/export actions are removed. Webcam settings are now per-preset (previously app-global). `AppSettingsStore` is reduced to hotkeys and window geometry; settings file version is 6.

---

## 7. Source Selection Target

Source picker is an in-window overlay modal (translucent backdrop + centered `QFrame`). Three tabs via segmented control: **Displays** / **Windows** / **Region**.

**Overlay modal behavior:**

- The overlay widget covers the entire central area (including title bar) so navigation is not accessible while the modal is open (DF-A11Y).
- Backdrop scrim: `rgba(8, 8, 10, 0.62)` — the overlay paints this over its full rect so the page behind is visibly dimmed (DF-02).
- Backdrop click (outside the picker panel) dismisses the overlay with cancel semantics (emits `closed()` signal identical to Escape).
- Both `SourcePickerOverlay` and `AboutOverlay` are parented to the QMainWindow central widget, not to the page `QStackedWidget`, so their subtrees are always reachable by UI Automation regardless of which stack page is current (DF-A11Y).

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

### Pipeline metrics — MVP vs Later

**MVP:**
- Pipeline section can be static/planned if real metrics are not instrumented.
- Static/planned cards must be clearly labeled (e.g., "Pipeline metrics not yet instrumented").
- Capability list should remain real (uses existing backend probes).
- No fake precision on any metric.

**Later:**
- Live latency/queue depth/drops/throughput instrumentation.
- Sparkline charts (Qt Charts or custom paintEvent) for encoder latency and bitrate.
- Real bottleneck detection based on live data.

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
- Rows: timestamp | severity | category | message.
- Severity is stored structurally by the app log backend, not inferred from rendered text.
- Level coloring: debug (muted), info (normal), warning (amber), error (coral/red).
- Filter: All / Info / Issues (segmented control); search matches category and message case-insensitively.
- Copy exports the currently visible filtered rows as deterministic plain text.
- Export writes the complete current in-memory log history as UTF-8 text.
- Clear empties the current in-memory history; new entries continue normally.
- Auto-scroll is user-controlled and does not force position while disabled.
- Footer note: full session logs at `%LOCALAPPDATA%\ExoSnap\logs`.

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
| Tokens / accent | Single palette → QSS, bundled fonts (Hanken Grotesk + IBM Plex Mono), default Studio Mint accent (curated accent variants in data structures; user-facing accent switcher is optional/later) |
| Record | Preview surface (preserving existing DXGI path), stable 3-zone bottom transport dock, all 4 states (ready/recording/paused/completed), countdown select, source-change pill |
| Webcam PiP | Visible in Record preview if webcam is enabled and a preview path exists (drag/resize/free placement is Later) |
| Settings | Two-column card grid, compact controls (segmented, select, toggle), Format & Encoding card, Audio card, Output card |
| Webcam card | Settings card with preview, device/resolution/mirror controls; chroma key UI only if feature exists or is clearly disabled/planned |
| Source picker | In-window modal, three tabs (Displays / Windows / Region), card grids, region presets, basic region draw/resize |
| Diagnostics | Capability matrix (real backend probes), pipeline section static/planned if metrics not instrumented (clearly labeled) |
| Hotkeys | Table display with active bindings, reset button |
| Logs | Contained log viewer, mono body, level coloring |
| About | Clean centered dialog with version/build/commit/author/GitHub |

### Later (post-MVP polish and depth)

| Area | What's deferred |
|---|---|
| Region overlay | Full resize/move/preset behavior (if not fully implemented) |
| Webcam PiP | Drag/resize/free placement in Record preview |
| Chroma key | Real-time processing with tolerance pipeline integration and preview parity (if not actually implemented) |
| Pipeline metrics | Live latency/throughput/drops gauges + sparklines (static cards until instrumentation lands) |
| Hotkeys rebinding | Click-to-rebind + conflict detection |
| Logs filtering | All / Info / Issues filter |
| Presets | Full schema migration, save/manage/export |
| Audio meters | Live dBFS reading in Settings (static layout until real audio thread feed) |
| Accent switcher | User-facing Tweaks panel / live accent switching / persisted accent preference |

---

---

## 11. Implementation Deviations (QSS / Widget fidelity)

Tracked here so the design-tracking file reflects the actual shipped behavior.

### DF-07 — Source row chrome (FIXED 2026-06-11)

`QFrame#recordSourceChip` border and background removed. The source row is now a slim borderless strip that reads as preview context metadata without competing with the preview surface border. The locked state (`sourceLocked="true"`) retains no visual chrome.

### DF-08 — TransportDock icon toggles: integrated audio meters (INTENTIONAL DEVIATION)

The design spec shows plain icon toggles in the transport dock left zone. The implemented toggles include live 3-pixel RMS meter strips rendered below each circle icon, wired to real ~30 Hz audio callbacks (see AUDIO-METER-R3A). This is an intentional product feature — richer than the plain `IconToggle` in the spec.

### DF-09 — Recent recordings filename column (FIXED 2026-06-11)

The recent-recordings filename button is now a fixed-width 260px column with middle-ellipsis elision via `QFontMetrics::elidedText`. Full file path is in the tooltip.

### DF-11 — Recording pill drop count (FIXED 2026-06-11)

When dropped frames > 0 during recording, the titlebar Recording pill shows `"Recording · N↓"`. The drop count is reset when recording stops. Connected via `chromeRuntimeMetricsChanged` signal in MainWindow → `OperationalTitleBar::setRecordingDropCount()`.

### DF-12 — Settings Audio "Separate track" control (FIXED 2026-06-11)

The `QCheckBox "Separate track"` for each audio source (sys/app/mic) has been replaced with an `ExoToggle` pill toggle + "Separate track" `QLabel`. This aligns with the design spec toggle style for boolean rows. Signal is `QAbstractButton::toggled`.

### DF-13 — Log viewer font size (FIXED 2026-06-11)

`QPlainTextEdit#logViewer font-size` updated from `12px` to `12.5px`, matching the spec value of `12.5px / 1.7 line-height`. IBM Plex Mono is not bundled (deferred as OOS-05, `${font-mono}` falls back to JetBrains Mono); the size fix is applied regardless.

### DF-14 — Hotkeys unset row dimming (SKIPPED-INVALID 2026-06-11)

The "Unset" keycap chip already uses `stateRole="muted"` with `color: ${text3}; background: ${bg2}; border-color: ${line1}`. This is the lowest visible text tone — additional dimming would approach invisible. The existing QSS is the correct implementation.

### DF-15 — Countdown pill color (FIXED 2026-06-11)

Added `StatusPill::Tone::Info` (azure `#7FBEE8`). `COUNTDOWN` and `STARTING` states now use `Tone::Info` instead of `Tone::Warn` (amber). This makes Countdown/Starting visually distinct from Paused (amber). Paused remains on `Tone::Warn`.

### DF-16 — Settings preset "Default" badge (SKIPPED-PRODUCT-DECISION 2026-06-11)

The `presetDefaultBadge` is an intentional indicator that the selected preset is the startup default — it is NOT a dirty-state indicator. Changing it would remove a tested meaningful feature. Kept as-is.

### DF-17 — AboutOverlay scrim opacity (SKIPPED-INVALID 2026-06-11)

`kBackdropAlpha = 158 = 0.62 × 255`, exactly matching the design spec scrim of `rgba(8, 8, 10, 0.62)`. Already correct.

### DF-18 — Hotkeys "Not in this build" section header subtitle (FIXED 2026-06-11)

`planned_header->setMeta("Not in this build")` removed from HotkeysPage.cpp. The section is titled "PLANNED / UNAVAILABLE" which is self-explanatory. Each individual planned row already shows a "Not in this build" badge — the section-level repetition was redundant noise.

### D3 — Logs page toolbar (FIXED 2026-06-11)

Cut Refresh, Clear, and Open Log Folder from the toolbar. Redesigned as single-row toolbar: LEFT cluster = segmented All/Info/Issues + search field (flex) + Auto-scroll; 16px gap; RIGHT cluster = Copy + Export… ghost-sm buttons (h32, radius 999, border line2). Footer path is a clickable folder link (mono 12, accent color, underline) that opens the log folder. Copy operates on the currently filtered view. Removed buttons are absent from the DOM (EQ null in tests).

### D4 — Saved-state metadata bar (FIXED 2026-06-11)

(1) Metadata bar buttons (Copy path, Rename, Delete) styled ghost-sm (h32, radius 999); Delete gets coral hover tint only.
(2) TransportDock: size readout (`size_label_`) hidden immediately after creation — the metadata bar owns file size. Label still exists as a C++ member for API compatibility but is never added to layout and never made visible.
(3) Recent rows: per-row "Folder" button removed. Filename chip uses `setMinimumWidth(280)` + `Qt::ElideMiddle` elision instead of the old fixed 260px width.
(4) Audio codec display casing: "OPUS" corrected to "Opus" in both `audioCodecLabel` overloads in RecordPage.cpp.

### D3/D4 — RescanSVG icon (FIXED 2026-06-11)

The ↺ (U+21BA) character was not rendering in IBM Plex Mono / Segoe UI — displayed as "!". Replaced with `:/theme/icons/rescan.svg` (16×16 SVG, currentColor fill) in both `WebcamSetupPanel` and `ConfigPage` audio rescan buttons. SVG registered in `exosnap_theme.qrc`. Text fallback preserved if SVG loads as null.

### Polish #01 — AboutOverlay build table (FIXED 2026-06-11)

Fixed clipped rows in the About overlay build metadata table. Removed fixed row height; set `min-height: 28px` via `setContentsMargins(0, 6, 0, 6)` per row, and `4px` bottom padding on the container.

### Polish #02 — Recent-row filename chip (FIXED 2026-06-11)

Changed from `setFixedWidth(260)` + `Qt::ElideMiddle` at 236px to `setMinimumWidth(280)` (no fixed cap) + `Qt::ElideMiddle` at `minimumWidth()-24`. Chip can grow with available space.

### Polish #04 — Diagnostics summary tiles zero-count (FIXED 2026-06-11)

Tile tinting is now conditional: count==0 → `statTone="zero"` (neutral bg2, line2 border, text3 numeral and label). Count>0 → active tone (blocker/notice/pass). Both QSS rules and C++ `setTileActive()` lambda added.

### Polish #05 — Diagnostics pipeline card dimming (FIXED 2026-06-11)

Planned pipeline step title color changed from `${text1}` (slightly dim) to `${text2}` (mut) in QSS, so Planned cards are visually quieter than OK cards whose title stays at `${text0}` (ink).

### Polish #06 — Hotkeys "Unset" → "Clear" + "Not set" chip (FIXED 2026-06-11)

Unset button text: `"Unset"` → `"Clear"`. KeycapChip empty_text: `"Unset"` → `"Not set"`. Object names unchanged (tests still find `hotkeyUnsetBtn_N`).

### Polish #08 — Settings webcam: disabled selects when no camera (FIXED 2026-06-11)

When no webcam device is selected, resolution and FPS combos now show a `"(no camera)"` placeholder item and are disabled (`setEnabled(false)`).

### Polish #09 — Rescan button SVG icon (FIXED 2026-06-11)

See D3/D4 — RescanSVG icon entry above.

### Polish #10 — Diagnostics pluralization (FIXED 2026-06-11)

`"NOTICE(S)"` → `"NOTICE"` / `"NOTICES"` based on count. `"blocker(s)"` → `"blocker"` / `"blockers"`. `"item(s)"` → `"item"` / `"items"`.

### Polish #11 — Logs meta line path truncation (FIXED 2026-06-11)

`folder_link_` in LogsPage shows `parentDir/filename` elided to 320px width via `QFontMetrics::elidedText(Qt::ElideMiddle)`. Full path is in the tooltip.

### Polish #12 — Countdown select timer glyph (FIXED 2026-06-11)

`CountdownSelect` items now use `addItem(timer_icon, text, data)` with a 14×14 `:/theme/icons/timer.svg` icon. Fallback to icon-less items if SVG not found. SVG registered in `exosnap_theme.qrc`.

### Polish #13 — Countdown disabled during countdown (VERIFIED 2026-06-11)

`CountdownSelect::setInteractive(false)` is called when `state_ == Countdown` (ready=false in `setReady(false && primary_enabled_)`). `Countdown_StateShowsCancelAndLocksSelector` test already passes, confirming correct behavior.

### Polish #14 — Diagnostics "Active configuration" caption inside card (FIXED 2026-06-11)

`makeCollapsibleSection` now places the subtitle label inside the `body` widget (collapses with it) instead of the outer `wrap` layout. The caption "Recording settings as currently configured in the app." is now hidden until the collapsible is opened.

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
