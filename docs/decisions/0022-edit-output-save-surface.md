# ADR 0022: Edit / Output / Save Surface (Post-Stop In-Window Mode)

## Status

Accepted — UI shell implemented in Production Suite wave; engine implemented in 0.9.0 wave
(Review post-flight report, Edit keyframe-accurate trim, Output real stream-copy export).

## Context

After a recording stops, users need to decide what to do with the captured file: keep the MKV
as-is, remux it to MP4 (stream-copy, no quality loss — ADR 0014), or review and optionally trim
it before exporting.

Several surface-shape options were considered:

- **Separate native dialog** — focus-management risk, cannot reuse the in-window nav shell.
- **New top-level tab** — inflates the nav to 6 items again; clashes with the 5-item target.
- **Overlay / modal** — correct for short decisions but too cramped for a full review/edit workflow.
- **In-window mode (stack replacement)** — the main content stack switches to the Edit/Export
  surface; Back returns to Record. Nav items remain accessible but the transport dock is hidden.
  This is the chosen shape.

## Decision

### Surface structure

`EditExportPage` replaces the main content area after recording stops. The surface has three linear
phases stepped by a top stepper bar:

```
Review → Edit → Output
```

- **Review**: post-flight report (frame-drop %, peak A/V drift, pipeline health) populated from
  `RecordingDiagnosticsSnapshot` and peak-drift tracking in `RecordPage`. Video player
  placeholder (deferred to 0.11). Duration / size / codec / container detail rail on the right.
- **Edit**: Trim / Marker controls (keyframe-accurate; spin-box dialog snaps to nearest keyframe
  and nearest marker within 50 ms). Split Chapter button deferred to 0.11.
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

Trim uses `ExtractKeyframeTimestamps` to load all video keyframe PTS values from the MKV master,
then snaps user-entered start/end seconds to the nearest keyframe at or before the requested
time. An additional snap to the nearest marker within 50 ms is applied if any markers exist. The
resulting `TrimRange` is passed to the trimmed overloads of `RemuxToProgressiveMp4` /
`RemuxToMkv`.

Trim is keyframe-accurate and lossless: no decoding occurs.

### Marker sidecar

Recording markers are persisted as a JSON sidecar file (`<stem>.markers.json`) alongside the
output file. The format is:

```json
{
  "version": 1,
  "timebase": "milliseconds",
  "markers": [
    { "timeMs": 1234, "type": "general|cut|highlight", "label": "..." }
  ]
}
```

Markers are never written as container chapters (no metadata change to the video file). The sidecar
is read by `EditExportPage::loadMarkers()` on entry to the edit surface and written by
`saveMarkers()` on each `onAddMarkerClicked()` call. The sidecar path is `EditContext::marker_sidecar_path`.

### IA decision: mode, not tab

The surface is a mode (stack replacement), not a new nav tab. Rationale:
- Keeps top-level nav at 5 items (Record · Settings · Diagnostics · Logs · About).
- The edit workflow is linear and transient — users enter it, decide, and leave.
- Adding a persistent tab implies the surface is always accessible, which is premature.
- Back returns cleanly to Record without polluting the nav history.

### Phase stepper

The three-step stepper (Review / Edit / Output) dynamically highlights the current phase:
- Review phase → "Review" highlighted (accent underline).
- Edit phase → "Edit" highlighted.
- Output / Exporting / Done / Failed → "Output" highlighted.

### Entry points

- **Post-stop "Edit" button**: appears in RecordPage result panel when a single-file successful
  recording exists. Builds a full `EditContext` (file metadata, diagnostics snapshot, markers, MKV
  master path) and emits `editExportRequested(const EditContext&)`.
- **Notification toast "Open" link**: builds a minimal `EditContext` (output path only, no
  diagnostics) and navigates directly to Review phase.

## Consequences

- `EditExportPage` couples to `recorder_core` (for `mp4_remuxer.h`, `pipeline_diagnostics.h`).
  The `edit_export_page_tests` CMake target therefore links `recorder_core`.
- Export runs on a `std::thread`; results are marshalled back to the UI thread via
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.
- Atomic overwrite: write to `.tmp`, then `std::filesystem::rename` temp → final.
- The main window stack (`QStackedWidget`) gains `EditExportPage` as an additional page,
  alongside Record, Settings, Diagnostics, Logs, About.

## Forward

- 0.11: video preview playback (QMediaPlayer or custom D3D11), Split Chapter button.
- Post-1.0: transcription (optional, capability-gated), batch export, GIF/WebP export.
