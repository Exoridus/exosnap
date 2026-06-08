# ExoSnap Hybrid v3 — Qt Widgets / QSS Port Plan

Source: `.workspace/design/exosnap-hybrid-v3/spec.jsx` (Section 07 — Qt Widgets / QSS port plan)
Reference: `docs/design/exosnap-hybrid-target.md`

Last refreshed: 2026-06-03

---

## Overview

This plan sequences the native Qt Widgets + QSS build of the Hybrid v3 design. Each phase is shippable on its own. Phases R1A–R3 land the MVP and resolve current "debug-heavy / oversized / kiosk" feedback. Phases R4–R6 layer on modals, telemetry, and utility-surface polish.

The prototype at `.workspace/design/exosnap-hybrid-v3/ExoSnap.html` is the pixel reference.

---

## HYBRID-PORT-R1A — Tokens / Fonts / Accent Foundation

### Scope

- Introduce Hybrid v3 token structure.
- Map Hybrid colors into `ExoSnapPalette`, `ExoSnapMetrics`, and QSS variables/roles.
- Add or prepare font loading for Hanken Grotesk and IBM Plex Mono via `QFontDatabase::addApplicationFont`.
- Implement default accent token support (single primary accent: Studio Mint `#9BD9D2`).
- Prepare curated accent variant data structures (6 accents) for future use.
- Do not require a visible Tweaks panel yet unless trivial and low-risk.
- Do not replace navigation/shell yet.
- Do not remove the existing sidebar yet.
- Do not remove `GlobalRecordingBar` yet.

Accent scope note:
- Accent variants exist in the design system as defined data structures.
- The first Qt port uses the default Studio Mint accent.
- A user-facing accent switcher / Tweaks panel is optional and should not block this phase.
- Do not implement live accent CSS-variable switching if it increases risk.

### Likely files/classes

**New / heavily modified:**
- `ui/theme/exosnap_dark.qss` — palette replacement (new hybrid tokens: `#0E0E10` bg, `#151517` surf, `#1C1C1F` surf2, `#9BD9D2` accent, etc.).
- `ui/theme/ExoSnapPalette.h` — new hybrid color tokens.
- `ui/theme/ExoSnapMetrics.h` — new spacing/radius metrics (8px base grid, 9–16px radii scale).
- `ui/theme/ExoSnapTheme.h/.cpp` — updated `ApplyExoSnapTheme()` loading the new QSS; font bundle loading.
- `ui/brand/BrandMarkWidget.h/.cpp` — update to new circular aperture mark logo.

**Not changed (yet):**
- `MainWindow.h/.cpp` — sidebar and page stack remain unchanged.
- `ui/chrome/OperationalTitleBar.h/.cpp` — title bar unchanged.
- `ui/chrome/GlobalRecordingBar.h/.cpp` — global bar unchanged.

### Risks

- QSS token migration touches nearly every widget style rule.
- Font bundling and license compliance for Hanken Grotesk + IBM Plex Mono.
- New tokens must render correctly on every existing page without structural changes.

### Validation

- Screenshot smoke: every page at default and maximized widths — existing pages still render correctly with new tokens.
- No functional changes.
- Existing tests green.
- Verify new tokens produce correct dark-mode look; no leftover amber-gold palette from previous design system.

### Do not touch

- Navigation, sidebar, shell structure.
- Capture backend, encoder, muxer, audio pipeline.
- Recording state machine logic.
- Diagnostics backend probes.
- Settings schema / profile registry.
- Tests (update only if existing tests break due to token changes).

### Model recommendation

**Claude Opus xhigh** — token system design, QSS variable mapping, font bundling. The split from shell work reduces risk.

### Implementation status — landed

- **Tokens:** `ExoSnapPalette` remapped to the Hybrid v3 roles (neutral cool-dark `#0E0E10`/`#151517`/`#1C1C1F`/`#242428`, white-alpha hairlines, ink/muted/dim text ramp, Studio Mint `#9BD9D2` accent, coral `#E0786C` / green `#84CBA2` / amber `#E6C57C` semantics). Token *names* were preserved so the token-driven QSS keeps resolving without a structural rewrite. New `${accent-ink}` (`#08130F`) token added for text on accent fills.
- **Accent variants:** the 7 curated accents (mint, amber, coral, azure, violet, lime, graphite) are defined as data only in `ui/theme/ExoSnapAccents.h`. No user-facing switcher (out of scope); a compile-time check keeps the default in sync with the palette.
- **Metrics:** radius scale softened to the Hybrid range (sm 8 / md 10 / lg 14); spacing and control heights unchanged.
- **Fonts:** target faces **Hanken Grotesk** (UI) and **IBM Plex Mono** (mono) are *not bundled* — no license-safe files are present in the repo and fonts are never copied from system folders. They sit at the front of the family stacks (`Hanken Grotesk, Inter, Segoe UI, sans-serif` / `IBM Plex Mono, JetBrains Mono, Consolas, monospace`) and are used only if the system provides them. The bundled Inter / JetBrains Mono remain the guaranteed fallback. *This deviates from the "bundled fonts" wording in `exosnap-hybrid-target.md` §10; bundling can be revisited if/when license-safe files are added.*
- **BrandMark:** `BrandMarkWidget` now paints the concentric aperture mark programmatically (idle Studio Mint; coral recording variant exposed via `setRecording()`, currently unwired pending R1B title-bar status). `exosnap-logo.svg` updated to the matching aperture.
- **QSS:** all hard-coded amber-accent/warn/ok/err and warm-neutral literals migrated to the Hybrid palette; on-accent text switched to `${accent-ink}`; Stop-button label darkened for contrast on coral. Object names, widget hierarchy, and test seams unchanged.
- **Validation:** debug build green; focused + full CTest (544 tests) pass; screenshot smoke across Record/Settings/Hotkeys/Diagnostics/Logs/About/Source Picker shows correct dark-mode look, no leftover amber, no contrast/font/parse regressions.
- **Not touched:** shell/sidebar/top-nav, `OperationalTitleBar`, `GlobalRecordingBar`, Record/Settings/Source layouts, capture/encoder/muxer/audio, settings schema, build metadata.

