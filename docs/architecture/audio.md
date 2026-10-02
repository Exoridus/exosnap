# Audio capture, processing and encoding

This document owns source-to-track resolution, sample flow, microphone DSP, loss handling and audio-codec framing. [Timing and CFR](timing-and-cfr.md#audio-clocks) owns clock measurement and compensation. The [product specification](../product-spec.md#5-audio-model) owns controls and defaults.

## Sources are not tracks

The application submits ordered source rows: Application, System and Microphone. Eligibility depends on the capture target. Application audio contributes only for a specific window/process target. The capability/model layer resolves enabled sources and merge-to-previous choices into output tracks once. The frontend displays that result and does not derive its own track count or routing.

WASAPI sources include system loopback, process loopback and microphone capture. Fixed device identity, semantic Windows default and a particular process instance are different source identities. A process is identified beyond a reusable PID; reopening must not capture a different process that inherited the number.

A track has one audio worker and encoder. A merged track has per-source FIFOs, gain and mute before summation. The recording mixer weights each configured source by `1 / N` and its gain multiplier; it does not raise surviving sources just because another is silent. Edit playback has a different mixing contract and must not inherit this recording gain rule.

## Sample flow

The normalized capture/mix path is 48 kHz float stereo. Microphone channel mapping and pre-gain happen before the microphone DSP decorator. DSP processes only microphone samples. Mixing, when needed, precedes the per-track output-format decorator and encoder.

`MixedAudioSrc` buffers each source separately, consumes only available frame spans and leaves packet tails for the next call. A source with no samples contributes silence while the others continue. The FIFO cap bounds surplus from independent clocks by discarding old surplus; it is not a precision multi-device clock synchronizer. A multi-source merge has no single attributable device clock.

The output-format decorator owns rate/channel conversion using libswresample. Mono output averages stereo channels rather than summing to a clipping level. Its 48 kHz/stereo identity route avoids conversion until compensation requires it. Encoding integer PCM/FLAC performs the final sample-depth conversion in the encoder. AAC additionally converts the interleaved float input into the native encoder's planar sample layout; that codec-local layout conversion is not a second independent clock controller.

## Gain, mute and limiter

Source gain is represented in decibels and converted using `10^(dB/20)`. Mute emits equal-duration silence instead of deleting samples. The configured source plan and track layout do not change mid-recording when a source is muted.

The mixed bus uses a stereo-linked peak limiter with a configurable ceiling, enabled by default at 0 dBFS. It uses smooth feed-forward attenuation with fast attack and slower release, plus a final clamp that enforces the ceiling despite attack smoothing. There is no lookahead latency. Disabling it uses the mixer's hard clamp; it is not permission to emit arbitrarily out-of-range mixed values. A unity-gain single source need not traverse a mixer merely to instantiate this stage.

## Microphone DSP

Every stage is individually switchable and defaults off. The ordered chain is high-pass filter → noise gate → automatic gain control → RNNoise. Keeping this chain together makes its interactions explicit.

| Stage | Mechanism and invariant |
|---|---|
| High-pass | Second-order Butterworth biquad, independent state per channel. Default cutoff 80 Hz; supported user range 20–1000 Hz. Remove low-frequency energy before the gate decides whether a signal exists. |
| Noise gate | Stereo-linked level detection, default threshold −45 dBFS, configurable −80–0 dBFS. Hold 120 ms, attack 2 ms, release 150 ms. Shared gain avoids stereo-image movement. |
| AGC | Smoothed stereo-linked level, target −18 dBFS, configurable −40–0 dBFS, bounded gain. Freeze gain near silence below the noise-floor threshold instead of amplifying room noise without limit. |
| RNNoise | One denoiser state per channel, 480-sample blocks at 48 kHz. Convert between float normalization and RNNoise's sample scale. Maintain exact stream length with one block of priming latency. |

RNNoise's initial 480 frames are silence while its first block fills, adding 10 ms at 48 kHz. The pipeline does not compensate that algorithmic delay by shifting timestamps. Unsupported input rates must not be passed to a 48 kHz model as if they were valid. Model quality and audible artifacts require listening evidence; arithmetic and length tests cannot prove perceived quality.

With stages off the DSP decorator is a passthrough. That is not a promise that every finished file is bit-identical to device input: gain, merge weighting, channel conversion, quantization, lossy encoding and clock slaving are separate operations.

The RNNoise weights are a pinned build-time dependency, verified by SHA-256, with a project mirror and upstream fallback. Released applications do not download a model at runtime. The curated C source list excludes upstream tools and duplicate model definitions.

## Codec framing

All encoders share a contract that can emit zero, one or several packets per input call and can buffer a tail until flush. Packet PTS follows emitted samples, not the timestamp of the most recent input call.

| Codec | Representation and initialization |
|---|---|
| AAC | FFmpeg's native AAC-LC encoder, raw access units and AudioSpecificConfig from extradata. No ADTS framing and no FDK-AAC dependency. Failure to find an enabled encoder is an explicit initialization failure. |
| Opus | libopus, 48 kHz output, audio application mode, configurable frame duration/complexity/bitrate. `OpusHead` carries initialization and codec-delay information. |
| PCM | Signed little-endian 16/24/32-bit samples using `A_PCM/INT/LIT`, or float32 using `A_PCM/FLOAT/IEEE`. No codec-private bytes; explicit readiness still releases the mux gate. |
| FLAC | libFLAC, 16/24-bit integer input, compression 0–8. The metadata callback supplies `fLaC` plus STREAMINFO before sample packets. Delayed frames must be drained at stop. |

The selectable output rates are 44.1, 48 and 96 kHz for codecs allowed by the current resolver; Opus is fixed at 48 kHz. The AAC wrapper passes the resolved sample rate to FFmpeg and reports encoder initialization failure explicitly.

## Source loss and silence

A lost audio device degrades that source, not the video session. The worker retries at a 500 ms cadence while producing equal-duration silence. Fixed microphones reopen their exact endpoint. Semantic-default sources resolve the current default when reopened. An already-open stream is not automatically moved merely because the default changed; that condition is separately reported. Process loopback reopens only for the same process instance.

A merged source marks failed inners and continues mixing the survivors. When the whole track has no delivering source, the audio worker uses wall-clock silence so its sample timeline does not stop. Reacquisition time is part of the outage. On applicable reactivation the device-clock baseline and compensation controller are reset; a restarted device counter must not look like enormous drift.

Healthy loopback can deliver **no packets**, rather than silent packets, when nothing plays. That is not degradation. After the silent-stall threshold, wall-clock filling keeps later sound at its actual time. The worker remembers silence already inserted so a subsequent discontinuity covering the same gap is not filled twice. Pause is excluded from this accounting.

Device loss, legitimate silence and user mute remain distinct diagnostics. Only measured capture failure raises source-degradation status. Real device unplug, default-follow behavior and partially recovered multi-source outages still require targeted live/fixture verification; no sample reconstruction can recover audio that was never captured.

## Implementation and tests

See [audio worker](../../libs/engine/src/audio_thread.cpp), [mixer](../../libs/engine/src/mixed_audio_src.cpp), [output-format decorator](../../libs/engine/src/output_format_audio_src.h), [audio UI/plan model](../../libs/capability/include/capability/audio_ui_state.h), and [audio loss policy](../../libs/engine/src/audio_device_loss_policy.h). Codec and DSP tests live in [engine tests](../../libs/engine/tests); gain/routing/preset tests also live in [capability tests](../../libs/capability/tests) and [application tests](../../app/tests).
