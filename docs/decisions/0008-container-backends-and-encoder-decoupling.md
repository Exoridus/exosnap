# ADR 0008: Container Backends and Encoder Decoupling

## Status

Accepted — libmatroska/libebml already shipped; fMP4 backend implementation scheduled for 0.2.0
(see roadmap). fMP4 backend library choice intentionally deferred (criteria below).

**Amended:** The deferred fMP4 backend choice has been resolved by ADR 0014 (remux-first,
no fMP4 recording writer). MKV is the sole recording container; progressive MP4 is delivered
via libavformat remux-on-stop. No fMP4 recording writer will be built.

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

### fMP4 / progressive MP4 — backend choice resolved by ADR 0014

~~For the 0.2.0 fMP4 slice, the backend will be one of:~~

~~- **`libavformat`** (FFmpeg) — proven, supports fMP4 fragment semantics, remux, and stream copy;~~
~~  introduces GPL/LGPL surface to the MP4 path.~~
~~- **Controlled ISO-BMFF writer** — purpose-built for ExoSnap's fragment semantics; avoids external~~
~~  library surface; higher implementation cost.~~

~~The choice is intentionally left open until the 0.2.0 design work begins.~~

**Resolved by ADR 0014:** No fMP4 recording writer is built. MKV (via libmatroska) is the sole
recording container; progressive MP4 is delivered via libavformat remux-on-stop. The evaluation
criteria from this section were resolved as follows:

| Criterion | Resolution |
|---|---|
| Crash-resilient fragment semantics | Provided by MKV (incremental cluster writes, truncation-tolerant); no fMP4 writer needed. |
| Remux / trim needs | libavformat handles remux-on-stop (MP4 faststart) and Quick Trim stream-copy on both MKV and MP4 inputs. |
| GPL surface | libavformat lgpl-shared, dynamically linked; no new obligations for a GPL-3.0-or-later project. |
| Binary size | ~28–32 MB in portable ZIP; accepted by the maintainer. Custom minimal build deferred to 0.5.0. |

See ADR 0014 for the full rationale and evaluation numbers.

### Media Foundation MP4 path — transitional

The current MF/SinkWriter MP4 path remains in place as a transitional fallback. It is not extended.
It is removed in the 0.2.0 slice and replaced by the libavformat remux-on-stop path (see ADR 0014).

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

- ~~The fMP4 backend choice must be documented in a follow-on ADR before implementation begins.~~
  Resolved: see ADR 0014.
- The MF MP4 path must not be extended with new features; it is removed in the 0.2.0 slice
  (see ADR 0014).
- Remux / trim (0.11.0) uses libavformat (the remux/trim engine introduced in ADR 0014) for
  stream-copy operations on both MKV and MP4 inputs; Quick Trim is not implementable on the
  MF path (which is removed).
- Container format support in the UI must accurately reflect which backend is active and what it
  supports (see ADR 0010 for the compatibility registry).