---

## HYBRID-PORT-R1B — Shell / Top Nav / Titlebar

### Scope

- Replace or adapt the current shell to the Hybrid v3 top-navigation direction.
- Add custom titlebar/status pill behavior.
- Move toward top navigation: Record, Settings, Hotkeys, Diagnostics, Logs, About.
- Keep routing implementation low-risk.
- Top navigation may continue to drive the existing `QStackedWidget` page model. Keep or replace `QStackedWidget` based on lowest-risk implementation. Do not replace routing architecture just for aesthetic reasons.
- Do not require complete accent Tweaks UI unless already prepared in R1A.
- Remove/demote old sidebar only once top nav is working.
- Remove/absorb GlobalRecordingBar only if equivalent status behavior exists in the title bar pill.

### Likely files/classes

**New / heavily modified:**
- `MainWindow.h/.cpp` — add top nav `QButtonGroup`; sidebar removal only after top nav is functional. Keep `QStackedWidget` behind the nav if it is the safest routing model.
- `ui/chrome/OperationalTitleBar.h/.cpp` — merge title bar and nav into a single 52px bar hosting logo + nav tabs + status pill + window controls.
- `ui/chrome/GlobalRecordingBar.h/.cpp` — demoted or absorbed into title-bar status pill once equivalent behavior is verified.
- `ui/chrome/RecordingStatusGuards.h` — update guards for title-bar status pill.

**Removed / repurposed:**
- Sidebar `QListWidget` — removed only after top nav is fully functional.
- `GlobalRecordingBar` — absorbed only when title-bar status pill covers the same states.

### Risks

- Frameless window on Windows requires careful DPI and resize handling.
- Sidebar → top nav migration requires routing all page-creation code.
- Status pill must correctly consume recording state machine states (Ready / Recording / Paused / Completed).
- Removing GlobalRecordingBar prematurely could lose recording-health visibility.

### Validation

- Navigation smoke for all pages: Record, Settings, Hotkeys, Diagnostics, Logs, About.
- Titlebar states: Ready (green), Recording (red + metrics), Paused (amber).
- No regression to recording behavior — start/stop/pause/resume all work.
- Screenshot smoke default + maximized.
- Verify all pages are still accessible and render correctly.

### Do not touch

- Capture backend, encoder, muxer, audio pipeline.
- Recording state machine logic (only consume its state for the pill).
- Diagnostics backend probes.
- Settings schema / profile registry.
- Tests (update only if existing tests break due to removed/renamed widgets).

### Model recommendation

**Claude Opus xhigh** — architecturally sensitive (frameless window, nav restructuring, title-bar state pill). The split from token work reduces the blast radius.

### Implementation status — landed

- **Shell:** the left `QListWidget` sidebar (and its footer About button) and the secondary `mainPageHead` title/subtitle/meta strip were removed. `OperationalTitleBar` now hosts the whole shell on one 56px bar: aperture mark + lowercase two-tone `exosnap` wordmark, top-nav tabs, a flexible drag spacer, the status pill, and min/max/close. The existing frameless window / native resize / DWM-border handling was kept as-is (lowest risk).
- **Navigation:** top nav is **Record · Settings · Hotkeys · Diagnostics · Logs · About**. The five page tabs are checkable buttons in an exclusive `QButtonGroup` that drive the existing `QStackedWidget` (routing model unchanged); the active tab gets a 2px accent underline. About stays a `QDialog` launched from its tab (no stack page added). Advanced/Webcam remain reachable sub-pages and keep the Settings tab lit.
- **Status pill:** consumes the existing recorder state only (`Ready` / `Recording` / `Paused`, plus the real transient `Checking`/`Starting`/`Stopping` and `Blocked`/`Error` states), now in hybrid title-case. The brand mark turns coral while recording via `BrandMarkWidget::setRecording`. No fabricated recording-health metrics are shown at the shell layer (real per-frame metrics are not surfaced here).
- **GlobalRecordingBar:** was already not instantiated in the shell; left inert/untracked-by-shell (file + unit test retained) since the title-bar pill now covers the same states. No transport buttons, debug telemetry, page numbers, or global CPU/GPU/RAM/Disk stats were reintroduced.
- **Tests:** new `chrome.OperationalTitleBarTest` covers nav tabs/order, no page numbers, About-as-action, `setActivePage`, status-pill states, and absence of transport buttons. Debug build green; focused + full CTest pass (550/550). Screenshot smoke under `.workspace/screenshots/hybrid-port-r1b-shell-titlebar/`.
- **Not touched:** Record/Settings/Source/Webcam/Diagnostics/Logs/Hotkeys page interiors, capture/encoder/muxer/audio, recording state machine, settings schema, build metadata, PreviewSurface/DXGI.

---

## HYBRID-PORT-R2 — Record Preview + Stable Bottom Dock

### Scope

- Preview-first layout: the preview fills available space. The design target is visual/layout behavior, not a mandate to replace the preview backend.
- **Preserve the existing `PreviewSurface` / DXGI preview integration where possible.** Do not replace the preview backend with `QVideoWidget`/`QOpenGLWidget` unless there is a specific technical reason in a later dedicated capture/preview slice.
- Remove debug terminal / legacy transport from preview surface.
- Stable bottom transport dock `QFrame` with 3-zone grid (left: source toggles | center: duration | right: actions).
- All four states: Ready, Recording, Paused, Completed (Result).
- Custom stereo dB meter widget (`paintEvent`) fed by audio thread at ~30 Hz.
- Recording health readouts (fps, bitrate, dropped, size) in the dock while capturing.
- Countdown select (Off / 3s / 5s / 10s) on the right side of the dock.
- Preview overlay elements: REC/PAUSED chip, watermark, source name, "Change source" pill, result playback overlay.

