# ADR 0026: Microphone Automatic Gain Control (Mic DSP chain stage 3)

## Status

Accepted — implemented in 0.6.0 (feat/0.6.0-mic-agc), the fifth Audio v2 slice
after per-track gain + mute ([[0018-per-track-audio-control-model]]), the
brickwall limiter ([[0023-brickwall-limiter]]), the microphone high-pass filter
([[0024-mic-high-pass-filter]]), and the microphone noise gate
([[0025-mic-noise-gate]]).

## Context

After the high-pass filter removes rumble and the noise gate silences the bed
between phrases, the surviving voice still varies in level: a quiet talker, an
inconsistent mic distance, or a soft passage all end up under-recorded, while a
loud burst sits near the ceiling. The standard fix is automatic gain control
(AGC): track the program level over time and apply a slowly-varying makeup gain
that drives the level toward a target loudness — boosting the quiet, attenuating
the loud — so the recorded voice stays at a consistent level.

The noise gate (ADR 0025) extended `MicDspAudioSrc` to a two-stage chain
(HPF → gate) owned by one `MicDspConfig` aggregate. AGC is the **third stage of
that chain** — it reuses the same decorator and config aggregate rather than
introducing a new wrapper.

## Decision

Add a stereo-linked automatic gain control (`AutomaticGainControl`) as the third
stage of the `MicDspAudioSrc` chain, after the high-pass filter and the noise
gate.

### DSP model (`libs/recorder_core/src/automatic_gain_control.{h,cpp}`)

- **Stereo-linked detection**: a smoothed level envelope is derived from the
  loudest channel of each frame (`max(|sample|)` across channels, one-pole
  envelope follower, like the gate and limiter), and one shared makeup gain is
  applied to every channel. This preserves the stereo image and keeps channels
  tracking together (no L/R drift).
- **Gain logic**: when the envelope is at or above the linear noise floor, the
  desired gain is `target_linear / max(env, tiny)`, clamped to
  `[min_gain, max_gain]` where `max_gain = pow(10, max_gain_db / 20)` and
  `min_gain = pow(10, -max_gain_db / 20)` (attenuation symmetric to boost). The
  applied gain is smoothed toward the desired value with a one-pole envelope — a
  fast `attack` coefficient when the gain rises (boosting), a slow `release`
  coefficient when it falls (attenuating).
- **Noise-floor freeze**: when the envelope falls below `noise_floor_db` the gain
  is **frozen** (held at its current value) — the AGC must not crank its gain up
  while the input is near-silence/noise. Without this freeze the gain would run
  away on a quiet tail and then slam the level on the next loud sample, and it
  would amplify whatever residual noise survived the gate. Because the gate runs
  *before* the AGC, the AGC sees an already-cleaned signal; the freeze is the
  belt-and-suspenders guarantee that it never pumps up a near-silent input.
- `Config` carries `target_db` (default −18 dB), `max_gain_db` (30 dB),
  `attack_ms` (50 ms), `release_ms` (400 ms), `noise_floor_db` (−55 dB),
  `sample_rate`, `channels`.
- `Configure` sanitizes degenerate input: `channels` clamped to
  `[1, kMaxChannels]`, `sample_rate >= 1`, every `*_db` reset to its default when
  non-finite, `max_gain_db` made non-negative; it precomputes the linear
  target/floor/max/min gains and the envelope/attack/release coefficients.
- `Reset` sets the makeup gain to unity (no makeup until the level is known) and
  clears the envelope; `Process` is allocation-free and `noexcept`. State carries
  across `Process` calls so a stream can be processed in blocks. `CurrentGain()`
  is exposed for tests/metering.

### Mic-DSP chain (`mic_dsp_audio_src`)

`MicDspConfig` gains `agc_enabled` (default false) + `agc_target_db`
(default −18 dB), and `AnyEnabled()` becomes
`hpf_enabled || gate_enabled || agc_enabled`. `MicDspAudioSrc` owns an
`AutomaticGainControl` member, configures it in `Init` from the inner source's
sample rate/channels and `agc_target_db` (defaults for the rest), and resets it.
In `AcquireBuffer` the stages run **in order: high-pass filter, then noise gate,
then AGC** — AGC is last so it normalizes the already-cleaned signal (the gate
has removed the noise bed, so the AGC won't pump it up).

### Integration, config and persistence

- `recorder_session::createAudioSource(Mic)` sets `dsp.agc_enabled` +
  `dsp.agc_target_db` from the new `RecorderConfig` fields; the existing
  `AnyEnabled()` wrap covers both the single-source and merged-track paths.
- `RecorderConfig` gains `mic_agc_enabled` (default **false**) +
  `mic_agc_target_db` (default −18 dB). Mic DSP alters captured audio, so like
  the HPF and gate it is **opt-in**.
- `capability::AudioUiState` and `AudioPlanResult` carry the same two fields;
  `BuildAudioPlan` passes them through; `RecordingCoordinator` copies them onto
  `RecorderConfig`.
- The TOML preset store persists `mic_agc_enabled` + `mic_agc_target_db` in the
  `[audio]` table; `kPresetSchemaVersion` is bumped to 12. Older presets default
  to disabled / −18 dB (no behavior change vs un-AGC'd capture).
  `SanitizePresetConfig` resets a non-finite target to −18 dB and clamps to
  `[-40, 0]` dB. `NormalizedConfigEquals` / `ConfigDirtyEquivalent` compare
  `mic_agc_enabled` (exact) and `mic_agc_target_db` (1e-2 dB tolerance).
- Settings → Audio (Expert) gains an "Automatic gain control" toggle and an
  "AGC target level" spin box (−40…0 dB, 1 dB steps), mirroring the gate rows;
  the target control is enabled only when the AGC is on. The old "AGC"
  v0.6 placeholder row is removed (the feature is now real); the remaining
  mic-DSP placeholder is **RNNoise**.

## Consequences

- Microphone recordings keep the voice at a consistent loudness across quiet and
  loud passages, on top of the rumble (HPF) and noise bed (gate) already removed.
- The chain order is now **HPF → gate → AGC**, all owned by `MicDspAudioSrc`.
  Adding RNNoise stays a matter of a new `MicDspConfig` field and a new stage run
  in order inside `AcquireBuffer` — no new decorators, no new integration wiring.
- The DSP is pure and dependency-free, so it is exhaustively unit-tested (quiet
  boost toward target, loud attenuation toward target, max-gain clamp,
  noise-floor freeze / no-runaway, sanitization, Reset) without hardware. The
  decorator has its own AGC-boost and full-chain (HPF→gate→AGC, no-blowup) tests
  against a mock source.
- The AGC's attack/release/max-gain/noise-floor are fixed defaults tuned for
  speech; only the target level is user-exposed. A configurable
  attack/release/range, a fast-acting peak limiter coupling, and a live preview
  of the AGC'd mic are not built (the brickwall limiter already guards the
  ceiling on the mixed output).

## Deferred

RNNoise (the remaining `MicDspAudioSrc` stage), PCM/FLAC codecs, and the
channel/sample-format model remain the rest of the Audio v2 (0.6.0) wave (see
[[0024-mic-high-pass-filter]] deferred list).
