# ADR 0049: A/V clock slaving via swresample compensation

## Status

Accepted — implemented (engine + settings + diagnostics). Multi-hour live soak
validation (net drift under threshold, audibly artifact-free) is a follow-up on the
0.10 soak gate.

Related: ADR 0030 (output-format audio decorator — the compensation lives here),
ADR 0035 (phase-correct CFR frame pacing — the QPC axis audio is slaved to),
ADR 0046 (device-loss policy — the reacquire path the controller resets alongside).

## Context

Video is paced on the QPC timeline (CFR frame index × frame interval); audio PTS is
derived from the accumulated sample counter, i.e. from the sound device's own
crystal. Consumer crystals differ by tens of ppm, so the two clocks diverge: 50 ppm
is ~360 ms of A/V drift over two hours — far above the ~45 ms lip-sync perception
threshold. Since #191 this drift is honestly *measured* (device-position/QPC pairs,
`audio_clock_drift.h`), but nothing corrected it: the file drifted while diagnostics
watched.

## Decision

Once measured drift crosses an engage threshold, gently pull the audio output
timeline back onto the QPC axis with a dosed **swresample rate compensation**
(`swr_set_compensation`) inside the existing `OutputFormatAudioSrc` decorator — a
sub-audible ppm pitch change (≤ 0.05 %, ≤ 0.87 cent at the cap), never a sample
drop/insert, never a PTS jump. **On by default and codec-independent** (sync before
bit-exactness); an expert opt-out (*Audio clock slaving*) restores byte-identical
capture for archival PCM/FLAC.

The control law is a pure, hardware-free **P (proportional) controller with an
engage latch** (`clock_slaving.h`): once |drift| > 15 ms it engages for the rest of
the session and drives the rate toward `residual / 60 s`, slew-limited to
125 ppm/s and capped at 500 ppm. It deliberately leaves a small bounded stationary
residual `R_ss = rate × 60 / 1000` ms (3–6 ms at typical 50–100 ppm) rather than
carrying a second integrator term. The residual the file actually holds is what the
A/V drift metric now reports; the raw drift and applied compensation ride alongside
in diagnostics.

### Alternatives considered

- **A — PTS restamping** (periodically re-stamp audio PTS to QPC, samples
  untouched). Rejected: audio PTS is a pure function of the sample counter across
  every encoder; restamping needs jumps or non-sample-true PTS, breaking the
  Matroska block cadence and the continuous-sample-timeline invariant.
- **B — coarse sample drop/insert** in the audio thread. Rejected: functionally a
  rate change delivered as impulses (clicks) needing its own crossfader — a second,
  worse resampler beside the one already present.
- **C — swresample soft compensation in the existing decorator. Chosen.** The
  decorator already sits in every track path and owns the only SwrContext in the
  resample case, so compensation and rate conversion run through one engine. FFmpeg
  activates the resampler on `swr_set_compensation`, so even a 48 k→48 k identity
  context compensates — pinned by a characterization test rather than assumed.
- **D — a separate ClockSlavingAudioSrc decorator.** Rejected: at non-48 kHz
  targets it would stack two SwrContexts in series (double filtering/latency); it
  adds nothing the existing decorator cannot do.
- **E — slave video to audio** (nudge the CFR interval). Rejected: constant-interval
  CFR is a product promise (NLE compatibility). Audio is the elastic degree of
  freedom.

### Why a latch, not full hysteresis

A disengage threshold below `R_ss` would sawtooth (engage → correct → disengage →
re-drift). The latch makes the behaviour monotone and explainable: "past 15 ms of
drift, the recording gently tracks the video clock for the rest of the session." A
PI integrator would drive the residual to zero but at the cost of windup handling and
tuning for a few ms already far below audibility — deliberately not built.

## Consequences

- The default 48 kHz/stereo path is **no longer byte-identical once slaving
  engages** (it is resampled), including for PCM/FLAC — the byte-identical promises
  in `product-spec.md` and `KNOWN_LIMITATIONS.md` are reworded, with the expert
  toggle as the bit-exact opt-out.
- The A/V drift metric's semantics change: `av_drift_ms` is now the **residual**
  after compensation; `av_drift_raw_ms`, `clock_slaving_ppm`, and
  `clock_slaving_active` are added. The post-flight report card appends
  "clock slaving corrected Y ms" when it engaged.
- **Multi-source merged tracks are not slaved** (several device clocks); a single
  gain-adjusted source now is (it forwards its inner device timing and gaps).
- From ~250 ppm the residual no longer drops below the engage threshold — the
  controller converts unbounded drift into a bounded, still-inaudible residual, not
  zero drift. Documented as a limitation.
- **Follow-up:** multi-hour live soak (A/B via the expert toggle), audible-artifact
  listening pass, and underrun-interaction check on the 0.10 soak gate.
