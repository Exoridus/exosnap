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

- `exosnap::engine::BitDepth::Bit10` is a real engine value. When selected with
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

## Update — NVENC bitstream color signaling (fix/color-range-signaling)

A real AV1+Opus+MKV recording measured with ffprobe showed `color_range=tv`
(studio) and `color_space`/`color_transfer`/`color_primaries=unknown`, despite
the default config being Full range / BT.709 — recordings looked dark/washed
out in every player. Root cause: `nvenc_encoder.cpp` never populated the
per-codec bitstream color fields (`NV_ENC_CONFIG_H264/HEVC_VUI_PARAMETERS` for
H.264/HEVC, `NV_ENC_CONFIG_AV1::{colorPrimaries,transferCharacteristics,
matrixCoefficients,colorRange}` for AV1), so the encoded bitstream itself
carried no color description, even though the Matroska `Colour` element (this
ADR, above) was already correctly written from the same `ColorMetadata`.

This is **not cosmetic for AV1**: verified empirically (real NVENC encode on an
RTX 5070 Ti, plus `mkvpropedit` container-only retagging experiments) that
ffmpeg/most decoders derive `color_range`/matrix/primaries/transfer for AV1
**exclusively from the bitstream sequence header**, ignoring a correctly
tagged Matroska `Colour` element outright. For H.264 the container tag is
honored as a fallback when the bitstream is untagged, but signaling at the
bitstream level is still correct practice and is what the NVENC SDK exposes
for exactly this purpose.

Fix: `ApplyColorMetadataToNvenc(NV_ENC_CONFIG&, VideoCodec, const
ColorMetadata&)` (pure, GPU-free, unit-tested) maps `RecorderConfig::color` —
the same single source of truth already driving the `VideoProcessor`
conversion and the Matroska `Colour` element — onto the codec-specific NVENC
fields, called from `NvencEncoder::FetchPresetConfig()` after `SetColor()` is
wired from `video_thread.cpp`. No signaling-unrelated behavior changed; no new
UI; HDR fields (BT.2020/PQ/mastering metadata) round-trip through the same
generic mapping without further type churn, consistent with this ADR's
original design. (The default *range* value did change — see the next update.)

**Known, separate, pre-existing gap (not part of this fix):** the AV1 track's
Matroska `CodecPrivate` (`codec_private.cpp::DeriveAv1CodecPrivate`) has only
ever written the 4-byte `AV1CodecConfigurationRecord` header — it does not
embed the sequence-header OBU (`configOBUs`), unlike a spec-conformant av1C
(verified: a real ffmpeg-muxed AV1-in-MKV file carries a 16-20 byte
CodecPrivate, header + seq-header OBU; ours is always exactly 4 bytes). The
sequence header (now carrying correct color info after this fix) is only ever
present **in-band**, in whichever frame(s) NVENC repeats it in (first frame at
minimum; `repeatSeqHdr` is left at the preset default). Tools that decode/parse
packets (ffprobe, all real players) see the fix correctly; tools that read only
`CodecPrivate`/extradata without decoding a frame (e.g. `ffmpeg -bsf:v
av1_metadata` for retagging already-recorded files) do not, and may fail
outright (`Invalid data`) because the extradata isn't a valid av1C at all, not
because of anything color-related. Filed as a follow-up; out of scope here
under the no-scope-creep rule (it is a general AV1 CodecPrivate conformance gap,
independent of color truth, and requires restructuring a fixed-4-byte field to
a variable-length one end-to-end).

## Update — default Y'CbCr range flipped Full -> Limited (fix/color-range-signaling)

