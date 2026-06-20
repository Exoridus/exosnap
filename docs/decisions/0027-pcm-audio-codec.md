# ADR 0027: Uncompressed PCM audio codec (MKV-only)

## Status

Accepted — implemented in 0.6.0 (feat/0.6.0-pcm-codec), an Audio v2 slice
alongside per-track gain + mute ([[0018-per-track-audio-control-model]]), the
brickwall limiter ([[0023-brickwall-limiter]]), and the mic DSP chain
(HPF [[0024-mic-high-pass-filter]] → gate [[0025-mic-noise-gate]] →
AGC [[0026-mic-agc]]). This is the first lossless-audio slice; it extends the
container/codec matrix rather than the mic DSP chain.

## Context

ExoSnap's audio path has shipped two encoded codecs: Opus (libopus, WebM +
Matroska) and AAC-LC (Media Foundation / FDK-AAC, Matroska + MP4). Both are
lossy. Editing/archival workflows and downstream NLEs sometimes want
**uncompressed** audio: no generation loss, deterministic bytes, trivial decode.
The roadmap container/codec matrix (`docs/roadmap.md`, "Final container / codec /
audio matrix") lists PCM (and FLAC) under the 0.6.0 Audio v2 wave and notes that
**PCM-in-MP4** is intentionally deferred — its ISO-BMFF sample-entry variant and
player matrix are unspecified, so a bare "PCM in MP4" is not a real deliverable.

Matroska, by contrast, has a fully specified, widely-supported uncompressed PCM
track type: `A_PCM/INT_LIT` (integer, little-endian) with the bit depth carried
in the track audio header. The capture pipeline already runs at a fixed
**48 kHz, stereo** internal format and converts to interleaved 16-bit PCM in the
AAC path. So a minimal, correct PCM slice is: emit S16LE into an `A_PCM/INT_LIT`
Matroska track. No new third-party dependency is required — PCM is a
sample-format conversion, not a codec.

The `capability::AudioCodec` enum already carried a `Pcm` placeholder (declared
in the product surface, marked `NotImplemented` in the capability builder and
`Experimental` in the container-compat registry). This slice makes it real.

## Decision

Add **PCM** as a selectable audio codec producing uncompressed
**16-bit signed little-endian** audio in a Matroska `A_PCM/INT_LIT` track at the
existing pipeline format (48 kHz, stereo). **PCM is MKV-only**; WebM and MP4
reject it.

### Engine model and encoder

- `recorder_core::AudioCodec` gains `Pcm` (the enum was `{AacMf, Opus}`).
  `capability::AudioCodec::Pcm` ↔ `recorder_core::AudioCodec::Pcm` everywhere the
  two enums are mapped (`RecordingCoordinator::ApplyOutputSettingsToRecorderConfig`,
  `translation.cpp`, the visual-harness scenario mapper, history/label helpers).
- `PcmAudioEncoder` (`libs/recorder_core/src/pcm_audio_encoder.{h,cpp}`)
  implements the existing `IAudioEncoder` interface so the audio thread and mux
  path treat it identically to Opus/AAC. It is a **passthrough**: each
  `FeedFloat32` call converts the interleaved Float32 buffer to interleaved S16LE
  (clamp to [-1, 1], scale by 32767, round-to-nearest via `lround`) and emits
  **one packet per input buffer** with `pts_ns` derived from the running
  frame counter (`accumulated_frames * 1e9 / sample_rate`), identical to the
  Opus/AAC PTS formula. `CodecPrivateBytes()` returns empty (A_PCM/INT_LIT needs
  none), `Flush()` is a no-op (no buffered state), and `Init` rejects zero
  sample-rate/channels. Full-scale +1.0 → +32767 and -1.0 → -32767 (the
  asymmetric -32768 is only reachable below -1.0, which is clamped away),
  matching the existing AAC-path float→PCM16 conversion.
- `audio_thread.cpp` gains a PCM branch mirroring the Opus branch: init the
  encoder, publish an **empty** codec-private and mark the track ready (so the
  pre-mux readiness gate releases), run the capture/encode loop, drain (no-op),
  push the audio EOS sentinel.

### Mux / Matroska writer

- `MatroskaStreamConfig.audio_is_opus` (a bool) is replaced by an explicit
  `StreamAudioCodec { Aac, Opus, Pcm }` discriminator. Opus and AAC behavior is
  byte-identical to before.
- The audio-track builder adds a PCM path: CodecID `A_PCM/INT_LIT`, **no**
  CodecPrivate, and `KaxAudioBitDepth = 16` in the track audio header (alongside
  the existing 48000 Hz / 2-channel fields). `mux_thread.cpp` maps
  `recorder_core::AudioCodec` → `StreamAudioCodec`.

### Validation and capability gating

- `RecorderSession::Validate` accepts `AudioCodec::Pcm` **only** for
  `Container::Matroska`; WebM and MP4 are rejected with `E_INVALIDARG` and a
  clear message.
- `ContainerCompatRegistry`: MKV + (AV1|H.264) + PCM are **Allowed**
  (PCM is video-codec-independent); MKV + HEVC + PCM stays Experimental (HEVC
  video unimplemented); WebM + PCM is **Prohibited**; MP4 + PCM stays
  **Experimental** (sample-entry deferred). `Allowed` maps to
  `ValidUnvalidated` in the CapabilitySet, so PCM is selectable with a
  "not validated on this system" caveat. The capability builder's
  dimension-level annotation for `Pcm` flips `NotImplemented` → `Available`
  (PCM has no NVENC/MF runtime dependency). `translation.cpp` whitelists
  MKV+AV1+PCM and MKV+H264+PCM.

### UI and persistence

- ConfigPage Output card: the audio-codec selector adds
  "PCM (uncompressed)" **only when the container is MKV** (the list is rebuilt on
  container change; switching away from MKV with PCM selected falls back to
  Opus). The existing container-compat callout already flags PCM as invalid for
  WebM/MP4 if a stale preset selects it.
- Preset persistence: `RecordingPresetStore` already serialized `audio_codec` as
  a string; it now round-trips the `"pcm"` value. `kPresetSchemaVersion` is bumped
  **12 → 13** (defensively; the change is additive). `RecordingHistoryStore`
  round-trips `"pcm"` so completed PCM recordings display correctly.
- PCM has no bitrate; the engine simply does not read `audio_bitrate_kbps` for
  the PCM path. The Expert audio bitrate / Opus-specific rows are left inert for
  PCM (they are already non-functional for AAC today — a pre-existing UI
  inconsistency not addressed here).

## Consequences

- ExoSnap can record lossless, uncompressed stereo audio into MKV. Files are
  large (~1.5 Mbit/s per channel: ~11.5 MB/min stereo) — this is inherent to
  uncompressed PCM and surfaced via the "Large files; lossless audio" registry
  reason.
- The `IAudioEncoder` abstraction proved sufficient: PCM slotted in as a third
  `IAudioEncoder` with no changes to the interface, the mux queue, or the
  pre-mux readiness machinery. The `audio_is_opus` bool → `StreamAudioCodec`
  enum makes the writer's codec selection explicit and extensible.
- The PCM encoder is pure (clamp/round/byte-pack only), so it is exhaustively
  unit-tested (full-scale, clamp beyond ±1, round-to-nearest, silence,
  little-endian byte order, PTS/frame-counter advancement, empty CodecPrivate,
  degenerate inputs) with no hardware. Validation, registry, translation,
  stream-writer (A_PCM/INT_LIT + BitDepth=16), and preset round-trip tests cover
  the wiring.

## Deferred

- **FLAC** (lossless compressed) — the other 0.6.0 lossless codec; deferred to a
  later slice.
- **Full channel / sample-format model** — arbitrary sample rate, mono/multichannel,
  and 24-/32-bit depths (`A_PCM/INT_LIT` at other bit depths, `A_PCM/FLOAT_IEEE`).
  This slice is fixed at 48 kHz / stereo / S16LE, matching the pipeline format.
- **PCM in MP4** — ISO-BMFF PCM sample-entry variant and player matrix remain
  unspecified (roadmap note); MP4 + PCM stays Experimental / rejected.
- RNNoise (the remaining mic-DSP stage) — see [[0026-mic-agc]] deferred list.
