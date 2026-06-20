# ADR 0023: Brickwall Limiter for Mixed Audio

## Status

Accepted — implemented in 0.6.0 (feat/0.6.0-audio-limiter), the second Audio v2
slice after per-track gain + mute ([[0018-per-track-audio-control-model]]).

## Context

0.6.0 introduced per-track gain (up to +24 dB) and the merging of multiple
sources (APP + SYS + MIC) into a single output track
(ADR 0018). Both operations can push the summed signal past full scale
(0 dBFS). Until now `MixedAudioSrc` resolved overs with a naive per-sample hard
clamp to `[-1, 1]` — i.e. digital clipping, which is harsh, irreversible
distortion.

A limiter is the standard, low-risk way to make boosted/summed audio safe: it
reduces gain smoothly around peaks so the signal stays under a ceiling and the
result sounds clean, instead of chopping the waveform.

## Decision

Add a stereo-linked feedforward peak limiter (`BrickwallLimiter`) and apply it to
the mixed Float32 output inside `MixedAudioSrc`, replacing the hard clamp when
enabled.

### DSP model (`libs/recorder_core/src/brickwall_limiter.{h,cpp}`)

Per interleaved frame:

1. Detect a **stereo-linked** peak: `peak = max(|ch|)` across channels, so the
   stereo image is not shifted by per-channel gain differences.
2. `target = (peak > ceiling) ? ceiling / peak : 1.0`.
3. Smooth the envelope with a one-pole filter: **attack** coefficient when the
   target asks for more reduction (`target < gain`), **release** coefficient
   when recovering toward unity. `gain = target + (gain - target) * coeff`.
4. Apply `gain` to every channel, then a final per-sample clamp to the ceiling.

The final clamp is the **brickwall guarantee**: without lookahead the envelope
can lag the very first sample of a sudden transient; the clamp catches that one
residual sample. Below the ceiling the envelope returns to unity (1.0) and the
limiter is transparent. Envelope state is carried across `Process()` calls so a
stream can be processed block by block; `Reset()` clears it.

Defaults: attack 1 ms, release 80 ms, 48 kHz, stereo. Degenerate config
(non-positive ceiling, zero channels/sample-rate) is sanitized on `Configure`.

### Integration (`MixedAudioSrc`)

`MixedAudioSrc` gains an optional `limiter_enabled` + `limiter_ceiling_linear`
constructor pair (default disabled → legacy hard-clamp behavior preserved for
existing callers and tests). When enabled it configures the limiter in `Init`
and runs `limiter_.Process(mix_buffer_, kMixFrameCount)` in place of the clamp
loop. The limiter therefore covers exactly the paths where our processing can
create overs: merged tracks and any single source with non-unity per-row gain
(those are already routed through `MixedAudioSrc`). A unity-gain single source
that bypasses `MixedAudioSrc` is left untouched — it is unaltered capture, not
something we can clip.

### Config and persistence

- `RecorderConfig` gains `audio_limiter_enabled` (default true) +
  `audio_limiter_ceiling_db` (default 0.0 dBFS). `recorder_session` converts the
  dBFS ceiling to linear via `LimiterCeilingDbToLinear` and passes both to every
  `MixedAudioSrc` it builds.
- `capability::AudioUiState` and `AudioPlanResult` carry the same two fields;
  `BuildAudioPlan` passes them through; `RecordingCoordinator` copies them onto
  `RecorderConfig`.
- The TOML preset store persists `limiter_enabled` + `limiter_ceiling_db` in the
  `[audio]` table; `kPresetSchemaVersion` is bumped to 9. Older presets default
  to enabled / 0.0 dBFS. `SanitizePresetConfig` resets non-finite ceilings to
  0.0 and clamps to `[-60, 0]` dBFS. `NormalizedConfigEquals` /
  `ConfigDirtyEquivalent` compare `limiter_enabled` (exact) and
  `limiter_ceiling_db` (1e-2 dB tolerance).

### Default = enabled at 0.0 dBFS

Enabling the limiter at a 0 dBFS ceiling reproduces the previous clamp ceiling
exactly, so levels are unchanged for any mix that did not already exceed full
scale. The only behavioral change is that overs are now limited smoothly instead
of hard-clipped — strictly an improvement. Users can lower the ceiling for
headroom or disable the limiter in Settings → Audio (Expert).

## Consequences

- Per-track gain and source merging no longer hard-clip; the recorder ships a
  clean, configurable peak ceiling.
- The DSP is pure and dependency-free, so it is exhaustively unit-tested
  (ceiling guarantee, transparency, attack/release, stereo linking, mono,
  sanitization) without hardware.
- Lookahead is intentionally omitted (one-sample residual handled by the clamp),
  keeping the implementation simple and allocation-free on the audio path. True
  lookahead / true-peak (inter-sample) limiting can be added later if needed.
- The limiter only protects `MixedAudioSrc` output. A future channel/sample-
  format model or a universal post-mix bus could extend it to all tracks; this
  is deferred (see [[0018-per-track-audio-control-model]] deferred list).

## Deferred

Mic AGC, noise gate, high-pass filter, RNNoise, PCM/FLAC codecs, and the
channel/sample-format model remain the rest of the Audio v2 (0.6.0) wave.
