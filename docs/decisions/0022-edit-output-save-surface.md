# ADR 0022: Edit / Output / Save Surface (Overlay Over Record)

## Status

Accepted — UI shell implemented in Production Suite wave; engine implemented in 0.9.0 wave
(post-flight report, keyframe-accurate trim, real stream-copy export).

**Superseded surface shape (EDIT-OVERLAY-R1, UI-redesign-port wave):** the surface was
originally an in-window *stack page* (Record → EditExportPage swap, Back returns to Record).
Per the redesign IA (`suite-ia.jsx` decision 5 / `suite-edit.jsx`: "an overlay over Record, not
a nav tab"), it is now an **overlay over the Record page** — `EditExportOverlay` hosts
`EditExportPage` instead of swapping it into the `QStackedWidget`. See
"Surface shape (current): overlay" below; the original "In-window mode" rationale is kept
for history but no longer describes the shipped shape.

**Amended (UI-polish round): opaque backdrop.** The overlay's backdrop was originally
semi-transparent (the SourcePickerOverlay dim, alpha 158), leaving the Record page dimly
visible underneath. It is now painted **fully opaque**. The original justification was that any
alpha below 255 let the window chrome bleed through the margin band; with the title bar no
longer covered (see below) that reason is spent, but a translucent backdrop would show the
Record page underneath, which is not wanted either. The "Record page dimly visible underneath"
property is deliberately given up — the transient over-Record relationship is carried by the
overlay's structure (parenting, dismiss behavior), not by see-through pixels. Passages below
that described the backdrop as "dimmed" have been updated accordingly.

**Amended (2026-08-02): one view instead of a three-step stepper.** The `Review → Edit →
Output` stepper is gone, and with it `EditExportPage::Phase`. The surface is a single view —
player, trim timeline, details rail, post-flight report icon — with the output choice and the
export progress/result in a nested **export card** over it. The overlay also no longer covers
the real title bar. See "Surface structure" and "Export card" below; the sections describing
the phases and the stepper have been replaced.

**Amended (2026-08-03): the export card becomes a rail panel, and the surface scales.** The
nested export card is gone (`ui::dialogs::ExportOverlay` deleted). Container, save mode,
destination, progress and result now live in an **embedded panel in the right rail**, under the
details card, and the action bar's button starts the export directly instead of opening
anything. The overwrite confirmation is the only modal left in the flow. The rail also stops
being a fixed 280 px: it narrows across width breakpoints (but is never hidden — it carries the
export controls) and scrolls vertically, and the page recomputes that layout from `resizeEvent`
and `showEvent`. See "Surface structure" and "Export panel" below.

**Amended (2026-08-03, second pass): the rail never auto-scrolls, and it densifies instead.** The
first pass answered "the status is below the fold at 860 × 700" by scrolling the rail to the
panel's bottom edge when a run reported something (`ExportPanel::statusShown` →
`EditExportPage::revealExportPanel()`). At the minimum window the panel is taller than the
viewport, so that scroll was a jump by construction, it landed each of the three states at a
different offset, and it moved the details card along with it. Both the signal and the method are
**gone**: the status area now sits at the *top* of the export card, directly under its heading and
above the output rows, so there is nothing to scroll to. The rail's scroll position is left to the
user in every state. Alongside it the details card gained a **compact density** for the narrow
breakpoint only, freeing roughly 50 px for the export card at the minimum window. See "Export
panel" and "Surface structure" below.

**Amended (2026-08-12, final visual polish): workspace presentation, unchanged ownership.** The
Qt Quick frontend's `EditOverlay` is still a layer over the Record page and still owns the clip,
the decoder session and the export exactly as decided here — **no page migration happened, and
none is claimed.** What changed is presentation. The Quick port had regressed the shape this ADR
already settled: it covered the *whole* window with a scrim, a 20 px floating gap and a rounded,
bordered frame nearly the size of the window, which read as a modal dialog and put the shell's own
minimize/maximize/close buttons underneath it. It now occupies the normal page content region below
the 40 px title band — the same "title bar stays visible" property the Widgets implementation had —
with no scrim, no gap, no outer frame and no outer border.

