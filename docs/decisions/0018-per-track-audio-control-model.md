# ADR 0018: Per-Track Audio Gain and Mute Model

## Status

Accepted — per-source-row gain + mute implemented in 0.6.0
(feat/0.6.0-audio-pertrack).

## Context

ExoSnap exposes audio via source rows (APP, SYS, MIC). Each row can be
independently enabled, and adjacent rows can be merged into a single output
track. Before 0.6.0, every source within a mix contributed with uniform weight
— there was no way to boost or attenuate individual sources, and no per-source
mute control. The global `mic_gain_linear` field on `AudioUiState` existed as a
one-off workaround for microphone level adjustment, but it was not consistent
with a general gain model and could not be extended to APP or SYS rows without
duplication.

## Decision

### Model

Add `gain_db` (`float`, default `0.0`) and `muted` (`bool`, default `false`) to
`recorder_core::AudioSourceRow`. These are the primary user-visible controls.

The domain for `gain_db` is `[-60.0, +24.0]` dB (enforced by
`SanitizePresetConfig`). At 0 dB the behavior is byte-identical to the previous
version (unity gain, unmuted).

### dB-to-linear conversion

A single pure free function `GainDbToLinear(float gain_db, bool muted)` is
defined in `audio_track_model.h` (the only engine-layer header the app and
assemby site both include). It returns:

```
muted → 0.0f
otherwise → powf(10.0f, gain_db / 20.0f)
```

The conversion is standard audio dBFS semantics. No separate header is
introduced; the function is `inline` and `[[nodiscard]]`.

### Propagation through the plan

`ResolveAudioTracks` now populates `ResolvedAudioTrack::source_gain_linear`
(parallel vector to `sources`) with the linear gain for each contributing row.
This keeps the engine assembly site simple: it reads the pre-computed linear
gain from the resolved plan without needing to know about dB or mute state.

### Assembly site (recorder_session.cpp)

**Merged-source tracks**: the `source_gain_linear` values from the resolved
track are passed directly as `source_gain_multipliers` to `MixedAudioSrc`, one
per source. `MixedAudioSrc` then applies `(1/N) * multiplier[i]` per source —
a muted source contributes `(1/N) * 0 = 0`, which does not break the mix or
introduce discontinuities (it simply produces silence from that source slot).

**Single-source tracks**: previously, gain was only applied for Mic sources
when `mic_gain_linear != 1.0f` (by wrapping in a single-source
`MixedAudioSrc`). This is now generalised: whenever the effective gain differs
from `1.0f`, the single source is wrapped in `MixedAudioSrc` with the effective
gain as the multiplier. For unity gain (0 dB, not muted) the source is passed
through directly, preserving the existing code path.

### Composition with mic_gain_linear

`AudioUiState.mic_gain_linear` is kept as-is for backward compatibility. At the
assembly site the effective multiplier for a Mic source is:

```
effective = mic_gain_linear * GainDbToLinear(row.gain_db, row.muted)
```

This means `mic_gain_linear` acts as a global mic pre-gain (the existing
behaviour), while `row.gain_db`/`row.muted` act as a per-row trim on top of it.
A muted Mic row produces `mic_gain_linear * 0 = 0` regardless of
`mic_gain_linear`. A Mic row at 0 dB / not muted produces `mic_gain_linear * 1
= mic_gain_linear`, preserving existing behaviour exactly.

No change is made to how `mic_gain_linear` is stored, exposed in the UI, or
serialised — that migration is left as a future cleanup once a full channel
model is designed (see Deferred below).

### Preset serialization

Per-row `gain_db` and `muted` are persisted via `RecordingPresetStore` as
`gain_db` (float) and `muted` (bool) keys inside each `[[audio.sources]]`
TOML array-of-tables entry (the store moved INI → TOML in 0.5.0, ADR 0020).
`kPresetSchemaVersion` is bumped to 8. Older presets that lack these keys
default to `0.0` / `false` (no behavior change after schema reset).

### Dirty tracking and sanitization

`NormalizedConfigEquals` and `ConfigDirtyEquivalent` compare `gain_db` (with a
`1e-2f` dB tolerance) and `muted` (exact bool) for each row.
`SanitizePresetConfig` clamps `gain_db` to `[kMinGainDb, kMaxGainDb]` and
resets `NaN`/`Inf` to `0.0`. `muted` is a bool and needs no sanitization.

