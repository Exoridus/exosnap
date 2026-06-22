# ADR 0032: Color-Management Foundation

## Status

Accepted — first slice of the 0.7.0 "HDR and final codec matrix" wave
(feat/0.7.0-color-metadata). Establishes the color-description model the roadmap
lists as a prerequisite ("Color-management architecture … Precedes HDR and
extended chroma"). The HDR10 metadata and 10-bit/P010 slices build on this model
without further type churn. Relates to [[0008-container-backends-and-encoder-decoupling]]
and [[0011-encoder-capability-and-settings-schema]].

## Context

Through 0.6.0 the engine never described the color of its output:

- **Conversion was undefined.** `video_thread.cpp` converts the captured
  full-range RGB desktop/window composite to NV12 with a D3D11 `VideoProcessor`,
  but never called `VideoProcessorSetStreamColorSpace` /
  `VideoProcessorSetOutputColorSpace`. The matrix (BT.601 vs BT.709) and range
  (full vs studio) were therefore the **driver default** — implementation-defined
  and not guaranteed to be identical across GPUs/driver versions. The same
  desktop could encode to subtly different colors on different machines.
- **The container carried no color tags.** `matroska_stream_writer.cpp` wrote no
  `Colour` element, so every player had to guess the primaries/transfer/matrix
  (most assume BT.709 for HD, but this is a guess, and NLEs may differ).

A guessed-but-untagged BT.709 pipeline mostly looks right, but it is not
deterministic and gives the upcoming HDR work nothing to build on: HDR10 needs a
real color-description model (BT.2020 primaries, PQ/HLG transfer, mastering
display + content light level) wired from a single source through both the
encoder-input conversion and the container.

## Decision

Introduce a single `ColorMetadata` model (`recorder_core/color_metadata.h`) using
ISO/IEC 23001-8 (CICP) code points — the same code points Matroska and MP4 store
— as the **single source of truth** for both sides of the pipeline:

- **Default = SDR Rec.709, limited range, 8-bit.** This makes the previously
  implicit assumption explicit. Fields for HDR (BT.2020 primaries, PQ/HLG
  transfer, `hdr` flag, MaxCLL/MaxFALL) are modeled now but left unset; SDR
  recordings omit the HDR sub-elements entirely.
- **Deterministic conversion.** `video_thread.cpp` now pins the `VideoProcessor`
  input to full-range RGB and the NV12 output to studio-range BT.709, so the
  conversion is identical on every GPU and matches the tags.
- **Container tags.** `matroska_stream_writer.cpp` writes a `Colour` element
  (`Primaries`, `TransferCharacteristics`, `MatrixCoefficients`, `Range`,
  `BitsPerChannel`; plus `MaxCLL`/`MaxFALL` when `hdr` is set) from the model.
- `RecorderConfig::color` carries the model into the engine; `mux_thread.cpp`
  threads it into `MatroskaStreamConfig`. UI exposure stays deferred — 0.7.0
  SDR recordings simply get correct, deterministic, explicitly-tagged BT.709.

Input/output ranges agree (full RGB in, studio Y'CbCr out, tagged limited), so
there is no black-level mismatch.

## Consequences

- Recordings are now color-deterministic across GPUs and explicitly tagged;
  players and NLEs no longer guess. This is a (small) change to the actual
  encoded pixels where a driver previously defaulted to BT.601 — **requires a
  real visual spot-check** before the wave ships (a unit test covers the
  container tags but not the on-screen color).
- The MP4 remux path (`mp4_remuxer.cpp`) does not yet copy these tags into the
  `colr`/codec-specific boxes — tracked for the HDR/`hvc1` slice.
- The model is HDR-ready: the HDR slice populates the BT.2020/PQ + mastering
  fields and adds the `MasteringMetadata` sub-element; no type changes needed.
- 10-bit (`BitsPerChannel=10`) is representable in the model but the encode path
  (P010, 10-bit NVENC profiles) remains a later slice.