Two behavioural notes, so this file does not stay wrong about the shipped build:

- **Nav tabs stay available while the workspace is open** (corrected 2026-08-16, QCR-001), with
  Record still marked as the current destination. Three shapes have existed and only the third is
  shipped: the Widgets shape closed the overlay through the discard guard on nav-away; the Quick
  port locked the tabs instead; the shipped shape does neither. An open edit session is state of
  the Record destination — leaving Record hides the workspace without ending the session, and
  returning shows the same one. Navigation is unconditional and asks nothing; Back is still the
  only way out of the session, and it still guards unsaved trim points and markers. The nav-away
  paragraphs under "Decision" below describe the Widgets shape and are kept for history.
  Normative behaviour: `docs/product-spec.md` §2.
- **The Done state offers one folder action.** `Open folder` and `Show in Explorer` were two labels
  for one user task (the second opens the same folder *and* selects the file); the panel now offers
  `Show in folder` alone. The "Export panel" section below still describes the pair and is kept for
  history.

## Context

After a recording stops, users need to decide what to do with the captured file: keep the MKV
as-is, remux it to MP4 (stream-copy, no quality loss — ADR 0014), or review and optionally trim
it before exporting.

Several surface-shape options were considered:

- **Separate native dialog** — focus-management risk, cannot reuse the in-window nav shell.
- **New top-level tab** — inflates the nav to 6 items again; clashes with the 5-item target.
- **Overlay / modal** — originally rejected as "too cramped for a full review/edit workflow";
  revisited in EDIT-OVERLAY-R1 once the surface fills nearly the full overlay (a framed panel,
  not a small centered dialog) — the "cramped" concern does not apply to a full-size overlay.
  This is the **currently chosen shape** (see "Surface shape (current): overlay" below).
- **In-window mode (stack replacement)** — the main content stack switched to the Edit/Export
  surface; Back returned to Record; nav items stayed accessible but the transport dock was
  hidden. This was the shape shipped in the Production Suite / 0.9.0 waves; superseded by the
  overlay shape in EDIT-OVERLAY-R1.

## Decision

### Surface shape (current): overlay over Record

`EditExportOverlay` (`app/ui/dialogs/EditExportOverlay.{h,cpp}`) hosts `EditExportPage` as a
child widget instead of adding it to the main `QStackedWidget`. It follows the exact technical
recipe already established by `SourcePickerOverlay`: a plain `QWidget` parented to the
`MainWindow` central widget (a sibling of the title bar and the page stack), sized to the full
client area, with an opaque backdrop painted in `paintEvent` (originally a semi-transparent
dim; amended — see "Amended: opaque backdrop" in Status). This parenting is required —
verified empirically — for the backdrop to correctly composite over the native DXGI live-preview
child window (Qt promotes the overlapping sibling to its own native window).

Consequences of the shape change vs. the original in-window mode:
- The overlay spans the client area **minus a top inset equal to the title bar height**
  (`EditExportOverlay::setTopInset()`, pushed in by MainWindow and re-applied on every parent
  resize). Nav tabs and window controls stay visible and clickable for the whole edit session;
  navigating away closes the overlay through the discard guard below.
- The overlay is built lazily on first use (`MainWindow::buildEditExportOverlay()`), same timing
  as the former `buildEditExportPage()` in the staged post-show hydration sequence.

### Dismiss rules

Escape or a click on the backdrop (outside the hosted page's framed panel) closes the overlay
**except while an export runs**, where it must not be silently abandoned — only the export
card's own Cancel can end that flow (`EditExportOverlay::isDismissBlocked()`, which reads
`EditExportPage::isExportRunning()`). Navigating to a different top-level page while the overlay
is open dismisses it for the same reason and under the same exception (a nav click during an
active export is ignored, not queued).