### Webcam PiP — MVP vs Later

**MVP:**
- PiP visible in Record preview if webcam recording is enabled and a preview path exists.
- Settings contains webcam preview/device/mirror/chroma controls.
- No placement controls in Settings.

**Later:**
- Free drag/resize/placement in Record preview if not already technically supported.
- Advanced PiP styling/borders/shapes.
- Real-time chroma processing if not already implemented.

Do not present PiP as fake-active if the Qt app cannot support drag/resize yet.

### Likely files/classes

**New / heavily modified:**
- `pages/RecordPage.h/.cpp` — complete restructure: preview-dominant layout, dock integration, remove legacy right-rail transport.
- `ui/widgets/PreviewSurface.h/.cpp` — preserve existing DXGI/rendering path; update layout and overlay support only.
- New: `ui/widgets/TransportDock.h/.cpp` — 3-zone grid dock widget with all state layouts.
- New: `ui/widgets/AudioSourceToggle.h/.cpp` — circular icon toggle (System/Mic/Webcam/App).
- New: `ui/widgets/StereoMeterWidget.h/.cpp` — custom paintEvent stereo dB meter (L/R bars).
- New: `ui/widgets/CountdownSelect.h/.cpp` — compact dropdown for recording delay.
- `ui/widgets/VUMeterWidget.h/.cpp` — update or replace with stereo version.

**Removed / repurposed:**
- Legacy RecordPage transport controls (the right-rail pause/stop/record buttons).
- Debug terminal overlay on preview.

### Risks

- Preview surface layout changes must not break existing DXGI swapchain rendering.
- Transport dock state transitions (Ready → Recording → Paused → Completed) must be driven by `RecordingCoordinator` state machine.
- Stereo meter requires real audio thread feed; ensure ~30 Hz update doesn't block UI thread.
- Completed state needs access to result data (filename, size, duration, format) from recording coordinator.

### Validation

- Screenshot smoke: all four dock states at default and maximized.
- Verify dock geometry is stable — no shift between states.
- Verify audio toggle states reflect actual source on/off and are wired to recording config.
- Verify countdown works end-to-end for Off / 3s / 5s / 10s, including Cancel and Escape.
- Verify preview fills available space on window resize.
- Verify no regression on Settings/Diagnostics/other pages.
- Verify existing DXGI preview still renders correctly after layout changes.

### Do not touch

- DXGI capture backend / swapchain.
- Settings/config pages.
- Diagnostics.
- Source picker modal.
- Hotkeys/Logs/About.
- Encoder, muxer.
- Settings schema.

### Model recommendation

**Claude Opus xhigh/max** — complex state-driven widget with real-time audio meter painting, recording-coordinator integration, and precise pixel alignment to prototype dock.

### Implementation status — R2A landed

Delivered as **R2A — Record Preview + Dock Skeleton** (the full four-state dock with real data); the genuinely honest-risky pieces (live meters, title-bar health metrics) are deferred to **R2B**.

- **Preview-first layout:** `RecordPage` is now a single column — the existing `PreviewSurface` fills the available area above a bottom dock (page gutter 24px, 16px gap). `updatePreviewHeightClamp()` sizes the surface to the largest 16:9 rectangle that fits the host (no rail width reserved); `updateResponsiveLayout()` collapsed to a single-column no-op. The DXGI/preview backend, webcam PiP overlay, and `PreviewSurface` API are untouched. The legacy right rail / kiosk transport is gone from the visible page.
- **New widgets:** `ui/widgets/TransportDock` (3-zone `QFrame`: left toggles/result | center duration | right actions, stable geometry across states), `ui/widgets/AudioSourceToggle` (circular self-painted icon pill; SVG icons via `QSvgRenderer`; on/off + interactive/read-only), `ui/widgets/CountdownSelect` (Off/3s/5s/10s compact recording-delay selector).
- **State behaviour:** Ready `[toggles] · 00:00:00 · countdown + Record]`; Recording `[toggles · timer · Pause + Stop]`; Paused `[toggles · timer(amber) · Resume + Stop]`; Completed `[filename link + Open folder + size · timer(green) · Record again]`. Driven from `updateTransportDock()` off the existing `RecordViewModel`/`RecordingCoordinator` state — no new engine wiring.
- **Honesty:** audio toggles edit pre-record `AudioUiState` (System/Mic/App) via the existing `audioSettingsChanged` path and become read-only status pills while the source is locked; the webcam toggle is an honest read-only status pill (configured in Settings); countdown runs as a UI-level delay before backend start and never fabricates a recording session; the dock shows **no** audio meters and **no** fabricated recording-health numbers. Completed uses real result filename/size/duration; the filename opens the file and Open folder reveals it.
- **Legacy preservation:** the old below-preview sections (audio settings, destination, readiness, target pickers, result panel, right rail) are constructed exactly as before but parked off-screen in a hidden `recordLegacyHost` so every `refresh()`/`updateStats()`/`updateResult()` pointer stays valid and no engine path changed. They are removed for real when Settings absorbs them in **R3**.
- **Tests:** new `record.TransportDockTest` (11 cases) covers the dock seams/objectNames, per-state visibility (Ready→Record, Recording→Pause+Stop, Paused→Resume+Stop, Completed→Record again + result info), primary-enable gating, interactive vs read-only toggles, the record signal, the timer text/role, and the absence of a kiosk "Start Recording" label. Debug build green; focused (264) + full (561) CTest pass; screenshot smoke under `.workspace/screenshots/hybrid-port-r2-record-dock/`.
- **Deferred to R2B:** live L/R stereo dB meters in the dock (real per-channel data not yet plumbed; no fakes added), title-bar Completed→"Saved" pill + recording-health metrics, and final pixel polish.
- **Not touched:** Settings/Source/Webcam/Diagnostics/Logs/Hotkeys/About interiors, R1B shell/top-nav (status pill semantics unchanged), capture/encoder/muxer/audio internals, recording state machine, settings schema, build metadata, `PreviewSurface`/DXGI.

