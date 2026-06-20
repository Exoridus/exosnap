# ADR 0028: Lossless FLAC audio codec (MKV-only)

## Status

Accepted — implemented in 0.6.0 (feat/0.6.0-flac-codec), an Audio v2 slice that
follows the uncompressed PCM slice ([[0027-pcm-audio-codec]]). Where PCM added a
lossless codec with no new dependency, this slice adds the second lossless
option — **compressed** lossless — and introduces ExoSnap's first new
audio-codec third-party library, **libFLAC**.

## Context

ExoSnap's audio path now ships three codecs: Opus and AAC-LC (both lossy) and
uncompressed PCM (lossless but large: ~11.5 MB/min stereo at 48 kHz/16-bit).
PCM's file size is inherent to being uncompressed. **FLAC** (Free Lossless Audio
Codec) gives the same bit-exact, archival-grade audio as PCM but typically
~40–60 % smaller, with universal decoder support. The roadmap container/codec
matrix (`docs/roadmap.md`) lists FLAC under the 0.6.0 Audio v2 wave and notes
that **FLAC-in-MP4** is intentionally **not a 1.0 target** — FLAC fits Matroska
natively (`A_FLAC`), and MP4's FLAC sample-entry support is needlessly fragile
across players.

Matroska has a fully specified `A_FLAC` track type. The one mandatory
requirement: the track's `CodecPrivate` must be the **native FLAC stream
header** — the `fLaC` 4-byte marker followed by the STREAMINFO metadata block
(and any other metadata blocks emitted before the first audio frame). Players
require this to decode an embedded FLAC stream.

The capture pipeline already runs at a fixed **48 kHz, stereo** internal format
and converts Float32 → interleaved 16-bit in the PCM/AAC paths, so a minimal,
correct FLAC slice is: feed 16-bit samples to libFLAC and emit its encoded
frames into an `A_FLAC` Matroska track.

## Decision

Add **FLAC** as a selectable audio codec producing **lossless, compressed**
audio in a Matroska `A_FLAC` track at the existing pipeline format
(48 kHz, stereo, 16-bit, libFLAC compression level 5). **FLAC is MKV-only**;
WebM and MP4 reject it.

### Dependency: libFLAC (FetchContent)

- **libFLAC 1.4.3** (xiph/flac), **BSD-3-Clause**, pulled via FetchContent in
  `third_party/CMakeLists.txt` mirroring the libopus/FDK-AAC style. We build only
  the C library: `BUILD_CXXLIBS`, `BUILD_PROGRAMS`, `BUILD_EXAMPLES`,
  `BUILD_TESTING`, `BUILD_DOCS`, `INSTALL_MANPAGES`, and `WITH_OGG` are all OFF;
  `BUILD_SHARED_LIBS` is forced OFF for the populate (and restored after), so the
  static `FLAC::FLAC` target defines `FLAC__NO_DLL` for correct static linkage on
  MSVC. The exported target is **`FLAC::FLAC`** (an ALIAS of `FLAC`), which
  propagates its `include/` directory via `INTERFACE`. License `COPYING.Xiph` is
  staged to `licenses/flac.txt` and referenced in `THIRD_PARTY_NOTICES.md`.
  `recorder_core` links `FLAC::FLAC` (both the full and skeleton builds).
- No Ogg dependency: `A_FLAC` carries native FLAC frames (not Ogg-FLAC).

### Engine model and encoder

- `recorder_core::AudioCodec` gains `Flac` (the enum was `{AacMf, Opus, Pcm}`);
  `capability::AudioCodec` likewise gains `Flac` (its `AllAudioCodecs()` array
  grows 3 → 4). `capability::AudioCodec::Flac` ↔ `recorder_core::AudioCodec::Flac`
  everywhere the two enums are mapped
  (`RecordingCoordinator::ApplyOutputSettingsToRecorderConfig`, `translation.cpp`,
  the visual-harness scenario mapper, history/label helpers).
- `FlacAudioEncoder` (`libs/recorder_core/src/flac_audio_encoder.{h,cpp}`) wraps
  libFLAC's stream encoder behind the existing `IAudioEncoder` interface so the
  audio thread and mux path treat it like Opus/AAC/PCM. `Init` creates a
  `FLAC__StreamEncoder`, sets channels / `bits_per_sample=16` / sample rate /
  compression level 5 / streamable subset, and calls
  `FLAC__stream_encoder_init_stream` with a write callback. Each `FeedFloat32`
  converts the interleaved Float32 buffer to interleaved int16 (clamp to [-1, 1],
  scale by 32767, round-to-nearest — the same mapping as PCM) and calls
  `FLAC__stream_encoder_process_interleaved`. `Flush` calls
  `FLAC__stream_encoder_finish` to drain buffered frames.
- **CodecPrivate convention.** libFLAC's write callback receives `samples==0`
  for header/metadata writes and `samples>0` for audio frames. The encoder
  captures **everything written before the first audio frame** (the `fLaC`
  marker + STREAMINFO and any leading metadata blocks) as `CodecPrivateBytes()`
  — the native FLAC header — and treats subsequent (`samples>0`) writes as frame
  packets. This is the simplest robust way to obtain the mandatory A_FLAC
  CodecPrivate without a second STREAMINFO-parsing pass.
