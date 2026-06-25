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

**Updated 2026-06 (0.7.0 — Y'CbCr range made selectable; default flipped to Full).**
Two corrections after live verification:
1. *Bug:* the legacy `D3D11_VIDEO_PROCESSOR_COLOR_SPACE.Nominal_Range` is ignored on
   **output** by the NVIDIA driver, so the VideoProcessor actually emitted full-range
   Y'CbCr (black=0/white=255) while the container was tagged limited — recordings
   looked too dark (player expanded limited→full, crushing shadows). The conversion
   now uses `ID3D11VideoContext1::VideoProcessorSet{Stream,Output}ColorSpace1` with
   explicit `DXGI_COLOR_SPACE` enums, which drivers honour, so the pixels genuinely
   match the tag (GPU-verified with a black/white pattern: 16/235 limited, 0/255 full).
2. *Default change:* the Y'CbCr range is now a **user-selectable setting** (Full / Limited),
   **defaulting to Full (0-255)**. The captured desktop is native full-range RGB, so
   Full preserves precision (no banding from the 16-235 compression) and avoids a lossy
   round-trip — the better fidelity choice for screen content. Limited remains available
   for maximum compatibility with players/editors that ignore the range flag. The chosen
   range drives BOTH the VideoProcessor output space AND the container `Range` tag, so
   they always agree. Persisted in `RecordingPreset` (schema 18). This is independent of
   any GPU/display "output dynamic range" setting, which only governs the cable signal to
   the monitor, not the full-range desktop buffer the engine captures.

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

## Update — 10-bit / P010 encode path landed (0.7.0 S5)

The 10-bit encode path is now implemented (ValidUnvalidated → GPU-verified on an
RTX 5070 Ti / NVENC):

- `recorder_core::BitDepth::Bit10` is a real engine value. When selected with
  HEVC or AV1, the `VideoProcessor` converts BGRA → **P010**
  (`DXGI_FORMAT_P010`) instead of NV12, the P010 textures are registered with
  NVENC as `NV_ENC_BUFFER_FORMAT_YUV420_10BIT`, and the encoder uses the HEVC
  **Main10** profile (`NV_ENC_HEVC_PROFILE_MAIN10_GUID`) / AV1 Main profile with
  `inputBitDepth = outputBitDepth = NV_ENC_BIT_DEPTH_10`. H.264 stays 8-bit only
  (rejected by `Validate()` with `E_NOTIMPL`).
- The Matroska hvcC (`CodecPrivate`) now **parses the real SPS** (Exp-Golomb,
  emulation-prevention-byte removal) for `general_profile_idc` /
  `bit_depth_luma/chroma_minus8` / `chroma_format_idc` / level, instead of the
  previous hardcoded 8-bit-Main constants, so a Main10 stream is tagged
  correctly. ffprobe confirms `profile=Main 10`, `pix_fmt=yuv420p10le` for both
  MKV and MP4 (hvc1), and an 8-bit recording still reports `profile=Main` /
  `pix_fmt=yuv420p`.
- **Known limitation — SDR 10-bit only.** The `VideoProcessor` output color space
  stays studio-range BT.709 (the SDR HD standard). 10-bit here buys reduced
  banding, not wider gamut: HDR transfer/primaries (BT.2020 / PQ / HLG, mastering
  display + MaxCLL/MaxFALL) remain the next slice. The `ColorMetadata`
  `BitsPerChannel` should be set to 10 by the UI/profile layer when 10-bit is
  chosen (the model already supports it).
- CaptureFrame (snapshot) is not implemented for 10-bit (the NV12→BGRA readback
  assumes 8-bit); it fails cleanly and does not affect the encode path.