**Discard guard.** With the title bar and the nav tabs reachable there are more ways to leave the
surface, so closing it with trim points or markers set asks first: `Discard edits?`, with
`Keep editing` as the default button and `Discard` as the reject-role choice — the same shape as
the overwrite confirmation. `EditExportPage::hasUnsavedEdits()` is the gate (a trim range is set,
or the clip carries markers). It guards the back arrow, Escape, a backdrop click, and nav-away;
on `Keep editing` from nav-away the navigation is cancelled outright rather than deferred.

**Recording start dismisses the overlay.** A capture start (recording or countdown) closes the
Edit overlay — on the stack-page shape this happened implicitly via the swap-back to Record;
with the overlay it is explicit in `onRecordChromeStateChanged`. This dismissal deliberately
applies **during a running export too**: closing the overlay only hides the progress UI — the
hosted page and its export worker thread live on, and re-entering the editor after Stop re-shows
the running export instead of resetting the page (`navigateToEditExportPage` re-opens without
touching the context while `isDismissBlocked()`). It is also the one close that **skips the
discard guard**: a modal that blocks a hotkey-triggered recording is worse than a lost trim.
Conversely, `navigateToEditExportPage` is a no-op while a recording or countdown is active, so a
stale toast's Edit action can never open the editor over a live capture.

**App close during an export.** `MainWindow::closeEvent` guards a running export the same way
it guards the post-stop MP4 remux: a dialog offers "Wait for export to finish" (default) or
"Cancel export and close". Cancelling is data-safe — an export never mutates the original
recording; at most a partial temp/`_edit` file is abandoned. The close-to-tray path is unaffected
(hide-to-tray leaves the export running).

**Mic privacy while the overlay is open.** The overlay covers the Record page without hiding it,
so RecordPage's `hideEvent` privacy gating (which stops the visibility-gated mic meter and with
it the Windows microphone-in-use indicator when navigating away) never fires. The overlay's
`opened()`/`closed()` signals therefore drive `RecordPage::setEditOverlayActive()`, which
suspends/resumes the visibility-gated meter monitoring explicitly. A mic that is enabled as a
recording source is unaffected — identical semantics to navigating to another page.

**Title bar stays reachable.** The overlay originally covered the custom title bar, so the
window could not be moved or minimized with the mouse for the whole edit session — including
during an export, where Escape/backdrop dismiss is blocked. That trade is reversed: the overlay
now starts below the title bar (`setTopInset()`), and window management works normally
throughout. The SourcePickerOverlay precedent still applies to that overlay, which is a short
modal choice rather than a session-length surface.

### Entry points (current)

- **Post-stop "Edit" button** (`RecordPage` result panel) — unchanged: builds the full
  `EditContext` and emits `editExportRequested`.
- **Notification toast "Edit" action** — unchanged: builds the path-only fallback `EditContext`.
- **Recent recordings menu** (`RecordPage`'s "Recent" button, next to "Change source") — new in
  EDIT-OVERLAY-R1. Each recent recording gets two actions: the pre-existing "open externally"
  behavior (`onRecentItemOpen`, previously present but unwired to any UI) and a new "Edit" action
  (`onRecentItemEdit`) that builds a fallback `EditContext` from history metadata (shared
  `MakeEditContext` helper with the result-button path; no live diagnostics) and emits
  `editExportRequested`. The Edit action uses the same editability gate as the post-stop result
  button (`CanOpenInEditor`: file exists AND not multi-segment — split recordings have no single
  edit master), with a shared explanatory tooltip when disabled. The Recent button itself is
  disabled while the history is empty and hidden while the source is locked; a menu left open at
  recording start is closed by the lock update so its actions cannot fire into a live capture.

All three entry points call `MainWindow::navigateToEditExportPage()`, which now activates Record
(if not already active) and opens `edit_export_overlay_` instead of swapping the main stack.

### Surface structure

The surface is **one view** — trimming a clip and writing it out is a single task, and stepping
through it made a re-layout look like a decision. It carries:

- **Header** — back arrow, title, filename, and the post-flight report at its right end (see
  below). Always visible.
- **Player** — decoded video with a centred play/pause toggle. No longer gated behind a step; it
  is present from the moment the surface opens.
- **Timeline** — direct manipulation (`app/ui/widgets/EditTimeline`): draggable trim in/out
  handles (trimmed-away ranges dimmed; keyframe-accurate — see "Trim implementation"), marker
  verticals, and a scrubbable playhead that follows the preview clock. There is no button row
  above the strip. Split Chapter deferred to 0.11.
- **Right rail** — a scrollable column carrying two cards: `app/ui/widgets/EditDetailsRail`
  (duration / size / resolution / frame rate / video / audio / container as right-aligned mono
  values) and the export panel below it. It scrolls because the two cards together outgrow the
  column at the 700 px minimum window height, and a clipped result action would be unreachable.
  Scrolling is the *user's*: the surface never scrolls the rail on its own (see the 2026-08-03
  second-pass amendment). The details card has two densities — `EditDetailsRail::setCompact()`,
  driven from `updateResponsiveLayout()` and enabled only at the narrow rail breakpoint. Compact
  trims the card's vertical padding, the fact rows' padding and the title gap, and keeps rule lines
  only between fact groups (duration/size · resolution/frame rate · video/audio/container). It
  drops no fact and shrinks no type — the seven facts are worth their space in a tall window and
  not at the enforced minimum, where they left the export panel with almost no usable height.
