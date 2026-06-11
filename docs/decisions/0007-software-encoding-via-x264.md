# ADR 0007: Software Encoding via x264 (and Optional SVT-AV1)

## Status

Accepted — implementation scheduled for 0.8.0 (see roadmap).

## Context

ExoSnap needs a software H.264 encoder for three reasons:

1. **Universal fallback** — users without supported hardware (or whose GPU encoder is busy/failed)
   can still record.
2. **GPU-less testing** — CI and development environments without a GPU must be able to run the
   encode path without hardware stubs.
3. **ARM64 readiness** — a future ARM64 port has no NVENC/AMF/QSV; software encoding provides an
   initial working baseline.
4. **Hardware-init failure detection** — the software path catches regressions that GPU-only builds
   would silently mask.

The candidate software H.264 encoders are x264 and the FFmpeg internal encoder (`libx264` via
`libavcodec`). SVT-AV1 is the candidate for software AV1.

## Decision

### x264 for software H.264

x264 (`GPL-2.0-or-later`) is the software H.264 encoder. It is integrated behind the
`IVideoEncoder` interface as `X264VideoEncoder` (see ADR 0006).

x264 is not wired directly into the video thread or called from any UI layer. It is constructed
exclusively through `VideoEncoderFactory` and hidden behind `IVideoEncoder`. This keeps the
distribution/license gate centralized.

**License and patent gate:** A license + patent-distribution audit must be completed and signed off
before any release binary ships x264. x264's GPL-2.0-or-later license is compatible with
ExoSnap's GPL model, but patent licensing for H.264 distribution requires explicit review. The
audit gate is enforced in the release pipeline, not deferred to runtime.

### SVT-AV1 for software AV1 (optional)

SVT-AV1 is an optional software AV1 encoder, integrated as `SvtAv1VideoEncoder`. It is opt-in at
build time and disabled by default until the performance and binary-size impact is characterized.
AV1 patent licensing is confirmed royalty-free (AOM patent pool), so no additional patent audit is
required.

### Ordering rationale: software before AMD/Intel

The roadmap places software encoding (0.8.0) ahead of AMD AMF (0.9.0) and Intel QSV (0.10.0):

- Software encoding closes the universal-fallback gap for all users immediately.
- AMD/Intel hardware encoders widen the audience but do not close a reliability gap for existing
  NVIDIA users.
- The software path validates the `IVideoEncoder`/`VideoEncoderFactory` interface under conditions
  not covered by NVENC alone.
- CI and ARM64 testing become possible without GPU access.

## Consequences

- No release ships x264 until the license + patent-distribution audit passes.
- `VideoEncoderFactory` must enforce the x264 gate; it must not be possible to select x264 through
  a config value in a release build that has not cleared the audit.
- SVT-AV1 is a build-time opt-in; the default release binary may omit it until performance is
  validated.
- Software encoding performance warnings (CPU load, thermal) must be surfaced through
  `EncoderDiagnosticsAdapter`, not suppressed.
- GPU → CPU readback (NV12 or BGRA frame copy) is required when a hardware surface cannot be
  passed directly; this is an expected cost, not a pipeline defect.