### Implementation status — R2B landed

Delivered as **R2B — Record Dock Polish / Completed Status / Source Pill**, finishing the R2A visual gaps without expanding scope.

- **Completed → "Saved" title pill:** `RecordPage::buildChromeStatusLabel()` now returns `SAVED` for a clean saved recording (`Completed` + `HasResult` + `last_succeeded`); `OperationalTitleBar` maps `SAVED` to a green `Saved` pill (same tone as Ready, distinct label), shown while the result dock is visible and reverting to Ready on the next record. No new coordinator/state-machine state was introduced — the label is derived from existing view-model result state. `MainWindow` normalizes `SAVED` → `READY` for the Settings readiness badge so Settings behaviour is unchanged.
- **Source/change-source pill cleanup:** kept above the preview (the DXGI preview runs a native child HWND that is live in the Ready state, so on-surface Qt overlay pills would be occluded — PreviewSurface/DXGI internals untouched per the sanctioned fallback). Visually compressed: the vestigial empty pre-R2A `preview_context_row_` is collapsed, the column gap tightened (6px) so the source row reads as the preview's context header, and the source chip slimmed to a lighter pill. `Change source` still opens the unchanged Source Picker and stays locked while recording/paused.
- **Dock pixel polish:** countdown select height aligned to the 40px action buttons (Ready-zone alignment), completed-zone Open folder button aligned to 40px. Stable 3-zone geometry preserved.
- **Meter honesty:** no stereo meters added — no real L/R dBFS feed exists; deferred until the audio pipeline provides per-channel data. No mono RMS duplicated into fake stereo.
- **Recording-health title metrics:** none added — encoder load is not available at the shell layer, so the simple Recording pill is kept (no generic CPU/GPU/RAM/Disk, no fabricated Drop/Frame/Enc).
- **Tests:** new `chrome.OperationalTitleBarTest.StatusPill_ShowsSavedAfterCompletedRecording`; existing `record.TransportDockTest` unchanged and green. Debug build green; focused CTest 265/265; full CTest 562/562. Screenshot smoke under `.workspace/screenshots/hybrid-port-r2b-record-polish/`.
- **Not touched:** Settings IA / Source Picker behaviour / Webcam / Diagnostics / Logs / Hotkeys / About, capture/encoder/muxer/audio internals, recording state machine, settings schema, build metadata, `PreviewSurface`/DXGI, right-rail/global-transport (remain absent).

---

## HYBRID-PORT-R3 — Settings Compact IA

### Scope

- Restructure settings into the hybrid card layout (two-column grid).
- Replace large quality cards with segmented control (QButtonGroup: High / Balanced / Small / Custom).
- Replace current page-based settings (VideoPage, AudioPage, OutputPage, WebcamPage, ConfigPage) with single Settings page containing cards:
  - Preset card (span 2, top)
  - Format & Encoding card
  - Audio card
  - Webcam card (span 2)
  - Output card (span 2)
- QComboBox selects for codec/container/preset/devices.
- QSS switch toggles (mirroring prototype `Toggle` component).
- Output folder + filename pattern row with token reference chips.
- Preset management (create/rename/export) — marked as Later; UI scaffolding only in this phase.
- Compact audio source rows with inline stereo meters.
- Remove separate Video, Audio, Output, Webcam, Advanced as top-level pages; Advanced becomes a section or detail within Settings.

### Likely files/classes

**New / heavily modified:**
- New: `pages/SettingsPage.h/.cpp` — unified settings page with two-column card grid.
- `pages/ConfigPage.h/.cpp` — absorb or replace; becomes SettingsPage.
- New: `ui/widgets/SettingsCard.h/.cpp` — card container with title, right accessory, border, radius (shared across cards).
- `ui/widgets/AudioSourceRow.h/.cpp` — update to compact row style (icon + label + dB readout + toggle + inline stereo meter).
- `ui/widgets/SegmentedControl.h/.cpp` — new widget for segmented button groups (container, quality, FPS, timing, output resolution).
- New: `ui/widgets/ChromaKeyPicker.h/.cpp` — color swatch grid for webcam chroma key. **MVP:** UI may show chroma key controls only if the feature exists or is clearly disabled/planned. Do not present chroma key as active if the capture/compositor path does not process it. **Later:** real-time chroma key processing, tolerance pipeline integration, preview parity with final recording output.

**Removed / repurposed:**
- `pages/VideoPage.h/.cpp` — absorbed into SettingsPage.
- `pages/AudioPage.h/.cpp` — absorbed into SettingsPage.
- `pages/OutputPage.h/.cpp` — profile management absorbed; output settings into SettingsPage.
- `pages/WebcamPage.h/.cpp` — absorbed into SettingsPage Webcam card.
- `pages/AdvancedPage.h/.cpp` — collapsed advanced section within Settings.
- `ui/widgets/CodecCard.h/.cpp` — removed (replaced by segmented + select).
- Quality card widgets — removed (replaced by segmented control).

### Risks