- **Action bar** — one button, `Export`, bottom-right like the Record page's transport actions.
  It starts the export against the panel's current settings; it is disabled while a run is in
  flight, since a second `runExport()` would block the UI thread joining the first worker.

**Responsive layout.** `EditExportPage::updateResponsiveLayout()` runs from `resizeEvent`, from
`showEvent`, and once more deferred by a zero-timer — the same pattern `RecordPage` uses, and for
the same reason: a surface that is hidden while the window is resized receives no resize event and
would otherwise re-open with a stale layout. The rail width follows the page width (320 px at
≥ 1180, 280 px at ≥ 960, 240 px below), measured against the page, i.e. the client area minus the
overlay's 20 px margin band per side. The rail is **never** collapsed away: unlike a purely
informational sidebar it carries the export controls, and the surface has to stay fully usable at
the enforced 860 × 700 minimum window.

**Post-flight report.** The three numbers the Review step used to show — real frame drops, peak
A/V drift, pipeline health — live in an icon at the right end of the header (`editReportIcon`),
with the values in its tooltip. The computation is unchanged, including the em-dash treatment for
unavailable values and the real-versus-benign drop definition. A hover-only tooltip would swallow
a genuine finding, so the icon carries the severity: a muted `info` glyph for Good/Unavailable, an
amber warning triangle plus a short label for Warning, coral for Critical.

### Export panel

`app/ui/widgets/ExportPanel` is an embedded card in the right rail, under the details card. Two
combo boxes and a static destination line never justified covering the clip being exported with a
modal, and carrying the progress and the result in that same modal hid the clip for the whole run;
the rail had the vertical room standing empty. Four states:

```
Options ──(action bar)──> Running ──┬── ok ──> Done
                                    └── err ─> Failed ──Retry──> Running
```

Order inside the card is **title → status → output rows**. The status area is the part that
changes, so it sits where the card is anchored: at 860 × 700 it is then readable without scrolling,
and the output rows are what scrolls out of view instead. A `QBoxLayout` skips hidden items and the
spacing around them, so the resting card reserves no empty band where the status will appear.

- **Output rows** — container combo (MKV / MP4, both stream-copy / lossless), save-mode combo
  (new file = `<name>_edit.<ext>` / overwrite original = atomic rename), and a two-line destination
  statement (`Lossless stream copy` + what the selected mode does with the file). It is set in the
  **muted** text step rather than the dimmest one: it states what pressing Export does to the
  user's file, which is a different weight of information from ordinary help text, and as a single
  running sentence it was the one thing in the rail that ran out of column. These rows are present
  in **every** state and only *disabled* while a run is in flight, so nothing swaps out from under
  the pointer mid-export and the settings are already in place for the next run.
