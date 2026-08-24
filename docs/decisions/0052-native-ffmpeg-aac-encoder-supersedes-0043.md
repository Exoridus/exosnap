# ADR 0052: Migrate AAC to FFmpeg's native AAC-LC encoder (supersedes ADR 0043)

## Status

**Accepted — 2026-07-19.** Supersedes **ADR 0043** ("FDK-AAC license position").
The maintainer has explicitly decided to reverse ADR 0043's choice (keep FDK-AAC
via the `fdk-aac-free` stripped fork) and instead migrate the AAC audio path to
FFmpeg's native, LGPL-licensed AAC-LC encoder. This is a deliberate, known
reversal, not an oversight.

## Context

ADR 0043 (accepted 2026-07-11) kept FDK-AAC as ExoSnap's only AAC encoder,
switching from upstream `mstorsjo/fdk-aac` to the `fdk-aac-free`/`fdk-aac-stripped`
LC-only fork to reduce — but not eliminate — the GPL-incompatibility and patent
exposure. Its own analysis was explicit that this "does not fully resolve the
FSF's general 'free, but GPL-incompatible' textual finding" and that adopting the
fork "is making the same kind of risk-tolerance call Fedora made, not resolving
the underlying ambiguity outright." Option (c) in ADR 0043 (drop FDK-AAC for a
different encoder) was kept "in reserve."

FFmpeg's **native** AAC encoder (`avcodec_find_encoder(AV_CODEC_ID_AAC)`) is a
plain LGPL-2.1+ component — not `libfdk_aac`, not gated behind FFmpeg's
`--enable-nonfree`. ExoSnap already vendors FFmpeg (`avformat`/`avcodec`/`avutil`/
`swresample`) as an LGPL shared build via `cmake/VendorFFmpeg.cmake`, consumed for
muxing/remuxing and Edit-page decode. Encoding AAC-LC through that same avcodec
carries no additional third-party static link and sidesteps the fork-specific
"patents don't apply to what's left" argument entirely, rather than accepting it.
ExoSnap only ever uses AAC-LC (see ADR 0043; `FdkAacEncoder` hardcodes
`AOT_AAC_LC`), which the native encoder produces at quality adequate for the
product's Compatibility/MP4 path.

## Decision

1. Adopt **FFmpeg's native AAC-LC encoder** as ExoSnap's AAC encoder, wrapped
   behind the existing `IAudioEncoder` contract as `FfmpegAacEncoder`
   (`libs/engine/src/ffmpeg_aac_encoder.{h,cpp}`). It matches the
   `FdkAacEncoder` behaviour that recording depends on: AAC-LC only, 44.1/48 kHz,
   mono/stereo, default **192 kbit/s** (identical default), and raw AAC access
   units plus an `AudioSpecificConfig` published via `AVCodecContext::extradata`
   — the exact framing the Matroska `A_AAC` writer (CodecPrivate) and the MP4
   remux path already consume. No ADTS framing is introduced anywhere.

2. **The cutover is deliberately deferred.** `MakeEncoderSetup` in
   `audio_thread.cpp` continues to construct `FdkAacEncoder` for
   `AudioCodec::AacMf` for now. The reason is a hard dependency-sequencing gap:
   the prebuilt FFmpeg DLL currently pinned (`exosnap-ffmpeg-build` release
   **r4**) was configured with `--disable-everything` plus a whitelist that
   contains **no `--enable-encoder`**, so its `avcodec` has zero encoders and
   `avcodec_find_encoder(AV_CODEC_ID_AAC)` returns null at runtime. Swapping the
   active path before an encoder-enabled DLL ships would break AAC recording for
   every user. `FfmpegAacEncoder::Init()` therefore fails **cleanly and
   distinguishably** (returns false with an `avcodec_find_encoder`-tagged message)
   against r4 rather than crashing.

3. **`FdkAacEncoder` is retained until the cutover completes.** It is not removed
   in this change, and neither is its `third_party/CMakeLists.txt` fetch.
   `THIRD_PARTY_NOTICES.md` now records that the FDK-AAC dependency is scheduled
   for removal once the cutover lands (the notice stays until it is actually
   unlinked).

## Blocking dependency and remaining steps (in order)

1. **Publish `exosnap-ffmpeg-build` release `r5`.** Branch `feature/aac-encoder`
   adds `--enable-encoder=aac` to the configure whitelist (no `--enable-muxer=adts`
   — raw + ASC framing needs none). The maintainer cuts the release by pushing the
   `r5` tag (which triggers the GitHub Actions release build).
2. **Repin `cmake/VendorFFmpeg.cmake`** — update the `FetchContent_Declare` URL to
   the `r5` asset and its published SHA256, and the `r4 -> r5` notes.
3. **Flip `MakeEncoderSetup`** — construct `FfmpegAacEncoder` for
   `AudioCodec::AacMf` (a two-line change; both encoders share the
   `SetBitrateKbps`/`IAudioEncoder` shape). The `test_ffmpeg_aac_encoder`
   encode-path cases stop skipping and exercise the real encoder.
4. **Remove FDK-AAC** — delete `fdk_aac_encoder.{h,cpp}` + its test, drop the
   `fdk-aac` fetch from `third_party/CMakeLists.txt` and the `engine` link
   line, and remove the FDK-AAC entry from `THIRD_PARTY_NOTICES.md` and
   `licenses/fdk-aac.txt`.

The naming of the `AudioCodec::AacMf` enumerator (a stale reference to the retired
Media Foundation encoder) is a separate, cosmetic rename with a large blast radius
(UI labels, settings persistence, `CodecLabels.h`) and is intentionally **out of
scope** here — tracked as a follow-up.

## Consequences

- No user-visible behaviour change yet: AAC recording still runs through
  `FdkAacEncoder` until the deferred cutover. `docs/product-spec.md` needs no
  update in this change (AAC-LC, container/codec matrix, and defaults are all
  unchanged).
- Once the cutover completes, ExoSnap ships no statically-linked FDK-AAC and the
  GPL-incompatibility / patent-non-grant question ADR 0043 wrestled with no longer
  applies to the shipped binary — the AAC path becomes plain LGPL FFmpeg, on the
  same footing as the mux/decode paths already vendored.
- `FfmpegAacEncoder` is implemented and unit-tested now, so the eventual swap is a
  small, low-risk change gated only on the external FFmpeg release.