- Consolidating 5+ pages into one settings page is a large structural change.
- Audio source rows need real or simulated dBFS values; fake values must be labeled clearly.
- Preset management scaffold must not break existing recording profile registry.
- Webcam card must preserve real webcam preview; no fake PiP.
- Advanced settings must not be lost; collapsed section or dedicated detail panel.

### Validation

- Screenshot smoke: Settings at default and maximized, scrolled to each card.
- Verify every control that was on the old pages has a home on the new Settings page.
- Verify two-column grid collapses to one column at narrow widths.
- Verify container/codec reconciliation still works (e.g., MP4 hides Opus).
- Verify webcam preview still renders real camera feed.
- Verify no regression on Record page, Diagnostics, or any remaining pages.

### Do not touch

- Capture backend, encoder, muxer.
- Recording coordinator / state machine.
- Source picker.
- Diagnostics backend.
- Hotkeys/Logs/About.
- Settings schema (reuse existing keys; add new ones only if needed).

### Model recommendation

**Codex xhigh** if R0 docs are precise and card-by-card specs are clear. **Claude Opus xhigh** if significant widget-creation ambiguity remains or if the consolidation of 5 pages into one requires architectural decisions.

### Implementation status — landed

- **Card IA:** `ConfigPage` reflowed into the hybrid compact card grid — full-width **Preset** card, two-column **Format & encoding | Audio** row (collapses to one column below 880px), full-width **Webcam** card, full-width **Output** card, and a collapsed **Advanced / Expert** note. The readiness banner stays at the top. The old "Preset & Format / Capture Quality / Capture Behavior / Audio Sources / Webcam Setup" titles are gone; their controls moved into the new cards with object names, signals, slots, and the single `config_page_` instance preserved.
- **Consolidation reality:** `VideoPage`/`AudioPage`/`OutputPage` were already not compiled or routed (dead code from earlier slices); only `ConfigPage` is the live Settings surface. R3 reshapes its interior rather than merging five live pages. Advanced and Webcam remain Settings **sub-pages** (reached from the cards), not top-level nav — no shell/routing change was needed.
- **Format & encoding:** container radios, video/audio codec selects (reconciliation unchanged), the compact 3-segment Quality control (hidden `videoQualityCombo` still the single emit seam), CFR + Capture cursor, and a new **read-only** Frame rate select (`frameRateCombo`, fixed 60 fps — honest, not a fake 30/120 selector).
- **Audio:** hybrid source order System → Application → Microphone; Separate-track toggles + mic device select preserved. **No** meters added in Settings (no real per-source L/R feed); the hint points to the Record dock. No fake gain/volume.
- **Webcam:** Record webcam + Camera + `Open Webcam Setup` preserved; no placement/PiP in Settings; Mirror honestly stated as not saved (`WebcamSettings` has no field); chroma stays in Webcam Setup where the real `video_thread` chroma pipeline lives.
- **Output:** folder + Browse + filename pattern + token reference preserved; added a **disabled/planned** Output-resolution segmented (Native/4K/1440p/1080p/720p — scaling not implemented) and compact filename token chips (only tokens `FilenameBuilder` resolves: `{datetime}/{date}/{time}/{app}/{title}/{target}/{profile}/{container}`).
- **Preset:** dropdown + quiet Save/Reset + Manage-presets overflow (identical menu/actions) + status badge; honest hint that webcam & source selection are saved separately (`RecordingProfile` stores output/video/audio only).
- **QSS:** added `outputResSegmented` to the segmented-container rule and a `tokenChip` label role; reused existing panel/cardTitle/fieldLabel/qualitySegment/profileStatusBadge roles.
- **Tests:** `test_config_page.cpp` updated — `HybridCardTitles_AreVisible`, `OutputResolution_IsPlannedAndDisabled`, `FrameRateControl_IsReadOnly`, `FilenameTokenChips_AreShown`; all existing object-name/behavior assertions kept. Debug build green; focused CTest 290/290; full CTest 565/565. Screenshot smoke under `.workspace/screenshots/hybrid-port-r3-settings-compact-ia/`; smoke note `.workspace/smokes/hybrid-port-r3-settings-compact-ia.md`.
- **Not touched:** Record/TransportDock/PreviewSurface/DXGI, Source picker, Diagnostics/Logs/Hotkeys/About content, capture/encoder/muxer/audio internals, recording state machine, settings schema, profile registry, build metadata, shell/top-nav.

---

## HYBRID-PORT-R4 — Source Modal + Region UX

### Scope

- In-window overlay modal pattern (translucent backdrop + centered `QFrame`, not a separate `QDialog` window).
- Source picker: Displays / Windows / Region tabs via segmented control.
- Card grid selection with thumbnails, accent border on selected, check badge.
- Region tab: 6 preset cards (Draw custom, 16:9 Landscape, 16:9 HD, 9:16 Vertical, 1:1 Square, 4:5 Portrait).
- Region overlay: drag-to-draw, corner resize, move within bounds, Escape to cancel.
- No search at current list size (add later only if Windows list genuinely needs it).
- System/helper/tool windows hidden from default Windows grid.
- Webcam WYSIWYG PiP placement/size/mirror in Record preview — marked as Later.

### Likely files/classes

**New / heavily modified:**
- `ui/dialogs/SourcePickerDialog.h/.cpp` — convert from `QDialog` to in-window overlay widget pattern; restructure tabs as segmented control.
- New: `ui/widgets/OverlayModal.h/.cpp` — reusable in-window modal base (backdrop + centered frame).
- `ui/widgets/CaptureTargetCard.h/.cpp` — update to hybrid card style (striped thumbnail, accent border, check badge).
- New: `ui/widgets/RegionSelectionOverlay.h/.cpp` — or update existing: preset grid + draw/resize interaction.
- `ui/dialogs/SourcePickerWindowRules.h` — update window filtering rules.
- New: `ui/widgets/AspectThumbnail.h/.cpp` — aspect-ratio preview card for region presets.

