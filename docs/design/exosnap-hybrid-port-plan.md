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
- Verify countdown works (dismissed if not yet wired end-to-end; clearly labeled as planned).
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
