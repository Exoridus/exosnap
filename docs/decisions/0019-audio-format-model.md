# ADR 0019: Audio Format Model

## Status

Accepted — Opus bitrate/frame-duration/complexity and AAC bitrate controls implemented in 0.5.0
(feat/0.5.0-audio-encoding).

## Context

Prior to 0.5.0, audio encoding parameters were entirely hardcoded inside the encoder
implementations: Opus ran at libopus defaults (~96 kbps auto VBR for mono, ~128 kbps for stereo)
with a fixed 20 ms / 960-sample frame size and complexity 10; FDK-AAC ran at 192 kbps CBR; MF AAC
ran at ~192 kbps (derived from a hardcoded `MF_MT_AUDIO_AVG_BYTES_PER_SECOND`). There was no
user-visible or preset-level control of any of these values.

Recording-focused users have legitimate reasons to tune audio quality vs. file size, and to trade
encoder latency (Opus frame size) against CPU usage. The 0.5.0 roadmap calls for exposing
configurable audio encoding with sensible defaults.

## Internal pipeline

All audio paths capture at 32-bit float, 48 kHz, stereo (F32 48kHz stereo) and deliver
interleaved samples to the encoder. This is the only supported input format; no resampling or
format negotiation occurs at the engine level. The encoder is responsible for any internal
downmix (e.g., FDK-AAC converts stereo to mono CBR when the bitrate is very low).

## Decision

### Audio encoding parameters

Three new user-configurable parameters are added to `AudioUiState`, `AudioPlanResult`, and
`RecorderConfig`:

| Field | Type | Default | Range |
|---|---|---|---|
| `audio_bitrate_kbps` | `uint32_t` | 160 (UI) / 0 (engine) | Opus [32, 510]; AAC [64, 320] |
| `opus_frame_duration` | `OpusFrameDuration` | `Ms20` (20 ms) | Ms20 / Ms10 / Ms5 / Ms2_5 |
| `opus_complexity` | `int` | 10 | [0, 10] |

The engine default of 0 for `audio_bitrate_kbps` means "use the codec's built-in default". This
sentinel is resolved per-encoder inside the encoder implementation, never in the UI or coordinator.

### Opus parameters

- **Bitrate**: VBR mode (`OPUS_SET_VBR(1)`). Target bitrate set via `OPUS_SET_BITRATE(bps)`.
  Range: [32, 510] kbps. Default 160 kbps. 0 in `RecorderConfig` = libopus auto (~96–128 kbps).
- **Frame duration**: Controls the window over which Opus operates. Shorter frames reduce encoding
  latency at the cost of slightly lower quality-per-bit and higher CPU usage.
  - 20 ms = 960 samples (default) — best quality/bit, lowest CPU, 20 ms latency
  - 10 ms = 480 samples — 10 ms latency, modest quality reduction
  - 5 ms = 240 samples — 5 ms latency, noticeable quality reduction at low bitrates
  - 2.5 ms = 120 samples — 2.5 ms latency, not recommended for normal recordings
- **Complexity**: `OPUS_SET_COMPLEXITY(n)`, range [0, 10]. 10 = maximum quality / highest CPU.
  Default 10 — hardware-encoded video is the CPU bottleneck, not Opus at 10.
- **Application**: `OPUS_APPLICATION_AUDIO` — optimised for general audio/music fidelity, not
  voice (`OPUS_APPLICATION_VOIP`).

PTS math is frame-size-aware: `pts_ns = (accumulated_frames * 1_000_000_000) / sample_rate`
where `accumulated_frames` is incremented by `frame_size_samples` per Opus packet. This is
correct for all supported frame sizes.

### AAC parameters (FDK-AAC and MF AAC)

- **Bitrate**: set via `AACENC_BITRATE` (FDK-AAC) or `MF_MT_AUDIO_AVG_BYTES_PER_SECOND` (MF).
  Range: [64, 320] kbps. Default 192 kbps.
- Frame size is fixed at 1024 samples per AAC frame (mandated by the AAC standard). Not
  user-configurable.
- `opus_frame_duration` and `opus_complexity` are ignored when the active audio codec is AAC.

### Preset persistence (schema version 5)

The three new fields are persisted in `presets.ini` under the keys:
- `aud_audio_bitrate_kbps` (integer, kbps)
- `aud_opus_frame_duration` (string: "20ms" / "10ms" / "5ms" / "2.5ms")
- `aud_opus_complexity` (integer)

Schema version bumped 4 → 5. Older preset files are silently reset to defaults.

### UI placement

The three controls appear in the Audio Settings panel on the Record page, below the mic gain row
and above the Resulting Tracks section. The controls are disabled during active recording.
`opus_frame_duration` and `opus_complexity` are always visible; they are Opus-specific but are
kept visible regardless of the active audio codec to avoid layout shifts on codec changes.

## Consequences

- Opus default bitrate changes from libopus auto (~96–128 kbps) to 160 kbps VBR for all new
  recordings. Existing recordings (before 0.5.0) are unaffected.
- Preset schema version 5 is incompatible with schema version 4; preset files from 0.4.x and
  earlier are silently reset on first launch.
- The engine (`recorder_core`) is codec-agnostic: `RecorderConfig::audio_bitrate_kbps` is passed
  to the encoder implementations; the codec-specific clamping and default resolution happens
  inside each encoder, not in the coordinator or UI layer.
- AAC bitrate control is wired to FDK-AAC and MF AAC but both use the same `audio_bitrate_kbps`
  field; the UI does not need to know which AAC backend is active.

## Forward: 0.6.0

- Per-track audio encoding settings (e.g., different bitrate for app vs. mic track) — deferred.
- AAC-specific UI controls (no frame-duration/complexity for AAC) — deferred; current controls
  simply have no visible effect for the non-Opus parameters when AAC is active.
- Sampling-rate selection (48 kHz is fixed; 44.1 kHz support is a post-1.0 item).
