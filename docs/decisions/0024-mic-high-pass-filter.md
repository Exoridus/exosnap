# ADR 0024: Microphone High-Pass Filter (Mic DSP chain)

## Status

Accepted — implemented in 0.6.0 (feat/0.6.0-mic-hpf), the third Audio v2 slice
after per-track gain + mute ([[0018-per-track-audio-control-model]]) and the
brickwall limiter ([[0023-brickwall-limiter]]).

## Context

Microphone capture commonly carries low-frequency energy that is not part of the
voice: desk thumps, footsteps, HVAC hum, plosives, and electrical mains rumble.
This content wastes encoder bitrate and muddies the recording without adding any
intelligibility. The standard fix is a high-pass filter on the mic input.

The limiter (ADR 0023) protects the *mixed* output bus. A mic high-pass filter is
a different kind of stage: it must run on the *single* mic source, before mixing,
and it is the first of several planned per-mic processors (noise gate, AGC,
RNNoise). That calls for a dedicated, extensible mic-DSP insertion point rather
than another flag bolted onto `MixedAudioSrc`.

## Decision

Add a 2nd-order Butterworth high-pass biquad (`HighPassFilter`) and a thin
`IAudioCaptureSource` decorator (`MicDspAudioSrc`) that owns the mic-DSP chain.
The decorator wraps the mic capture source and runs the enabled stages on its
audio before the rest of the pipeline sees it.

### DSP model (`libs/recorder_core/src/high_pass_filter.{h,cpp}`)

- RBJ audio-EQ cookbook high-pass coefficients with `Q = 1/sqrt(2)`
  (Butterworth, maximally flat passband), evaluated at `Configure` time.
- Runs in Transposed Direct Form II with two state registers **per channel**
  (up to `kMaxChannels = 8`), so a stereo or multichannel endpoint is filtered
  channel-independently without bleed.
- `Configure` sanitizes degenerate input: `channels` clamped to
  `[1, kMaxChannels]`, `sample_rate >= 1`, `cutoff_hz` reset to 80 Hz when
  non-finite/non-positive and clamped strictly inside `(0, Nyquist)`.
- `Reset` clears the per-channel registers; `Process` is allocation-free and
  `noexcept`. State carries across `Process` calls so a stream can be blocked.

### Mic-DSP chain (`libs/recorder_core/src/mic_dsp_audio_src.{h,cpp}`)

`MicDspAudioSrc` is an `IAudioCaptureSource` decorator constructed from a
`std::unique_ptr<IAudioCaptureSource> inner` and a `MicDspConfig`. `MicDspConfig`
is the **single aggregate of mic-DSP settings** — today just `hpf_enabled` +
`hpf_cutoff_hz` with an `AnyEnabled()` predicate; later slices add gate/AGC/
RNNoise fields here. The decorator:

- Inits the inner source, reads its `SampleRate()`/`Channels()`, configures the
  HPF, and allocates a Float32 scratch buffer.
- Delegates `PendingFrameCount`/`ReleaseBuffer`/`Shutdown`/`SampleRate`/
  `Channels`/`EndpointName` to the inner source. **Always reports `Float32`** as
  its `SampleFormat` (Int16 inner sources are converted up).
- On `AcquireBuffer`: passes silent/empty buffers straight through; otherwise
  converts inner bytes (Int16 via `/32768.0f`, matching `MixedAudioSrc`, or
  Float32) to interleaved Float32 in the scratch buffer **preserving the inner
  channel count**, runs the enabled stages in order, and hands back the scratch
  buffer (valid until `ReleaseBuffer`) with the discontinuity/silent flags
  preserved.

### Integration (`recorder_session`)

The `createAudioSource(Mic)` lambda builds the `WasapiCaptureSrc`, then a
`MicDspConfig` from the new `RecorderConfig` fields, and wraps the mic in
`MicDspAudioSrc` **only when `cfg.AnyEnabled()`**. Because both the single-source
and merged-track paths build the mic through this one lambda, the chain covers
both. A mic with no DSP enabled is left untouched (unaltered capture).

### Config and persistence

- `RecorderConfig` gains `mic_hpf_enabled` (default **false**) +
  `mic_hpf_cutoff_hz` (default 80 Hz). Mic DSP alters captured audio, so unlike
  the limiter it is **opt-in**.
- `capability::AudioUiState` and `AudioPlanResult` carry the same two fields;
  `BuildAudioPlan` passes them through; `RecordingCoordinator` copies them onto
  `RecorderConfig`.
- The TOML preset store persists `mic_hpf_enabled` + `mic_hpf_cutoff_hz` in the
  `[audio]` table; `kPresetSchemaVersion` is bumped to 10. Older presets default
  to disabled / 80 Hz (no behavior change vs unfiltered capture).
  `SanitizePresetConfig` resets a non-finite cutoff to 80 Hz and clamps to
  `[20, 1000]` Hz. `NormalizedConfigEquals` / `ConfigDirtyEquivalent` compare
  `mic_hpf_enabled` (exact) and `mic_hpf_cutoff_hz` (1e-2 Hz tolerance).
- Settings → Audio (Expert) gains a "High-pass filter" toggle and an "HPF
  cutoff" spin box (20–1000 Hz, 5 Hz steps), mirroring the limiter rows; the
  cutoff control is enabled only when the filter is on.

## Consequences

- Microphone recordings can drop low-frequency rumble before it reaches the
  encoder, saving bitrate and cleaning up the result.
- `MicDspAudioSrc` establishes the **single extension point** for the rest of the
  mic processing chain. Noise gate, AGC, and RNNoise become new `MicDspConfig`
  fields and new stages run in order inside `AcquireBuffer` — no new decorators,
  no new integration wiring.
- The DSP is pure and dependency-free, so it is exhaustively unit-tested (DC/low
  attenuation, cutoff −3 dB point, passband near unity, stability, sanitization,
  per-channel independence, Reset) without hardware. The decorator has its own
  pass-through / format / attenuation tests against a mock source.
- The filter is fixed at 2nd-order Butterworth (12 dB/octave). A steeper or
  selectable slope, and a live preview of the filtered mic, are not built.

## Deferred

Mic noise gate, AGC, and RNNoise (further `MicDspAudioSrc` stages), PCM/FLAC
codecs, and the channel/sample-format model remain the rest of the Audio v2
(0.6.0) wave (see [[0023-brickwall-limiter]] deferred list).