- **Running** — status line, real progress from `RemuxProgressCallback`, `Cancel`.
- **Done** — output filename, `Open folder` / `Show in Explorer`. The two actions stay stacked (a
  240 px rail cannot hold them side by side without eliding one away) but at a reduced height and
  gap, so Done is not conspicuously taller than the other three states.
- **Failed** — the remuxer's own error text, `Retry`.

The panel carries **no Export button of its own**: the trigger is the action bar's, so that two
equal-weight Export buttons a hand's width apart never make the choice ambiguous. `Retry` is the
exception, and it repeats a run rather than starting a fresh one.

The panel is presentation only. Export execution — thread, trim range, marker sidecar, atomic
rename (`EditExportPage::runExport()`) — stays in the page, which drives the panel through its
`showRunning()` / `setProgress()` / `showDone()` / `showFailed()` slots and reacts to its signals.
The overwrite confirmation still runs before the export starts and is now the **only** modal in
this flow.

Dismissing the surface leaves the panel as it is: a run still in flight behind the dismissed
overlay must be found in the state it was in when the editor is re-entered (see "Recording start
dismisses the overlay" above). Handing the surface a different clip resets it.

### Format / cost model

All export paths are stream-copy only. Re-encode is explicitly out of scope for the MVP.

| Option       | Mechanism    | Quality   | Speed    |
|--------------|-------------|-----------|----------|
| MKV          | stream-copy  | lossless  | instant  |
| MP4          | stream-copy  | lossless  | instant  |

Stream-copy uses `RemuxToMkv` / `RemuxToProgressiveMp4` (ADR 0014 path, libavformat).

### MKV edit master retention

When the recording output is MP4 (remux-on-stop path), `RecordingCoordinator` renames the
transient MKV to `<stem>.edit.mkv` instead of deleting it on remux success. This companion file
is the edit master: it is used as the `input_path` for all subsequent trim/remux operations from
`EditExportPage`. The path is forwarded to the UI as `UiRecordingResult::mkv_master_path` and
stored in `EditContext::mkv_master_path`.

For recordings where MKV is the primary output, `mkv_master_path` equals `output_path`.

The `.edit.mkv` file is not surfaced to the user in the file browser (it is a companion file,
not a final output). It remains on disk until the user explicitly deletes the recording or it is
cleaned up by a future management UI.

### Keyframe interval setting

A "Keyframe interval" selector is exposed in Settings → Advanced → Video:

| Value | GOP / IDR period | Trim grid |
|-------|-----------------|-----------|
| 2 s (default) | 120 frames at 60 fps | 2-second accuracy |
| 1 s            | 60 frames            | 1-second accuracy |
| 0.5 s          | 30 frames            | 0.5-second accuracy |

The setting maps to `RecorderConfig::keyframe_interval_secs` → `SetKeyframeIntervalSecs()` on
the NVENC encoder. Shorter intervals increase file size slightly and are recommended for users
who clip frequently.

### Trim implementation

Trim is set by dragging the timeline's in/out handles (the former spin-box dialog is gone). The
handles constrain each other through pure clamp logic (`app/models/EditTimelineModel.h`,
`kMinTrimGapMs`) so they can never cross. While a handle (or the playhead) is dragged it scales
slightly and a centred `MM:SS.mmm` time label (hours only for recordings >= 1 h) renders above it.

On handle release, the cut point snaps: `ExtractKeyframeTimestamps` loads all video keyframe PTS
values from the MKV master, the value snaps to the nearest keyframe at or before the requested
time, then to the nearest marker within 50 ms if any markers exist. The snapped value is written
back to the handle so the UI shows the real cut. The resulting `TrimRange` is applied only when
the user clicks Save & export, passed to the trimmed overloads of `RemuxToProgressiveMp4` /
`RemuxToMkv`.

Trim is keyframe-accurate and lossless: no decoding occurs.

### Preview playback clock and scrubbing

The player area's play/pause toggle drives a position clock (`QTimer`-advanced, no decoded frames
yet — the 0.11 frame view will attach to the same position). The timeline playhead follows the
clock. Scrubbing (dragging the playhead or pressing on the track) seeks the position; playback
pauses for the duration of the drag and resumes on release only if it was playing when the scrub
began (paused stays paused). Reaching the end of the clip pauses at the end. Hiding the page
stops the clock.

