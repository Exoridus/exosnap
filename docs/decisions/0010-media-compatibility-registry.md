# ADR 0010: Media Compatibility Registry

## Status

Accepted — implementation scheduled for 0.2.0 (container registry) and 0.5.0 (full capability
registry) (see roadmap).

## Context

ExoSnap exposes container, video codec, and audio codec choices across multiple UI pages (Output,
Video, Audio, presets). Without a single authority governing which combinations are valid,
implementations diverge:

- Container/codec pairings get validated inconsistently (e.g., UI allows WebM + H.264 even though
  no player supports it).
- Compatibility warnings are duplicated or absent depending on which code path runs.
- Adding a new codec or container requires touching every validation site.

The UI must only surface vetted combinations. "Theoretically muxable" is not the bar — the bar is
a tested player/editor matrix.

## Decision

A single **media compatibility registry** is the authoritative source for:

- Whether a container/codec combination is allowed, recommended, experimental, or prohibited.
- Associated compatibility warnings (Apple, browser, NLE).
- Fallback behavior when a selected combination becomes unavailable.

The registry is queried by the UI and by `RecordingPreset` reconciliation. No combination is
offered to the user unless the registry explicitly allows it.

### Container / codec / audio matrix

| Container | Video codecs | Audio codecs | Notes |
|---|---|---|---|
| MKV | AV1, HEVC, AVC | Opus, AAC, PCM, FLAC | Primary target; VFR supported |
| MP4 | AV1, HEVC, AVC | AAC | Primary for Apple/NLE compatibility; delivered as progressive MP4 via remux-on-stop (ADR 0014) |
| MP4 | — | PCM | Allowed; sample-entry variant and player matrix must be specified |
| MP4 | — | FLAC | Not a 1.0 target; fragile MP4 compatibility |
| WebM | AV1 | Opus | AV1 + Opus only |
| WebM (optional) | VP9 (later) | Opus | — |

### Explicit prohibitions

- **WebM + H.264** is prohibited. No major player or browser supports H.264 in a WebM container.
  The registry must return `Prohibited` for this pairing; it must not appear in any UI picker.
- **WebM + HEVC** is prohibited for the same reason.

### Known caveats

**PCM in MP4:** ISO-BMFF supports PCM (`lpcm`, `in24`, `in32`, `sowt`, etc.) but compatibility
varies by sample-entry type, tool, and platform. The registry entry for PCM-in-MP4 must specify
the concrete sample-entry variant and the tested player matrix before the combination is marked
`Allowed`. A bare "PCM" entry without this specification is not acceptable.

**FLAC in MP4:** Not a 1.0 target. FLAC fits MKV cleanly; FLAC-in-MP4 compatibility is fragile
and player support is inconsistent. The registry returns `Experimental` or `Prohibited` until
a real player matrix is validated.

**`hvc1` vs `hev1` for HEVC in MP4:** These two four-character codes differ in where parameter
sets are stored (`hvc1` = in `hvcC` box; `hev1` = in-band). Apple/QuickTime requires `hvc1` for
hardware-accelerated playback. The correct variant must be verified on real files via `ffprobe`
or Bento4/MP4Box in the 0.7.0 HEVC/HDR slice (via the libavformat remux path; see ADR 0014).
The registry must not mark HEVC-in-MP4 as
`Recommended` until this is confirmed.

### Compatibility classification

| Classification | Meaning |
|---|---|
| `Recommended` | Vetted combination; tested player/editor matrix on file |
| `Allowed` | Works but with caveats; warning shown in UI |
| `Experimental` | Technically possible; not yet tested at scale |
| `Fallback` | Used when a preferred combination fails; not user-selectable |
| `Prohibited` | Must not appear in the UI under any circumstance |

## Consequences

- Adding a new codec or container requires a registry entry with a tested matrix — not just a
  muxer capability flag.
- The UI pickers are generated from registry queries; per-page hardcoded lists are removed.
- `RecordingPreset` reconciliation calls the registry to validate loaded presets; invalid
  combinations are corrected to the nearest allowed combination on load.
- The `hvc1`/`hev1` and PCM-in-MP4 questions are blocking items before those combinations can
  be marked `Recommended` or `Allowed`.
- The registry is a pure value-model component with no Qt or UI dependencies; it is testable
  without a running application.