### Risks

- Modal pattern change (QDialog → overlay widget) may affect z-order, focus, and parent event handling.
- Region draw/resize interaction requires careful pointer event handling.
- Thumbnail generation for display/window cards must remain async and non-blocking.
- Window enumeration filtering must exclude system/noise/inaccessible windows.

### Validation

- Screenshot smoke: all three tabs (Displays, Windows, Region), region draw preview.
- Verify Escape closes modal, clicking backdrop closes modal.
- Verify region presets update the selected region rectangle.
- Verify selected source is communicated back to Record page and recording config.
- Verify no regression on existing thumbnail capture service.

### Do not touch

- Capture backend, encoder, muxer.
- Recording state machine.
- Thumbnail capture service (only consume its output).
- Settings, Diagnostics, Hotkeys, Logs, About.
- Webcam PiP drag/resize (Later).

### Model recommendation

**Claude Opus xhigh/max** — modal overlay pattern, region draw/resize interaction, and pointer event handling are architecturally sensitive.

---

## HYBRID-PORT-R5 — Diagnostics Pipeline

### Scope

- Pipeline cards: Source Capture → Frame Queue → Compositor → Encoder → Muxer → Disk.
- Each card: status (OK / Hotspot / Over), latency, queue depth, drops, throughput, colored border + status bar.
- Arrow connectors between pipeline steps.
- Bottleneck indicator above pipeline.
- Capability matrix (existing backend probes displayed in compact table).
- Pipeline telemetry sparklines (static gauges first; Qt Charts sparklines as Later).
- Recommendation card based on detected issues.
- Telemetry confined to this page — no global CPU/GPU/RAM/Disk stats.

### Pipeline metrics — MVP vs Later

**MVP:**
- Pipeline section can be static/planned if real metrics are not instrumented.
- Static/planned cards must be clearly labeled (e.g., "Pipeline metrics not yet instrumented").
- Capability list should remain real (uses existing backend probes).
- No fake precision on any metric.

**Later:**
- Live latency/queue depth/drops/throughput instrumentation.
- Sparkline charts for encoder latency and bitrate.
- Real bottleneck detection based on live data.

### Likely files/classes

**New / heavily modified:**
- `pages/DiagnosticsPage.h/.cpp` — restructure to pipeline-first layout.
- New: `ui/widgets/PipelineStepCard.h/.cpp` — individual pipeline step with status bar, key-value lines, status pill.
- New: `ui/widgets/PipelineFlow.h/.cpp` — horizontal flow with cards + arrow connectors.
- New: `ui/widgets/SparklineWidget.h/.cpp` — custom paintEvent sparkline (or Qt Charts wrapper if already linked).
- `diagnostics/CapabilitySummary.h/.cpp` — consume existing probes.
- `diagnostics/RecommendationEngine.h/.cpp` — consume existing recommendations.
- `diagnostics/DiagnosticsPresentation.h` — update presentation helpers for pipeline format.

### Risks

- Pipeline metrics may not yet have real instrumentation. Static cards must be clearly labeled (e.g., "Pipeline metrics not yet instrumented"). Never simulate live precision.
- Sparklines via Qt Charts add a dependency; custom paintEvent is lower-risk.
- Capability matrix must stay in sync with backend probes.
- Do not show fake live gauges; static/planned cards are acceptable if honestly labeled.

### Validation

- Screenshot smoke: Diagnostics at default, maximized, and scrolled.
- Verify capability checks reflect real system state.
- Verify pipeline cards render with correct status colors.
- Verify no fake precision on unimplemented metrics.
- Verify recommendation card shows actionable guidance.

### Do not touch

- Diagnostics backend probes (CapabilitySummary, SelfTestRunner, ConfigSummary).
- AppLog / error_message.
- Capture backend, encoder, muxer.
- Recording state machine.
- Settings, Record, Hotkeys, Logs, About.

### Model recommendation

**Codex xhigh** if live pipeline metrics exist and the task is primarily UI scaffolding around real data. **Claude Opus xhigh** if metrics are sparse and significant UI-only scaffolding with clear planned-state labeling is required.

### Implementation status — landed