### Marker sidecar — single canonical format and writer

Recording markers are persisted as a JSON sidecar file (`<media>.markers.json`) alongside the
output file. The format is:

```json
{
  "version": 1,
  "media": "<media filename>",
  "timebase": "milliseconds",
  "segmentIndex": 0,
  "markers": [
    { "timeMs": 1234, "type": "general|cut|highlight", "label": "..." }
  ]
}
```

(`media` is omitted when empty; `segmentIndex` is present only for per-segment split sidecars.)

There is exactly **one** serializer and **one** path convention, shared by both the engine-side
producer and the edit-surface consumer. The (de)serialization lives in `app/models/MarkerSidecar.h`
(`WriteMarkerSidecar` / `ReadMarkerSidecar` / `SerializeMarkerSidecar` / `ParseMarkerSidecar`).

- `RecordingCoordinator` is the in-recording producer: it accumulates markers via `AddMarker()`
  (dropped during capture by the `AddMarker` hotkey or the transport-dock button), writes the
  sidecar on every `AddMarker()`, again on stop for single-file recordings, and per segment
  (partitioned + rebased to segment-local time) for split recordings — all through the shared writer.
- `EditExportPage` is a **consumer**: `loadMarkers()` reads the existing sidecar at
  `EditContext::marker_sidecar_path` (authoritative once present; falls back to the markers carried
  in the result) and renders the markers on the timeline. Markers are not added or edited on the
  edit surface. Trim cut points snap to these markers (within 50 ms).
- An **export** writes a retimed sidecar next to the exported file — only when markers survive the
  trim — via the same serializer (`PlanMarkerSidecarForExport` / `ApplyMarkerExportPlan`). See ADR
  0042 for the rules (retiming, half-open survival window, stale-sidecar removal).

Markers are never written as container chapters (no metadata change to the video file; ADR 0042).

### IA decision: overlay, not tab (superseded: was "mode, not tab")

The surface is an overlay over Record (EDIT-OVERLAY-R1), not a new nav tab — and, as of
EDIT-OVERLAY-R1, no longer a stack-replacement mode either. Rationale (still holds for "not a
tab"; updated for "overlay" vs. the original "mode/stack replacement"):
- Keeps top-level nav at 5 items (Record · Settings · Diagnostics · Logs · About).
- The edit workflow is linear and transient — users enter it, decide, and leave.
- Adding a persistent tab implies the surface is always accessible, which is premature.
- An overlay (vs. a stack page) keeps the "transient, over Record" relationship structural:
  the Record page is still there underneath, not navigated away from, and dismissing lands
  back on it directly. (Originally the backdrop was also semi-transparent so Record stayed
  dimly *visible*; that pixel-level cue was deliberately given up when the backdrop became
  opaque — any alpha below 255 lets the window chrome bleed through. See the Status amendment.)
- Back / Done / dismissing the overlay all resolve to the same `closeOverlay()`, cleanly, without
  touching nav history (there was never any to pollute, but the overlay makes this structurally
  true rather than just behaviorally true).

## Consequences

- `EditExportPage` couples to `recorder_core` (for `mp4_remuxer.h`, `pipeline_diagnostics.h`).
  The `edit_export_page_tests` and `edit_export_overlay_tests` CMake targets therefore link
  `recorder_core`.
- Export runs on a `std::thread`; results are marshalled back to the UI thread via
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.
- Atomic overwrite: write to `.tmp`, then `std::filesystem::rename` temp → final.
- `EditExportPage` is **not** in the main window stack (`QStackedWidget`) — it is hosted inside
  `EditExportOverlay`, itself a sibling of the stack and the title bar, parented to the
  `MainWindow` central widget (see "Surface shape (current): overlay" above).

## Forward

- 0.11: decoded video frames in the player area (attaching to the existing preview position
  clock), Split Chapter button.
- Post-1.0: transcription (optional, capability-gated), batch export, GIF/WebP export.