- **Variable packets per call + PTS.** libFLAC buffers a full blocksize before
  emitting a frame, so a single `FeedFloat32` may produce **zero or more**
  packets (like the AAC path — the audio thread already handles a variable
  packet count). PTS is derived from a dedicated **emitted-sample counter**: each
  emitted frame's PTS is `(emitted_samples) * 1e9 / sample_rate`, advancing by
  the frame's sample count. This reflects the input position of the frame's first
  sample regardless of *when* libFLAC flushes it (reading the caller's
  accumulated-frame counter at flush time would lag by one blocksize).
- `audio_thread.cpp` gains a FLAC branch mirroring the PCM/AAC branches: init the
  encoder, publish its (non-empty) CodecPrivate produced during `Init` and mark
  the track ready, run the capture/encode loop, drain via `Flush`, push the audio
  EOS sentinel.

### Mux / Matroska writer

- `StreamAudioCodec` gains `Flac` (the enum was `{Aac, Opus, Pcm}`). The
  audio-track builder adds an A_FLAC path: CodecID `A_FLAC`, the encoder's native
  `fLaC` header copied into `KaxCodecPrivate`, and `KaxAudioBitDepth = 16` in the
  track audio header (alongside 48000 Hz / 2-channel). A CodecPrivate that does
  not begin with the `fLaC` marker is rejected at `Open()`. `mux_thread.cpp` maps
  `recorder_core::AudioCodec::Flac` → `StreamAudioCodec::Flac`.

### Validation and capability gating

- `RecorderSession::Validate` accepts `AudioCodec::Flac` **only** for
  `Container::Matroska`; WebM and MP4 are rejected with `E_INVALIDARG`.
- `ContainerCompatRegistry`: MKV + (AV1|H.264) + FLAC are **Allowed**
  (FLAC is video-codec-independent); MKV + HEVC + FLAC stays Experimental (HEVC
  video unimplemented); WebM + FLAC is **Prohibited**; MP4 + FLAC is
  **Experimental** (FLAC-in-MP4 deferred). `Allowed` maps to `ValidUnvalidated`
  in the CapabilitySet, so FLAC is selectable with a "not validated on this
  system" caveat. The capability builder marks `Flac` `Available`.
  `translation.cpp` whitelists MKV+AV1+FLAC and MKV+H264+FLAC. The reconcile
  audio-candidate order is Opus → AAC → PCM → FLAC.

### UI and persistence

- ConfigPage Output card: the audio-codec selector adds "FLAC (lossless)"
  **only when the container is MKV** (the list is rebuilt on container change;
  switching away from MKV with PCM or FLAC selected falls back to Opus).
- Preset persistence round-trips the `"flac"` string value;
  `kPresetSchemaVersion` is bumped **13 → 14** (defensively; the change is
  additive). `RecordingHistoryStore` round-trips `"flac"`.
- FLAC is lossless and has no bitrate; the engine does not read
  `audio_bitrate_kbps` for the FLAC path. The Expert audio bitrate / Opus-specific
  rows are left inert for FLAC (consistent with how PCM left them).

## Consequences

- ExoSnap can record lossless **compressed** stereo audio into MKV — archival
  quality at meaningfully smaller files than PCM. The container-compat reason
  surfaces "Smaller than PCM, still lossless."
- The `IAudioEncoder` abstraction again proved sufficient: FLAC slotted in as a
  fourth encoder, the only structural difference from PCM being that it links a
  real library and emits a variable number of packets per call (already supported
  by the audio thread / mux queue, like AAC) and carries a real CodecPrivate.
- FLAC's CodecPrivate is captured for free from the write-callback header writes,
  so no STREAMINFO parser is needed.
- The encoder is unit-tested headless (no playback): Init/Feed/Flush produce
  non-empty frames, CodecPrivate starts with `fLaC` and includes STREAMINFO,
  silence finishes to a valid stream, first-packet PTS is 0, and degenerate
  inputs are no-ops. Validation, registry, capability, translation, stream-writer
  (A_FLAC + native header + BitDepth=16) and preset round-trip tests cover the
  wiring.

## Deferred

- **FLAC in MP4** — not a 1.0 target; FLAC fits MKV natively and MP4 FLAC
  sample-entry support is fragile across players (roadmap note). MP4 + FLAC stays
  Experimental / rejected.
- **Full channel / sample-format model** — arbitrary sample rate,
  mono/multichannel, and 24-bit depth. This slice is fixed at 48 kHz / stereo /
  16-bit, matching the pipeline format and FLAC's `bits_per_sample=16`.
- **Configurable compression level** — fixed at libFLAC's default (5). Exposing a
  speed/size trade-off control is a later refinement.
- RNNoise (the remaining mic-DSP stage) — see [[0026-mic-agc]] deferred list.