### UI (AudioSourceRow widget)

Each row in the audio source list gains:

- A horizontal `QSlider` (range −60 to +24, stored as tenths-of-dB integers to
  avoid float rounding in Qt). Width 80 px.
- A `QLabel` showing the current value in "X.X dB" format. Width 52 px.
- A checkable `QPushButton` labelled "M" for mute. 28×28 px.

The `Merge with above` label is preserved exactly. The new controls are placed
between the existing meter/dBFS label area and the merge/toggle area, using the
existing vertical dividers for visual grouping.

`AudioPage` (the audio settings page) shows the rows as locked previews (same
as before). The live gain/mute controls are wired on the Record page where
source rows are interactive.

### Record page wiring (feat/0.6.0-audio-pertrack, PR #78)

`AudioSourceRow::Config` gains `has_gain_control` (default `true`). When false,
the gain slider and label are hidden at construction time. A matching
`setGainControlVisible(bool)` accessor mirrors `setMergeControlVisible`.

`RecordPage::rebuildAudioRowWidgets` now:
1. Initialises `cfg.gain_db` and `cfg.muted` from each `source_rows[i]` so the
   widget reflects persisted state on every rebuild.
2. Sets `cfg.has_gain_control = false` for the **Mic** row — mic gain continues
   to be controlled by the dedicated `mic_gain_slider_`; the per-row slider would
   be a duplicate. The mute button is always shown regardless of `has_gain_control`.
3. Connects `gainDbChanged` → `onAudioRowGainChanged` and `mutedChanged` →
   `onAudioRowMutedChanged` immediately after the existing enable/merge connects.

`onAudioRowGainChanged(int row_index, float gain_db)` and
`onAudioRowMutedChanged(int row_index, bool muted)` mirror `onAudioRowMergeChanged`:
they bounds-check `row_index`, write to `source_rows[i]`, call `RebuildAudioPlan`,
`updateAudioTrackPreview`, and `emitAudioSettingsChanged`.

**Mic gain composition** is unchanged: the assembly site computes
`mic_gain_linear * GainDbToLinear(mic_row.gain_db, mic_row.muted)`.  Because the
per-row gain slider is hidden for the Mic row the user cannot modify `mic_row.gain_db`
from the Record page, so it stays 0 dB (unity); the mute button is the only
per-row control the user sees for the Mic row. Muting via the button sets
`mic_row.muted = true`, causing `GainDbToLinear` to return 0, which produces
`mic_gain_linear * 0 = 0` at the assembly site.

## Deferred (explicitly out of scope for this ADR)

The following are acknowledged and deferred to a later 0.6.x slice:

- **Brickwall limiter** — post-mix peak limiting to prevent clipping.
- **Mic AGC (automatic gain control)** — dynamic microphone level normalization.
- **Noise gate** — silence gate below a threshold.
- **High-pass filter** — rumble / hum removal below ~80 Hz.
- **RNNoise** — neural network noise suppression.
- **PCM and FLAC codecs** — lossless audio output formats.
- **Full channel / sample-format model** — explicit mono/stereo/surround choice
  per source, sample-rate conversion policy, bit-depth selection.
- **Migration of mic_gain_linear to the row model** — consolidating
  `mic_gain_linear` into `row.gain_db` for the Mic row. This is safe to do in a
  dedicated migration slice once the channel model is settled.

## Consequences

- Default config (0 dB, not muted) is math-identical to the previous behavior.
  No A/V regression is possible under default settings.
- A muted source contributes silence. Because `MixedAudioSrc` silence-pads
  missing frames anyway, muting is equivalent to dropping the source from the
  mix — no discontinuity or buffer stall results.
- The resolved plan now carries `source_gain_linear` alongside `sources`.
  Existing tests that construct `ResolvedAudioTrack` directly need to account
  for the new field; `ResolveAudioTracks` is the authoritative factory and all
  test data should be constructed through it.
- `kPresetSchemaVersion` bump to 5 causes a clean reset for users upgrading
  from 0.5.x — the default preset is re-created with 0 dB / not muted, which
  is behaviorally identical.
