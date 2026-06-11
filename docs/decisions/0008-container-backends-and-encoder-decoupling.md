# ADR 0008: Container Backends and Encoder Decoupling

## Status

Accepted — libmatroska/libebml already shipped; fMP4 backend implementation scheduled for 0.2.0
(see roadmap). fMP4 backend library choice intentionally deferred (criteria below).

## Context

ExoSnap needs to write at least three container formats: MKV, WebM, and MP4 (progressive and
fMP4). These containers have different write semantics:

- MKV/WebM: seekable, cluster-based, naturally suited to streaming writes; libmatroska/libebml
  used since the MATROSKA-MUX-CORRECTNESS-R1 work.
- MP4: requires either a complete-file rewrite on stop (moov-at-end), or fragmented MP4 (fMP4)
  with front-loaded metadata — critical for crash resilience.
- Remux and trim (Quick Trim, 0.11.0) need a container-aware stream-copy path that is independent
  of the active encoder.

A "single muxer does everything" approach (e.g., routing all formats through a single library) risks
coupling encoder surface requirements to muxer limitations and creates GPL surface exposure for
paths that could avoid it.

## Decision

### libmatroska / libebml for MKV and WebM

Already in use. `libmatroska` + `libebml` write MKV and WebM directly. The implementation
follows the correctness work in MATROSKA-MUX-CORRECTNESS-R1 (Segment ForceSize, SeekHead,
per-keyframe Cues, Duration metadata, AAC PTS alignment).

This is the production-quality path for the current release.

### fMP4 / progressive MP4 — backend choice deferred

For the 0.2.0 fMP4 slice, the backend will be one of:

- **`libavformat`** (FFmpeg) — proven, supports fMP4 fragment semantics, remux, and stream copy;
  introduces GPL/LGPL surface to the MP4 path.
- **Controlled ISO-BMFF writer** — purpose-built for ExoSnap's fragment semantics; avoids external
  library surface; higher implementation cost.

The choice is intentionally left open until the 0.2.0 design work begins. The criteria that will
decide it:

| Criterion | Notes |
|---|---|
| Crash-resilient fragment semantics | Can the backend write sealed fragments without a final moov pass? |
| Remux / trim needs | Does Quick Trim (0.11.0) require stream-copy capabilities that the backend already provides? |
| GPL surface | Does using libavformat in the MP4 path add GPL obligations beyond what is already accepted? |
| Binary size | Does adding libavformat for MP4 alone justify the size cost vs. a minimal writer? |

The ADR for the fMP4 backend will be written at the start of the 0.2.0 slice with those criteria
resolved.

### Media Foundation MP4 path — transitional

The current MF/SinkWriter MP4 path remains in place as a transitional fallback. It is not extended.
It will be replaced by the fMP4 backend in 0.2.0.

### Encoder / container decoupling

Encoders and containers are deliberately decoupled:

- `IVideoEncoder` produces encoded packets (NVENC, AMF, QSV, x264, SVT-AV1 — see ADR 0006/0007).
- The muxer receives packets and writes the container; it has no knowledge of which encoder
  produced them.
- `libavformat` can remux/trim packets even when a native SDK did the encoding; no re-encode is
  required for stream-copy operations.

This decoupling is enforced at the interface level: no muxer type is referenced inside any
`IVideoEncoder` implementation, and no encoder type is referenced inside any muxer implementation.

## Consequences

- The fMP4 backend choice must be documented in a follow-on ADR before implementation begins.
- The MF MP4 path must not be extended with new features; any MP4 improvement targets the fMP4
  backend.
- Remux / trim (0.11.0) assumes the fMP4 backend is available; Quick Trim is not implementable
  on the MF path.
- Container format support in the UI must accurately reflect which backend is active and what it
  supports (see ADR 0010 for the compatibility registry).
