# ADR 0030: Channel / Sample-Format Model

## Status

Accepted — implemented in 0.6.0 (feat/0.6.0-audio-format-model), the headline Audio v2 slice
that completes the wave. **Supersedes** the deferrals recorded in
[[0019-audio-format-model]] ("48 kHz is fixed; 44.1 kHz is a post-1.0 item") and
[[0028-flac-audio-codec]] ("full channel/sample-format model" and "configurable
compression level" deferred). Builds on [[0027-pcm-audio-codec]] and [[0028-flac-audio-codec]].

## Context

Through 0.5.0 and the first 0.6.0 Audio v2 slices, the entire audio path was hardcoded to a single
internal format: **48 kHz, stereo, Float32**, with PCM and FLAC fixed at **16-bit**. ADR 0019
documented this as "the only supported input format; no resampling or format negotiation occurs at
the engine level." Concretely (mapped during the architecture pass):

- `MixedAudioSrc` mixes every source to `kOutputSampleRate=48000` / `kOutputChannels=2` / Float32.
  Its `ConvertToFloat32Stereo` does **channel down-/up-mix only** (mono→dup, >2→first two) and an
  Int16→Float32 scale. **It performs no sample-rate conversion** — it assumes every source already
  delivers 48 kHz (true in practice: WASAPI shared-mode capture runs at the 48 kHz device mix rate).
- `PcmAudioEncoder` (`kBitDepth=16`) and `FlacAudioEncoder` (`kBitsPerSample=16`,
  `kCompressionLevel=5`) convert Float32 → interleaved S16.
- `matroska_stream_writer.cpp` writes `KaxAudioSamplingFreq=48000`, `KaxAudioChannels=2`, and (for
  PCM/FLAC) `KaxAudioBitDepth=16` — all literal constants.

This is correct and lossless-capable, but it leaves no room for the legitimate recording use cases
the roadmap's final audio matrix calls for: 44.1 kHz delivery for music/NLE workflows, 96 kHz for
high-rate capture, mono recordings (smaller, common for voice), and 24-/32-bit lossless for
archival masters. The roadmap lists the **channel/sample-format model** under the 0.6.0 Audio v2
wave.

The enabler is already present: **`FFmpeg::swresample`** is linked and its DLL shipped (for the MP4
remux path, ADR 0014). It gives us sample-accurate resampling and channel rematrixing without a new
dependency.

## Decision

Introduce a single first-class **`AudioFormat`** value — `{ sample_rate, channels, bit_depth }` —
that flows source → mixer → output stage → encoder → mux, replacing the hardcoded constants. The
offered values are a **vetted, codec-gated set** (per the roadmap rule "the UI must only offer
vetted combinations"), not arbitrary integers.

### Vetted value sets

| Dimension | Offered values | Default | Gating |
|---|---|---|---|
| Sample rate | 44100, 48000, 96000 Hz | 48000 | **Opus → 48000 only** (libopus accepts only 8/12/16/24/48 kHz; 44.1/96 are not Opus input rates). AAC/PCM/FLAC: all three. |
| Channels | Mono (1), Stereo (2) | Stereo | All codecs. **5.1+ deferred** (capture side: WASAPI loopback/mic deliver mono/stereo; multichannel upmix is not a real source). |
| Bit depth | 16, 24, 32-bit signed int | 16 | **Lossless only** — PCM: 16/24/32; FLAC: 16/24 (libFLAC caps at 24-bit native). Lossy (Opus/AAC): N/A — bit depth is internal to the codec. |

The capture side stays **48 kHz Float32** (WASAPI shared mode + RNNoise's hard 48 kHz requirement).
All DSP (HPF/gate/AGC/RNNoise) and the mixer/limiter continue to run at 48 kHz/Float32. Conversion
to the target format happens **once, after the mix bus**, immediately before the encoder.

### Where resampling / rematrixing happens

A new **`OutputFormatAudioSrc`** decorator (`IAudioCaptureSource`) wraps each track's source
(typically the `MixedAudioSrc`). It owns one `SwrContext` and converts the 48 kHz/stereo/Float32 mix
to `{ sample_rate, channels }` Float32, reporting the target `SampleRate()`/`Channels()` downstream.

- **Decorator, not mixer surgery.** `MixedAudioSrc` is left at 48 kHz/stereo internally — the
  limiter, gain/mute, and silence-padding logic are unchanged. The new stage is composable and unit
  testable in isolation, mirroring the `MicDspAudioSrc` decorator pattern.
- **No-op fast path.** When the target is exactly 48 kHz/stereo (the default), the decorator is a
  passthrough — byte-identical to today, so existing behavior and the entire test corpus are
  preserved.
- **swresample owns both rate and channel conversion** in one `swr_convert` (it rematrixes
  stereo↔mono with the standard ITU downmix coefficients). Variable output frame counts per call
  are expected and handled (swr buffers internally), exactly like the FLAC/AAC encoders already
  produce a variable packet count.
- **Bit depth is the encoder's job, not the resampler's.** The decorator always emits Float32; the
  PCM/FLAC encoders convert Float32 → S16/S24/S32. This keeps the resampler format-agnostic and the
  one float→int rounding/clamping rule in the encoders (where it already lives).

PTS is computed from accumulated frames at the **target** sample rate (the decorator reports it),
which is sample-accurate because swresample conserves sample count across the conversion.

### Encoder changes

`IAudioEncoder::Init(sample_rate, channels, …)` already carries rate/channels — only the codec
bodies hardcoded assumptions need lifting:

- **PCM** (`PcmAudioEncoder`): `bit_depth ∈ {16,24,32}`. Float32 → S16LE / S24LE (packed 3-byte) /
  S32LE. Matroska CodecID stays `A_PCM/INT_LIT`; `KaxAudioBitDepth` follows the chosen depth.
- **FLAC** (`FlacAudioEncoder`): `bit_depth ∈ {16,24}` set as libFLAC `bits_per_sample`; Float32 →
  S16/S24 in `FLAC__int32` samples. **Compression level becomes configurable** `[0,8]` (default 5)
  — resolving the [[0028-flac-audio-codec]] deferral; lossless at every level, level only trades
  encode CPU vs. size.
- **Opus**: already 48 kHz-native; the format model locks Opus to 48 kHz and passes channels
  through (libopus handles mono/stereo natively). No bit depth.
- **AAC** (FDK/MF): accepts the chosen rate/channels; no bit depth. (MF AAC's prior "must be 48 kHz
  stereo" assumption is lifted to the negotiated rate/channels.)

### Mux

`matroska_stream_writer` takes the `AudioFormat` per audio track and writes
`KaxAudioSamplingFreq`/`KaxAudioChannels` from it; `KaxAudioBitDepth` from `bit_depth` for PCM/FLAC.

### MP4 PCM

**Deferred (Experimental) for 0.6.0.** Live verification with the project's FFmpeg (avformat-62,
the same lib the remux path uses) shows that libavformat writes the **`ipcm`** (ISO/IEC 23003-5)
sample entry for `pcm_s16le`/`pcm_s24le`/`pcm_s32le` in MP4, confirmed via `ffprobe`
(`codec_tag_string=ipcm`). `ipcm` is a very recent standard with limited player support — Windows
"Films & TV", QuickTime, and many NLEs do not play it. The assumption that libavformat would
choose the broadly-compatible QuickTime sample entries (`sowt`/`in24`/`lpcm`) did not hold.

Per ADR 0030's rule ("if a depth proves fragile it is narrowed back to Experimental with a logged
reason rather than shipped silently"), **MP4 + PCM remains Experimental**: it is not
user-selectable, `Validate` rejects it, and `ReconcileContainerCodecs` forces AAC for MP4 when
PCM is selected. The remuxer itself stays codec-agnostic (no change needed there). A future wave
can re-enable MP4 PCM by forcing a broadly-compatible sample-entry mapping (e.g. `sowt`/`in24`)
and publishing a real player compatibility matrix. **FLAC-in-MP4 stays deferred**
(Experimental/rejected) per [[0028-flac-audio-codec]]. **MKV remains PCM's home.**

### Config / model surface

- `recorder_core::RecorderConfig` gains `audio_sample_rate`, `audio_channels`, `audio_bit_depth`,
  `flac_compression_level`.
- `capability::AudioUiState` + `AudioPlanResult` gain the same four fields; `BuildAudioPlan` passes
  them through; the coordinator maps them into `RecorderConfig`.
- Sanitization clamps each field to its codec-gated vetted set (e.g. selecting Opus snaps sample
  rate to 48 kHz; selecting a lossy codec hides/ignores bit depth; switching off MKV reconciles as
  today).

### UI

ConfigPage Output/Audio card gains, codec- and container-gated:
- **Sample rate** selector (48 default; disabled/locked to 48 with an info hint when codec is Opus).
- **Channels** selector (Stereo default; Mono).
- **Bit depth** selector (visible only for PCM/FLAC; 16/24[/32 for PCM]).
- **FLAC compression level** slider 0–8 (visible only for FLAC).

Controls follow the established Expert/info-hint pattern and are disabled during recording.

### Preset persistence

`kPresetSchemaVersion` bumps **15 → 16**. The four new fields round-trip in the TOML
(`[audio]` section: `sample_rate`, `channels`, `bit_depth`, `flac_compression_level`). Pre-1.0
policy: incompatible older preset files are reset, not migrated.

## Consequences

- ExoSnap records at 44.1/48/96 kHz, mono or stereo, and 16/24/32-bit lossless — the audio matrix
  the roadmap promised, with the capture side and RNNoise unchanged at 48 kHz.
- The `IAudioCaptureSource` decorator pattern absorbed resampling cleanly; `MixedAudioSrc` and the
  DSP chain did not change, so their tests and behavior are preserved. The default path is a
  byte-identical no-op.
- One new runtime DLL surface is exercised (`swresample`, already shipped) — no new dependency, no
  packaging change.
- The vetted, codec-gated matrix keeps invalid combinations (Opus@96 kHz, FLAC@32-bit,
  multichannel) unrepresentable rather than blocked-after-the-fact.

## Deferred

- **MP4 + PCM** — libavformat emits `ipcm` (ISO/IEC 23003-5) rather than the broadly-compatible
  QuickTime sample entries (`sowt`/`in24`/`lpcm`), confirmed by `ffprobe codec_tag_string=ipcm`.
  Narrowed back to Experimental per ADR 0030's own rule; re-enable in a future wave once a
  broadly-compatible sample-entry mapping is forced and validated against a real player matrix.
  MKV is PCM's home for now.
- **>2 channels (5.1/7.1)** — no real multichannel source today; pure upmix is not worth shipping.
- **Float PCM** (`A_PCM/FLOAT_IEEE`, 32-bit float) — integer S32 covers the archival case; float
  PCM is a later niche.
- **FLAC-in-MP4** — unchanged from [[0028-flac-audio-codec]]; MKV is FLAC's home.
- **Arbitrary (non-vetted) sample rates** — deliberately not exposed.
