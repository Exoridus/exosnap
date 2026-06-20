# ADR 0029: Microphone RNNoise Neural Noise Suppression (Mic DSP chain stage 4)

## Status

Accepted — implemented in 0.6.0 (feat/0.6.0-rnnoise), the final mic-DSP slice of
the Audio v2 wave after per-track gain + mute
([[0018-per-track-audio-control-model]]), the brickwall limiter
([[0023-brickwall-limiter]]), the microphone high-pass filter
([[0024-mic-high-pass-filter]]), the microphone noise gate
([[0025-mic-noise-gate]]), and the microphone AGC ([[0026-mic-agc]]).

## Context

The high-pass filter removes rumble, the noise gate silences the bed between
phrases, and the AGC normalizes the surviving voice's loudness — but none of
these removes *non-stationary* noise that overlaps speech (a fan, keyboard
clatter, hiss, hum) while the talker is speaking. The gate cannot help there (it
only acts below threshold) and the HPF only attacks low frequencies. The
standard fix is a learned noise suppressor: RNNoise (xiph/rnnoise) is a small
recurrent neural network trained to separate speech from noise and attenuate the
noise per frequency band, frame by frame, even during speech.

The AGC (ADR 0026) extended `MicDspAudioSrc` to a three-stage chain
(HPF → gate → AGC) owned by one `MicDspConfig` aggregate. RNNoise is the
**fourth and final stage of that chain** — it reuses the same decorator and
config aggregate, completing the mic-DSP chain for 0.6.0.

## Decision

Add `RnnoiseDenoiser` as the fourth stage of the `MicDspAudioSrc` chain, after
the high-pass filter, the noise gate, and the AGC.

### Dependency (`third_party/CMakeLists.txt`)

RNNoise is **BSD-3-Clause** (xiph/rnnoise). Two upstream traps, both handled in
the third-party CMake and called out here:

- **No usable CMake build.** xiph/rnnoise is an autotools project with no
  `CMakeLists.txt`. We `FetchContent_Declare` + `FetchContent_MakeAvailable`
  (which only *populates* the sources, since there is no CMake target to build)
  and then define **our own static `rnnoise` target** over a curated source
  list. We do NOT blind-glob `src/*.c`: the curated list excludes the upstream
  tool programs that carry their own `main()`
  (`dump_features.c`, `dump_rnnoise_tables.c`, `write_weights.c`), the smaller
  alternate model (`rnnoise_data_little.c`, which would be a duplicate-symbol
  clash with `rnnoise_data.c`), and the entire `x86/` runtime-CPU-dispatch
  (RTCD) tree. With `RNN_ENABLE_X86_RTCD` left undefined RNNoise uses the scalar
  `nnet_default.c` path, which is plenty fast for our workload: one 480-sample
  (10 ms) mono frame per mic buffer.

- **The model weights are not committed to git.** On the pinned commit (master),
  the trained weights live in `src/rnnoise_data.{c,h}`, which are **not** in the
  repository — upstream's `download_model.sh` fetches a tarball from
  `media.xiph.org` keyed by the SHA256 in the `model_version` file and extracts
  it into `src/`. We replicate exactly that at configure time: `file(DOWNLOAD)`
  the pinned tarball with `EXPECTED_HASH SHA256=<model_version>`, then
  `file(ARCHIVE_EXTRACT)` it into the populated source dir before adding the
  target. The step is idempotent (skipped when `src/rnnoise_data.c` already
  exists). The 78 MB generated `rnnoise_data.c` is the network weights.

  We considered the in-tree-model tags `v0.1`/`v0.1.1` (which **do** commit the
  weights and need no download), but their `pitch.c`/`celt_lpc.c` use C99
  variable-length arrays that MSVC `cl.exe` rejects (C2057/C2466). master is
  VLA-free and compiles clean on MSVC, so we **pin master at
  `70f1d256acd4b34a572f999a05c87bf00b67730d`** and pay the model-download cost.

- **`config.h` / `HAVE_*`.** Every `#include "config.h"` in the RNNoise sources
  is guarded by `#ifdef HAVE_CONFIG_H`. We leave `HAVE_CONFIG_H` **undefined**,
  so **no generated config header is needed** — the cleanest possible outcome.
  The one MSVC adjustment is `_USE_MATH_DEFINES` (MSVC's `<math.h>` only defines
  `M_PI`, which `denoise.c`/`pitch.c` use, under that macro). Upstream C-source
  warnings are silenced with `/w` **scoped to the `rnnoise` target only** — we
  do not relax warnings project-wide.

- **License.** `COPYING` is staged to `licenses/rnnoise.txt` via
  `_exosnap_install_license` and listed in `THIRD_PARTY_NOTICES.md`.

`rnnoise` (alias `RNNoise::rnnoise`) is linked into `recorder_core`.

### DSP model (`libs/recorder_core/src/rnnoise_denoiser.{h,cpp}`)

RNNoise is strictly **mono**, runs on **exactly 480-sample (10 ms) blocks of
48 kHz** audio, and expects samples in the **int16 range** (normalized `[-1, 1]`
must be multiplied by 32768 before `rnnoise_process_frame` and divided by 32768
after). The stage therefore keeps, per channel:

- one `DenoiseState` (created with `rnnoise_create(NULL)` → the built-in default
  model; destroyed with `rnnoise_destroy`),
- an input accumulator and a denoised-output FIFO.

**Buffering / framing.** `Process()` accepts any block size: it de-interleaves
each channel's input into the accumulator, processes every whole 480-sample
block now available (scale ×32768 → `rnnoise_process_frame` → scale ÷32768 →
push to the FIFO), then emits exactly `frames` denoised samples in place. This
introduces a fixed **one-block (480 samples / 10 ms) latency**: the first 480
emitted samples per channel are priming silence while the first block fills,
after which output is the denoised stream delayed by one block. The invariant
`priming(480) + produced ≥ input_total` guarantees the FIFO always has enough to
emit. The common case in our pipeline is a 480-frame mic buffer
(`MixedAudioSrc kMixFrameCount = 480`), so each call consumes one block and
emits the previous one.

**48 kHz only.** RNNoise supports no other rate, so when `sample_rate != 48000`
the stage is a **no-op passthrough** (no states, no buffering). Our capture
pipeline is always 48 kHz, so this is never the live path; it just keeps the
stage safe under odd configurations.

`Configure` sanitizes `channels` to `[1, kMaxChannels]` and `sample_rate >= 1`,
(re)creates the per-channel states, and clears buffers. `Reset` clears the
accumulators/FIFOs and re-primes the latency (re-seeding the recurrent memory).
The class owns raw C states, so it is non-copyable/non-movable; the destructor
destroys each state. `Process` is non-throwing (`rnnoise_process_frame` does not
throw).

### Mic-DSP chain (`mic_dsp_audio_src`)

`MicDspConfig` gains `rnnoise_enabled` (default false), and `AnyEnabled()`
becomes `hpf_enabled || gate_enabled || agc_enabled || rnnoise_enabled`.
`MicDspAudioSrc` owns a `RnnoiseDenoiser` member, configures it in `Init` from
the inner source's sample rate/channels and resets it. In `AcquireBuffer` the
stages run **in order: HPF → gate → AGC → RNNoise** — RNNoise is last so the
neural model sees a level-normalized signal and removes the residual
non-stationary noise the cheaper stages leave behind.

### Integration, config and persistence

- `recorder_session::createAudioSource(Mic)` sets `dsp.rnnoise_enabled` from the
  new `RecorderConfig` field; the existing `AnyEnabled()` wrap covers both the
  single-source and merged-track paths.
- `RecorderConfig` gains `mic_rnnoise_enabled` (default **false**). Mic DSP
  alters captured audio, so like the HPF, gate and AGC it is **opt-in**.
- `capability::AudioUiState` and `AudioPlanResult` carry the field;
  `BuildAudioPlan` passes it through; `RecordingCoordinator` copies it onto
  `RecorderConfig`.
- The TOML preset store persists `mic_rnnoise_enabled` in the `[audio]` table;
  `kPresetSchemaVersion` is bumped **14 → 15**. Older presets default to
  disabled (no behavior change vs unsuppressed capture). No numeric sanitize is
  needed (it is a bool). `NormalizedConfigEquals` / `ConfigDirtyEquivalent`
  compare `mic_rnnoise_enabled` exactly.
- Settings → Audio (Expert) gains a single "Noise suppression (RNNoise)" toggle
  after the AGC rows. The old "RNNoise" v0.6 placeholder row is removed (the
  feature is now real).

## Consequences

- Microphone recordings get learned non-stationary noise suppression on top of
  the rumble (HPF), noise bed (gate), and loudness normalization (AGC) already
  applied. The chain is now the full **HPF → gate → AGC → RNNoise**, all owned by
  `MicDspAudioSrc`. This completes the mic-DSP chain for 0.6.0.
- RNNoise is the heaviest third-party dependency in the project: an autotools
  project with a separately-downloaded 78 MB model. The download is verified by
  SHA256 (from `model_version`) and cached across reconfigures. A network outage
  at first configure fails the build with an explicit message; this is the
  expected failure mode for a fetched dependency.
- The stage adds a fixed 10 ms latency to the mic path (one RNNoise block). This
  is below the threshold of perceptible A/V drift for a single muxed track and is
  inherent to block-based neural suppression; it is documented rather than
  compensated.
- The model is fixed (no user parameters) and runs only at 48 kHz. RNNoise's
  built-in default model is used (`rnnoise_create(NULL)`); custom models are not
  exposed. The DSP plumbing (finite/bounded output, length preservation,
  passthrough at non-48k, Reset re-priming, multi-channel independence) is
  unit-tested against the real default model; denoising *quality* is not asserted
  (it is a trained network). The decorator has a full-chain (HPF→gate→AGC→RNNoise,
  no-blowup) test.

## Deferred

User-selectable RNNoise models, latency compensation, and a live preview of the
denoised mic are not built. This is the last mic-DSP stage; the channel/sample-
format model remains the rest of the Audio v2 (0.6.0) wave (see
[[0024-mic-high-pass-filter]] deferred list).
