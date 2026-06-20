# ADR 0025: Microphone Noise Gate (Mic DSP chain stage 2)

## Status

Accepted — implemented in 0.6.0 (feat/0.6.0-mic-gate), the fourth Audio v2 slice
after per-track gain + mute ([[0018-per-track-audio-control-model]]), the
brickwall limiter ([[0023-brickwall-limiter]]), and the microphone high-pass
filter ([[0024-mic-high-pass-filter]]).

## Context

Even after the high-pass filter removes low-frequency rumble, a microphone keeps
picking up steady mid/high-level background noise between phrases: keyboard
clatter, fan and HVAC noise, room tone, and chair creaks. Listeners hear this as
a constant hiss/bed under the voice. The standard fix is a downward noise gate:
attenuate the mic toward silence when its level falls below a threshold, and pass
it through when the speaker is talking.

The high-pass filter (ADR 0024) established `MicDspAudioSrc` as the single,
extensible insertion point for per-mic processing, owning the chain through one
`MicDspConfig` aggregate. The noise gate is the **second stage of that chain** —
it should reuse the same decorator and config aggregate rather than introducing a
new wrapper.

## Decision

Add a stereo-linked downward noise gate (`NoiseGate`) as the second stage of the
`MicDspAudioSrc` chain, after the high-pass filter.

### DSP model (`libs/recorder_core/src/noise_gate.{h,cpp}`)

- **Stereo-linked detection**: the gate decision uses the loudest channel of each
  frame (`max(|sample|)` across channels, like the brickwall limiter), and one
  shared gain is applied to every channel. This preserves the stereo image and
  keeps channels opening/closing together (no L/R flutter).
- **Gate logic**: the gate target is open (1.0) when the per-frame level is at or
  above the linear threshold, closed (0.0) otherwise. A **hold timer** keeps the
  gate open for `hold_ms` after the level last exceeded the threshold so the tail
  of speech is not chopped and the gate does not chatter around the threshold.
- **Envelope**: the applied gain is smoothed with a one-pole envelope — a fast
  `attack` coefficient when opening toward unity, a slow `release` coefficient
  when closing toward silence (same shape as the limiter's attack/release).
- `Config` carries `threshold_db` (default −45 dB), `attack_ms` (2 ms),
  `hold_ms` (120 ms), `release_ms` (150 ms), `sample_rate`, `channels`.
- `Configure` sanitizes degenerate input: `channels` clamped to
  `[1, kMaxChannels]`, `sample_rate >= 1`, `threshold_db` reset to −45 dB when
  non-finite; it precomputes `threshold_linear = pow(10, threshold_db / 20)` and
  the attack/release coefficients.
- `Reset` clears the smoothed gain (closed) and the hold timer; `Process` is
  allocation-free and `noexcept`. State carries across `Process` calls so a
  stream can be processed in blocks. `CurrentGain()` is exposed for
  tests/metering.

### Mic-DSP chain (`mic_dsp_audio_src`)

`MicDspConfig` gains `gate_enabled` (default false) + `gate_threshold_db`
(default −45 dB), and `AnyEnabled()` becomes `hpf_enabled || gate_enabled`.
`MicDspAudioSrc` owns a `NoiseGate` member, configures it in `Init` from the
inner source's sample rate/channels and `gate_threshold_db`, and resets it. In
`AcquireBuffer` the stages run **in order: high-pass filter first, then the
noise gate** — the HPF strips low-frequency rumble before the gate so the rumble
cannot hold the gate open.

### Integration, config and persistence

- `recorder_session::createAudioSource(Mic)` sets `dsp.gate_enabled` +
  `dsp.gate_threshold_db` from the new `RecorderConfig` fields; the existing
  `AnyEnabled()` wrap covers both the single-source and merged-track paths.
- `RecorderConfig` gains `mic_gate_enabled` (default **false**) +
  `mic_gate_threshold_db` (default −45 dB). Mic DSP alters captured audio, so
  like the HPF it is **opt-in**.
- `capability::AudioUiState` and `AudioPlanResult` carry the same two fields;
  `BuildAudioPlan` passes them through; `RecordingCoordinator` copies them onto
  `RecorderConfig`.
- The TOML preset store persists `mic_gate_enabled` + `mic_gate_threshold_db` in
  the `[audio]` table; `kPresetSchemaVersion` is bumped to 11. Older presets
  default to disabled / −45 dB (no behavior change vs ungated capture).
  `SanitizePresetConfig` resets a non-finite threshold to −45 dB and clamps to
  `[-80, 0]` dB. `NormalizedConfigEquals` / `ConfigDirtyEquivalent` compare
  `mic_gate_enabled` (exact) and `mic_gate_threshold_db` (1e-2 dB tolerance).
- Settings → Audio (Expert) gains a "Noise gate" toggle and a "Gate threshold"
  spin box (−80…0 dB, 1 dB steps), mirroring the HPF rows; the threshold control
  is enabled only when the gate is on.

## Consequences

- Microphone recordings can drop steady background noise between phrases, on top
  of the rumble already removed by the high-pass filter.
- The chain order is now **HPF → gate**, both owned by `MicDspAudioSrc`. Adding
  AGC and RNNoise stays a matter of new `MicDspConfig` fields and new stages run
  in order inside `AcquireBuffer` — no new decorators, no new integration wiring.
- The DSP is pure and dependency-free, so it is exhaustively unit-tested (loud
  pass-through, quiet attenuation, hold behaviour, stereo linkage, sanitization,
  Reset) without hardware. The decorator has its own gate-attenuation and
  chain-order (HPF→gate) tests against a mock source.
- The gate's attack/hold/release times are fixed defaults tuned for speech; only
  the threshold is user-exposed. A configurable range/hysteresis ("soft" gate
  ratio) and a live preview of the gated mic are not built.

## Deferred

Mic AGC and RNNoise (further `MicDspAudioSrc` stages), PCM/FLAC codecs, and the
channel/sample-format model remain the rest of the Audio v2 (0.6.0) wave (see
[[0024-mic-high-pass-filter]] deferred list).