- **Pipeline-first IA:** `DiagnosticsPage` reordered to readiness banner → **CAPTURE PIPELINE** (the page's visual center, directly below the banner) → **RECOMMENDATIONS** (the former "Top Issues", actionable cards) → **CAPABILITY MATRIX** (real probes, visible but secondary) → **Active configuration** (collapsed reference) → **Self-Test** → **Logs** redirect. The old "Pipeline · Planned note" strip and the "Technical Details" umbrella were removed.
- **New widgets:** `ui/widgets/PipelineStepCard` (name + status pill + honest note; status enum `Ok / Hotspot / Over / Planned / Unavailable`, the last two live-only values reserved for future telemetry) and `ui/widgets/PipelineFlow` (the six canonical steps **Source Capture → Frame Queue → Compositor → Encoder → Muxer → Disk** with `→` connectors, fixed order, `setStepStatus(index, status, note)`).
- **Real vs planned mapping (honest, no fakes):** steps backed by a real probe are static availability checks — **Encoder** = `QueryVideoCodec(active).level` selectable, **Muxer** = `QueryContainer(active).level` selectable, **Disk** = `SelfTestRunner::CheckOutputPathWritable()` temp-dir probe → `Ok`/`Unavailable`. Probe-less internal stages (**Source Capture / Frame Queue / Compositor**) stay `Planned` ("… is not instrumented yet"). **No** card renders numeric latency / queue depth / drops / throughput; the section meta reads `Static checks` and a caption states the status is static, not live timing. No sparklines were drawn (skipped, not faked). No generic CPU/GPU/RAM/Disk dashboard.
- **Capability matrix:** the existing real `CapabilitySummary` table (OS/GPU/NVENC/codecs/containers) was promoted out of the collapsed "Technical Details" into a visible-but-secondary `CAPABILITY MATRIX` panel; the section meta shows the live probe count. Active configuration remains a collapsed reference. All existing diagnostics checks, recommendations, and self-test rows still render.
- **Honesty fixes:** the **Export Report** button no longer enables to a no-op — it stays disabled with a "planned for a future build" tooltip. The stale "listed in Technical Details below" copy now points to the capability matrix.
- **Tests:** new `pipeline_flow_tests` (step order/canonical names, default Planned, status→pill/property wiring, no-fake-metric guard) and `diagnostics_page_tests` (page contains the `pipelineFlow` with the six steps in order, real Encoder/Muxer/Disk resolve to `Ok` on the validated baseline while probe-less stages stay `Planned`, capability rows render, Run Check yields READY, Export stays disabled) — both `TEST_PREFIX diagnostic.`. Backend `diagnostics_tests` unchanged. Debug build green; focused CTest 317/317; full CTest 587/587. Screenshot smoke `.workspace/screenshots/hybrid-port-r5-diagnostics-pipeline/`; note `.workspace/smokes/hybrid-port-r5-diagnostics-pipeline.md`.
- **Not touched:** diagnostics backend probes (`CapabilitySummary` / `SelfTestRunner` / `ConfigSummary` / `RecommendationEngine` logic), `AppLog`, capture/encoder/muxer/audio internals, recording state machine, settings schema, build metadata, shell/top-nav, Record/PreviewSurface/DXGI, Source picker, Settings IA, Logs, Hotkeys, About.

---

## HYBRID-PORT-R6 — Hotkeys / About / Logs Polish

### Scope

- Hotkeys: compact table-like layout with monospace keycap chips, active vs. planned sections, reset-to-defaults button.
- Logs: contained log viewer surface, monospace body, level coloring, All/Info/Issues filter, Copy button.
- About: clean centered dialog card with logo, wordmark, version/build/commit/author, GitHub link.
- All three surfaces match the hybrid prototype pixel reference.

### Likely files/classes

**New / heavily modified:**
- `pages/HotkeysPage.h/.cpp` — update layout to compact table rows with keycap chips.
- `pages/LogsPage.h/.cpp` — update to contained log surface with filter controls.
- `ui/dialogs/AboutDialog.h/.cpp` — update to centered card style with detail table.
- `ui/widgets/KeycapChip.h/.cpp` — monospace keycap chip widget.
- `ui/widgets/LogFilterBar.h/.cpp` — segmented filter + Copy button.

### Risks

- Hotkeys rebinding and conflict detection are marked as Later; ensure planned-state labeling is honest.
- Logs filtering requires real log level parsing from AppLog.
- About dialog must display real build metadata (version, commit, encoder capability).

### Validation

- Screenshot smoke: Hotkeys, Logs, About at default and maximized.
- Verify log level colors match hybrid token set.
- Verify About dialog shows real version/commit from build metadata.
- Verify Hotkeys keycap chips use IBM Plex Mono.

### Do not touch

- Hotkeys backend / global shortcut registration.
- AppLog / logging infrastructure.
- Build system / version metadata generation.
- Capture backend, encoder, muxer.
- Settings, Record, Diagnostics.

### Model recommendation

**Codex high/xhigh** — three independent UI surfaces, each well-defined in the prototype, with minimal architectural risk.

### Implementation status — landed

- **Hotkeys:** `HotkeysPage` reflowed into compact table-like rows inside two `panel` frames —
  `ACTIVE HOTKEYS` (Available now) and `PLANNED / UNAVAILABLE` (Not in this build) — with hairline
  row separators, no oversized cards. Bindings render via the new `ui/widgets/KeycapChip`
  (IBM-Plex-Mono keycaps, subtle border + bottom-edge shadow, joined by dim `+` labels; muted
  `Unset` chip when empty). Active rows keep the genuinely-supported Set / Unset rebind
  (`QKeySequenceEdit`) plus a new `Reset to defaults` button; planned rows expose **no**
  rebind/keycap controls, only an honest `Not in this build` tag. An honest footnote states
  shortcuts register globally and that ExoSnap does **not** detect conflicts. Only the four
  backend-modelled actions are shown — Start/Stop + Pause/Resume are the real WM_HOTKEY-wired
  ones; Split + Mute Mic are presented as planned (registered but no live handler). No new global
  hotkey backend, rebinding-where-unsupported, or fake conflict detection was added.
- **Logs:** `LogsPage` keeps the contained dark `logViewer` (monospace, no-wrap) and gains a
  `Copy` action (copies the visible tail; disabled when empty) alongside Refresh / Open Log
  Folder, plus a footer note pointing at `%LOCALAPPDATA%\ExoSnap\logs`. **No** All/Info/Issues
  filter was added: `AppLog` lines carry component tags (`[record]`, `[dxgi-preview]`, …) not
  severity levels, and the design target lists level filters as *Later* — so honest, no fake
  filtering. Guarded by a unit test.
- **About:** `AboutDialog` rebuilt as a clean centered card — aperture `BrandMarkWidget` +
  two-tone lowercase `exosnap` wordmark + `Version … · for Windows` line, description, and a
  metadata table with hairline rows (VERSION / BUILD / COMMIT / AUTHOR) from the generated
  `ExoSnapBuildInfo.h` / `EXOSNAP_BUILD_CONFIG`. Real actions only: `GitHub` opens the configured
  repo URL (`https://github.com/Exoridus/exosnap`, from the git remote); `Copy details` copies the
  real metadata; `Close` (primary). No Release-notes action (no published feed), no fake Encoder
  capability row, no Copy-diagnostics no-op. QDialog keeps its native title bar but the inner
  layout is a single clean panel (no debug-card double-frame). About routing from the top nav is
  unchanged.
- **QSS:** new `keycap` / `keycap` `[stateRole=muted]` / `keycapPlus` / `hotkeyAction` /
  `hotkeyActionPlanned` roles and `aboutWordmark` / `aboutVersionLine` roles; existing
  `plannedTag`, `panel`, `sectionRuleLine`, `note`, button-role styles reused.
- **Tests:** `test_hotkeys_page` updated (active keycaps render real bindings, unset shows muted
  keycap, planned rows expose no fake controls, reset restores defaults); new `test_logs_page`
  (contained read-only no-wrap viewer + Refresh/Copy/Open-folder, no fake level filters, Copy
  disabled when empty) and `test_about_dialog` (real Version/Build/Commit/Author, GitHub uses the
  configured repo URL, no fake Release-notes action). Debug build green; focused CTest 329/329;
  full CTest 596/596. Screenshot smoke `.workspace/screenshots/hybrid-port-r6-utility-polish/`;
  note `.workspace/smokes/hybrid-port-r6-utility-polish.md`.
- **Not touched:** Record dock / PreviewSurface / DXGI, Source picker / Region model, Settings IA,
  Diagnostics backend/pipeline, capture/encoder/muxer/audio internals, recording state machine,
  settings schema, build-metadata generation, shell/top navigation (About routing unchanged),
  global hotkey registration backend, `AppLog` infrastructure.

---

## HYBRID-ABOUT-INLINE-R1 — Inline About Overlay + Saved Status Scope

Follow-up shell/About polish resolving the two HYBRID-VERIFY-R1 findings. Supersedes the
R6 note that "About `QDialog` keeps its native title bar / About routing is unchanged".

### Implementation status — landed

- **Inline About (Part A):** the separate native About `QDialog` is removed; About is now an
  in-window overlay (`ui/dialogs/AboutOverlay`) — a translucent backdrop (painted in code) with
  a centered `#aboutCard`. It is parented to the page **stack**, so the title bar / window
  controls stay usable and correctly painted (no semi-transparent sibling shine-through on the
  chrome). Qt manages native-window z-order when the alien overlay is `raise()`d above the native
  `PreviewSurface`, so it renders cleanly over the Record page (the DXGI preview yields behind the
  card; `PreviewSurface`/DXGI untouched). The top-nav About action opens it; Close / `Escape` /
  backdrop-click / any nav switch dismiss it; `navigateToPage()` closes it so it never lingers
  over an unrelated page. No native title bar, no focus-sticky child window after closing. The R6
  card content/actions are retained verbatim (aperture mark, two-tone wordmark, Version line,
  description, real VERSION/BUILD/COMMIT/AUTHOR table, `GitHub` configured-URL-only, `Copy
  details`, `Close`).
- **Saved status scope (Part B):** the title-bar `Saved` pill is scoped to the Record page via a
  pure helper `ui::chrome::ScopeStatusLabelForActivePage(label, on_record_page)` (in
  `RecordingStatusGuards.h`) applied by `MainWindow::applyTitleBarStatus()` on every page switch
  and chrome-state change. `SAVED` is normalized to `READY` on any non-Record page; every other
  status stays global. `record_status_label_` is unchanged, so returning to Record restores
  `Saved` while the result dock is visible. No recorder coordinator / state-machine change.
- **Tests:** `about_overlay_tests` (renders in-window not as a `QDialog`, real metadata,
  configured GitHub URL, no fake Release notes, open/close + `closed()` signal) replaces
  `about_dialog_tests`; `operational_title_bar_tests` gains the Saved-scope cases. Debug build
  green; focused CTest 330/330; full CTest 602/602. Screenshots
  `.workspace/screenshots/hybrid-about-inline-r1/`; smoke note
  `.workspace/smokes/hybrid-about-inline-r1.md`.
- **Not touched:** Record transport dock layout, `PreviewSurface`/DXGI, Source picker / Region
  model, Settings IA, Diagnostics pipeline/backend, Logs/Hotkeys content, capture/encoder/muxer/
  audio internals, recorder state machine, settings schema, build-metadata generation.

---

## Phase Summary

| Phase | Name | Priority | Effort | Model |
|---|---|---|---|---|
| R1A | Tokens / Fonts / Accent Foundation | MVP | Medium | Claude Opus xhigh |
| R1B | Shell / Top Nav / Titlebar | MVP | High | Claude Opus xhigh |
| R2 | Record Preview + Stable Bottom Dock | MVP | High | Claude Opus xhigh/max |
| R3 | Settings Compact IA | MVP | High | Codex xhigh if docs/spec are precise; Claude Opus xhigh if layout interpretation is needed |
| R4 | Source Modal + Region UX | MVP | High | Claude Opus xhigh/max |
| R5 | Diagnostics Pipeline | MVP | Medium | Codex xhigh if real metrics exist; Claude Opus xhigh if UI scaffolding/planned-state labeling is needed |
| R6 | Hotkeys / About / Logs Polish | MVP | Medium | Codex high/xhigh

---

## Cross-cutting rules for all phases

- Do not touch: capture backend, encoder, muxer, WASAPI audio pipeline, recording state machine internals, settings schema (add fields only if needed), build metadata generation.
- Every phase must pass screenshot smoke at default and maximized window sizes.
- Every phase must pass existing unit tests; fix only tests broken by removed/renamed widgets.
- If a metric is not real in the current slice, label it clearly as planned/static. Never simulate live precision.
- UI-only phases may use simulated placeholder data but must not claim live instrumentation.
- Commit one phase at a time; do not mix phases in a single commit.