**Decision reversed:** the 0.7.0 update above deliberately made Full (0-255)
the default, reasoning that truthful tagging plus a driver-honoured conversion
would let players display it correctly. A follow-up controlled comparison
(identical AV1 pixel content, once tagged `pc`/full and once `tv`/limited)
showed this assumption does not hold for the player the product's users
actually use: **VLC ignores the range flag entirely and always applies
limited->full expansion**, regardless of what the bitstream or container says.
A Full-range recording is therefore *permanently* crushed/dark in VLC — no
amount of correct signaling fixes it, because the flag is never read. This is
also the documented reason the rest of the consumer screen-recording/streaming
ecosystem (OBS included) encodes limited by default: player compatibility in
practice outweighs the theoretical precision advantage of full range, because
so much of the installed player base either ignores the range flag or
defaults to assuming limited when in doubt.

**New default: Limited (16-235, broadcast/studio).** Full remains fully
supported as an explicit opt-in for pipelines/players known to honour the
range flag (e.g. a controlled NLE workflow). This is a pure default-value
change, not a capability change — both values were, and remain, valid for
every codec/container combination (never gated). Changed in exactly four
places (the operative defaults, verified by tracing the config flow rather
than assumed):

- `app/models/OutputSettingsModel.h` (`OutputSettingsModel::color_range`) — the
  actual UI/preset-facing default; everything else derives from this.
- `libs/capability/include/capability/user_config.h`
  (`UserRecorderConfig::color_range`) — the default for direct engine API
  consumers that bypass the preset/UI layer.
- `libs/engine/include/exosnap/engine/color_metadata.h`
  (`ColorMetadata::range`, and thus `ColorMetadata::Sdr709()`) — technically
  inert for the normal flow (`capability::translation.cpp`'s
  `ToRecorderCoreConfig` always sets `core_config.color.range` explicitly from
  `UserRecorderConfig::color_range`, never leaving the struct default in
  place), but kept in sync for any direct `engine` consumer (tests,
  `tools/probes/probe_record`) that constructs a default `ColorMetadata`
  without going through `capability`.
- `libs/engine/src/yuv_to_bgra.h` (`YuvToBgraParams::range`) — likewise
  inert in production (`video_thread.cpp`'s CaptureFrame and live-preview call
  sites always explicitly assign `.range` from the live session config before
  converting — verified, not assumed), kept in sync for consistency.

`translation.cpp`'s range ternary and `video_thread.cpp`'s VideoProcessor
`fullRange` boolean both already read the *live config value* rather than a
hardcoded constant, so they need no code change — flipping the upstream
default automatically flips the `VideoProcessorSetOutputColorSpace1` studio/
full selection and the NVENC/Matroska tagging in lockstep, exactly as this
ADR's single-source-of-truth design intended.

**Persistence — schema 20 with a targeted migration (orchestrator product
decision after adversarial review):** `color_range` is a persisted TOML preset
field (`RecordingPresetStore`, key `output.color_range`, introduced schema 18)
that is **always written explicitly** on save and never re-seeded from code
defaults on load. Left alone, every existing preset — including the built-in
default preset — would therefore carry a materialized `"full"` from the old
code default and the dark-recordings bug would persist for 100% of existing
users. Aggravating factor: because of the ConfigPage hydration bug (fixed in
this branch), the colour-range combo always displayed "Full (PC)" regardless
of the stored value, so an *informed* Full choice could never have existed —
a stored `"full"` under schema ≤19 is provably the old default, not a user
decision.

Therefore `kPresetSchemaVersion` 19 → 20 with a **targeted field migration**
instead of the usual full reset: `Load()` accepts schema-19 files, rewrites
`color_range == "full"` to `"limited"` (one-shot; persisted as schema 20 on
the next save), and preserves everything else — user presets survive. A
schema-20 file with an explicit `"full"` is a deliberate post-flip opt-in and
is respected. Schemas older than 19 keep the pre-existing full-reset
behavior. The pre-1.0 "incompatible data resets" policy was deliberately NOT
applied here because nothing is structurally incompatible — a reset would
destroy user presets to fix a single field whose correct value is precisely
derivable.

**Consequence:** every newly created default-profile recording going forward
is limited-range end-to-end (VideoProcessor output, NVENC bitstream, Matroska
Colour tag) — verified by updated unit tests. Full remains one dropdown
selection away and is tagged with equal truthfulness (this ADR's core fix).
