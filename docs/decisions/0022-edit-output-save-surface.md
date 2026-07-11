# ADR 0022: Edit / Output / Save Surface (Overlay Over Record)

## Status

Accepted — UI shell implemented in Production Suite wave; engine implemented in 0.9.0 wave
(Review post-flight report, Edit keyframe-accurate trim, Output real stream-copy export).

**Superseded surface shape (EDIT-OVERLAY-R1, UI-redesign-port wave):** the surface was
originally an in-window *stack page* (Record → EditExportPage swap, Back returns to Record).
Per the redesign IA (`suite-ia.jsx` decision 5 / `suite-edit.jsx`: "an overlay over Record, not
a nav tab"), it is now an **overlay over the Record page** — `EditExportOverlay` hosts the same,
functionally-unchanged `EditExportPage` instead of swapping it into the `QStackedWidget`. See
"Surface shape (current): overlay" below; the original "In-window mode" rationale is kept
for history but no longer describes the shipped shape.

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
client area, with a dimmed backdrop painted in `paintEvent`. This parenting is required —
verified empirically — for the backdrop to correctly composite over the native DXGI live-preview
child window (Qt promotes the overlapping sibling to its own native window).

Consequences of the shape change vs. the original in-window mode:
- The main nav (including window controls) is covered by the overlay's backdrop while it is
  open — same as when `SourcePickerOverlay` is open. This is a deliberate trade vs. the original
  ADR text ("nav items remain accessible"): the surface is now a full-focus overlay, not a page.
- `EditExportPage`'s own internals (phases, trim, output, markers, export) are **unchanged** —
  this is a re-hosting change only. All existing object names (`editExportProgressBar`,
  `editFactDuration`, `editExportPrimaryBtn`, `editExportBackBtn`, …) are preserved.
- The overlay is built lazily on first use (`MainWindow::buildEditExportOverlay()`), same timing
  as the former `buildEditExportPage()` in the staged post-show hydration sequence.

### Dismiss rules

Escape or a click on the backdrop (outside the hosted page's framed panel) closes the overlay in
every phase **except** `Phase::Exporting`, where a running export must not be silently abandoned
— only the page's own controls (Cancel / Retry) can end that flow
(`EditExportOverlay::isDismissBlocked()`). Navigating to a different top-level page while the
overlay is open dismisses it for the same reason and under the same exception (a nav click
during an active export is ignored, not queued).

Not-yet-exported trim edits are discarded on dismiss without a confirmation prompt. This mirrors
the page's pre-existing Back-button behavior (`onBackClicked()`/`onDoneClicked()` have never
guarded against unsaved trim state) — introducing a confirmation only for Escape/backdrop would
be inconsistent with Back already discarding silently. Markers are unaffected: the edit surface
is a marker *consumer* only (markers are placed during recording), so nothing already-committed
is at risk.

**Recording start dismisses the overlay.** A capture start (recording or countdown) closes the
Edit overlay — on the stack-page shape this happened implicitly via the swap-back to Record;
with the overlay it is explicit in `onRecordChromeStateChanged`. This dismissal deliberately
applies **during `Phase::Exporting` too**: closing the overlay only hides the progress UI — the
hosted page and its export worker thread live on, and re-entering the editor after Stop re-shows
the running export instead of resetting the page (`navigateToEditExportPage` re-opens without
touching the context while `isDismissBlocked()`). Conversely, `navigateToEditExportPage` is a
no-op while a recording or countdown is active, so a stale toast's Edit action can never open the
editor over a live capture.

**App close during an export.** `MainWindow::closeEvent` guards `Phase::Exporting` the same way
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

**Known interaction trade:** while the overlay is open it also covers the custom title bar, so
the window cannot be moved/minimized with the mouse during an edit session (including during
`Exporting`, where Escape/backdrop dismiss is blocked). Alt+Space / Win+Arrow keyboard window
management still works; accepted as consistent with the SourcePickerOverlay precedent.

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

`EditExportPage` replaces the main content area after recording stops. The surface has three linear
phases stepped by a top stepper bar:

```
Review → Edit → Output
```

- **Review**: post-flight report (frame-drop %, peak A/V drift, pipeline health) populated from
  `RecordingDiagnosticsSnapshot` and peak-drift tracking in `RecordPage`. Video player
  placeholder (decoded frames deferred to 0.11; the play/pause toggle drives a real position
  clock). Duration / size / codec / container Details card on the right (right-aligned mono
  values).
- **Edit**: direct manipulation on the timeline under the player (`app/ui/widgets/EditTimeline`):
  draggable trim in/out handles (trimmed-away ranges dimmed; keyframe-accurate — see "Trim
  implementation"), marker verticals, and a scrubbable playhead that follows the preview clock.
  There is no button row above the strip. Split Chapter deferred to 0.11. The primary action
  ("Save & export" in Output) sits in a bottom action bar, bottom-right like the Record page's
  transport actions.
- **Output**: container combo (MKV / MP4, both stream-copy / lossless) + save-mode combo
  (new file = `<name>_edit.<ext>` / overwrite original = atomic rename). After clicking Export:
  Exporting phase (real progress bar from `RemuxProgressCallback`) → Done or Failed result panel.

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
began (paused stays paused). Reaching the end of the clip pauses at the end. Hiding the page (or
leaving the player phases) stops the clock.

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
- An overlay (vs. a stack page) makes the "transient, over Record" relationship visible in the
  UI itself: the Record page is still there underneath, dimmed, not navigated away from.
- Back / Done / dismissing the overlay all resolve to the same `closeOverlay()`, cleanly, without
  touching nav history (there was never any to pollute, but the overlay makes this structurally
  true rather than just behaviorally true).

### Phase stepper

The three-step stepper (Review / Edit / Output) dynamically highlights the current phase:
- Review phase → "Review" highlighted (accent underline).
- Edit phase → "Edit" highlighted.
- Output / Exporting / Done / Failed → "Output" highlighted.

(Unchanged by EDIT-OVERLAY-R1 — this is internal to `EditExportPage`, which was re-hosted, not
rewritten.)

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
