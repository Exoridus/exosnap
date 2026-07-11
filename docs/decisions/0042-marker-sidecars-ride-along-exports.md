# ADR 0042: Marker Sidecars Ride Along Exports (No Container Chapters)

## Status

Accepted.

## Context

Recording markers are persisted as a JSON sidecar (`<media>.markers.json`) next to the recording —
one canonical format and path convention, established in ADR 0022 (`app/models/MarkerSidecar.h`,
written by `RecordingCoordinator` during/after capture).

The Edit/Output/Save surface (ADR 0022) exports a possibly **trimmed** copy of the recording via
stream-copy remux. Until now the exported file carried no marker information at all: the sidecar
stayed behind with the original recording, and its timestamps would be wrong for a trimmed clip
anyway. Two ways to attach markers to an export were considered:

- **Container chapters (MKV Chapters / MP4 `chpl`-style metadata).** Player-visible, but requires
  container-specific writers on the remux path, mutates the media file's metadata, differs subtly
  between MKV and MP4, and locks the marker model to what chapter atoms can express.
- **The existing JSON sidecar, retimed.** One writer, one format, container-agnostic, no media
  mutation, trivially diffable/inspectable, already round-trips through the shared serializer.

## Decision

**Exports never write container chapters.** Markers ride along an export as the same JSON sidecar
format, derived per the ONE path convention: `<export>.markers.json` (media path with the
extension replaced).

Rules, all implemented as pure logic (`RetimeMarkersForTrim` in `app/models/EditTimelineModel.h`,
`PlanMarkerSidecarForExport` / `ApplyMarkerExportPlan` in `app/models/MarkerSidecar.h`):

1. **Retiming.** Surviving markers are re-based to the trimmed clip's start
   (`time_ms -= trim_start_ms`).
2. **Survival window.** A marker survives when it lies in the half-open window
   `[trim_start, trim_end)` — the same boundary convention as the split-segment partitioning
   (`PartitionSegmentMarkers`): a marker exactly on the out-point is outside the clip. Markers cut
   away by the trim are dropped.
3. **Write only when markers exist.** A sidecar is written **only** when at least one marker
   survives. An export with no surviving markers instead **removes** any stale sidecar already at
   the destination (relevant for overwrite-original exports and repeated exports to the same
   `_edit` path) — a sidecar must never describe timestamps its media file does not have.
4. **Timing.** The plan (marker snapshot + trim window) is computed on the UI thread when the
   export starts; the sidecar is written/removed on the export thread only **after** the remux
   succeeded and the temp file was atomically renamed into place. A failed or cancelled export
   leaves existing sidecars untouched.

The sidecar format itself is unchanged from ADR 0022 (version 1; `timeMs` / `type` / `label`;
optional `media`); this ADR only adds the export-time producer.

## Consequences

- Players do not see chapters in exported files. Anything that wants marker data reads the sidecar
  — the same contract the rest of the app already uses.
- The original recording's sidecar is never modified by an export to a new file; only the
  destination's sidecar is written or removed.
- Pre-1.0, no migration: recordings exported before this change simply have no sidecar next to the
  exported copy.

## Forward

- If a future release wants player-visible chapters, that becomes an explicit opt-in export option
  layered on top of the sidecar — the sidecar stays the source of truth.
