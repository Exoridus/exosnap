# Audio capture, processing and encoding

This document owns source-to-track resolution, sample flow, microphone DSP, loss handling and audio-codec framing. [Timing and CFR](timing-and-cfr.md#audio-clocks) owns clock measurement and compensation. The [product specification](../product-spec.md#5-audio-model) owns controls and defaults.

## Sources are not tracks

The application submits a typed audio intent. New configurations use independent source, track and route IDs. Legacy configurations retain their ordered rows and merge-to-previous semantics until explicit conversion. The resolver freezes eligible sources, dense output stream indices and stable identities once for each recording. The frontend edits that intent and displays the resolved result.

WASAPI sources include system loopback, process loopback and microphone capture. Fixed device identity, semantic Windows default and a particular process instance are different source identities. A process is identified beyond a reusable PID; reopening must not capture a different process that inherited the number. Before resolution, the application observes the capture-target process and each independently requested process through a native handle, confirming creation time and executable identity. Observations never replace a mismatching stored binding. A persisted independent-process binding retains its identity but loses session confirmation. It requires fresh source selection after restart; observing the same instance as the video target cannot restore that authority.

A track has one audio worker and encoder. A merged track has per-source FIFOs, gain and mute before summation. The recording mixer weights each configured source by `1 / N` and its gain multiplier; it does not raise surviving sources just because another is silent. Edit playback has a different mixing contract and must not inherit this recording gain rule.

## Sample flow

`SharedAudioCaptureRegistry` preflights every physical capture's native format before allocating and admitting bounded payload pools. One owner opens each physical capture key and corrects its measured clock once. `AudioTee` publishes immutable blocks with independent reader cursors and wakes. Native rate and channel count remain intact at this boundary.

`LogicalAudioSource` owns channel mapping and one stateful DSP chain per SourceId. HPF, gate, AGC and RNNoise precede post-DSP source gain, the pre-mute source meter and source mute. RNNoise alone adapts to its required 48 kHz format. Distinct logical sources sharing one physical capture retain independent DSP histories. Processed blocks fan out to routes without repeating DSP or clock correction.

`TrackAudioMixer` normalizes processed inputs to the output format, aligns them to a common QPC sample grid. Graph-owned tracks infer missing intervals only from shared completion frontiers for their routed sources. A standalone mixer retains its 30 ms arrival horizon. Configured muted or unavailable sources remain in the fixed divisor. The track limiter runs even on a solo route. Source meters are pre-mute and track meters are post-limiter; absent measurements stay unavailable. All tracks drain to the same committed output end.

The physical owner publishes a completion frontier only after posting the corresponding PCM or certifying an idle interval using the native publication-latency envelope. The graph snapshots that frontier before draining physical packets and advances the logical source frontier only after processing the queued data. A track snapshots the minimum frontier across its routed sources before collecting logical packets. Consumer scheduling cannot turn delayed shared PCM into silence on only some tracks.

After a confirmed capture failure, the physical owner commits timestamped outage silence through the same Tee before advancing completion. Readers share that publication under the owner lock, including while native reactivation is blocked. These blocks carry explicit missing-frame provenance through logical DSP and do not increment native capture counters. Publication keeps only the bounded retained horizon as PCM. Recovery discards or trims native samples already replaced by committed outage silence before fan-out. Finite seals can complete over that known outage without waiting for native reactivation. Healthy delayed packets remain governed by the publication-latency envelope rather than this failure policy.

A retained DSP or output-normalization tail limits completion to the samples actually published or queued. During sustained idle, the affected track waits for resumed input or finite drain rather than extrapolating over buffered samples. Tracks without that source keep their own progress. A new native packet preceding a published idle certificate is a terminal timeline failure. Reinitialization cannot revoke a certificate already consumed downstream.
Collection admits samples only into the unread half-second timeline window. When packet timestamps overlap, the first sample queued at each absolute grid position wins, so early and delayed consumers retain identical PCM. A normalized packet that crosses the window end retains its lease and offset until output advances; subsequent packets and resampler tails cannot overwrite that lease. Backlog within the upstream Tee horizon remains sample-preserving. Actual Tee evictions retain their QPC duration as silence and mark discontinuity. The source reader-overflow counter includes evicted frames per attached reader and remains cumulative after a loss is delivered.

The output-format decorator owns rate/channel conversion using libswresample. Mono output averages stereo channels rather than summing to a clipping level. Its 48 kHz/stereo identity route avoids conversion until compensation requires it. Encoding integer PCM/FLAC performs the final sample-depth conversion in the encoder. AAC additionally converts the interleaved float input into the native encoder's planar sample layout; that codec-local layout conversion is not a second independent clock controller.

## Runtime measurement ownership

The physical registry publishes one snapshot per live capture owner using its opaque clock ID. The logical source retains its own processed-block/frame, missing-frame and reader-overflow counters before route fanout. The track mixer publishes measured pre/post-limiter peaks, post-limiter RMS and current limiter gain through a mutex-protected shared snapshot. The graph retains that snapshot rather than a pointer to a mixer transferred to an audio worker, so readback remains safe through shutdown and ownership transfer.

Active track counts and audio queue capacity come from the resolved plan; legacy row-plan defaults apply only without that plan. The meter callback carries all three owner groups. The frontend coalesces delivery at 10 Hz and projects the frozen active plan independently of the editable next plan. Cumulative steady-clock elapsed processing time is measured at the physical conversion, logical processing and track mix/limiter boundaries. Upstream capture and DSP work is not assigned again to each track. These timings include work at the declared boundary and are not thread CPU time, encoder cost or an aggregate utilization percentage. Unmeasured clock quality, limiter detail and processing cost remain unavailable.

## Gain, mute and limiter

Source gain is represented in decibels and converted using `10^(dB/20)`. Mute emits equal-duration silence instead of deleting samples. The configured source plan and track layout do not change mid-recording when a source is muted.

The mixed bus uses a stereo-linked peak limiter with a configurable ceiling, enabled by default at 0 dBFS. It uses smooth feed-forward attenuation with fast attack and slower release, plus a final clamp that enforces the ceiling despite attack smoothing. There is no lookahead latency. Disabling it uses the mixer's hard clamp; it is not permission to emit arbitrarily out-of-range mixed values. Solo tracks retain the same limiter contract.

## Microphone DSP

Every stage is individually switchable and defaults off. The ordered chain is high-pass filter → noise gate → automatic gain control → RNNoise. Keeping this chain together makes its interactions explicit.

| Stage | Mechanism and invariant |
|---|---|
| High-pass | Second-order Butterworth biquad, independent state per channel. Default cutoff 80 Hz; supported user range 20–1000 Hz. Remove low-frequency energy before the gate decides whether a signal exists. |
| Noise gate | Stereo-linked level detection, default threshold −45 dBFS, configurable −80–0 dBFS. Hold 120 ms, attack 2 ms, release 150 ms. Shared gain avoids stereo-image movement. |
| AGC | Smoothed stereo-linked level, target −18 dBFS, configurable −40–0 dBFS, bounded gain. Freeze gain near silence below the noise-floor threshold instead of amplifying room noise without limit. |
| RNNoise | One denoiser state per channel, 480-sample blocks at 48 kHz. Convert between float normalization and RNNoise's sample scale. Preserve the input timeline and exact stream length. |

The typed logical source removes RNNoise priming silence and drains the real tail, preserving the input timeline and stream length. The legacy DSP decorator retains its one-block priming latency: its initial 480 frames are silence, adding 10 ms at 48 kHz without shifting timestamps. Unsupported input rates must not be passed to a 48 kHz model as if they were valid. Model quality and audible artifacts require listening evidence; arithmetic and length tests cannot prove perceived quality.

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

A lost audio device degrades that source, not the video session. The typed physical owner retries with bounded backoff from 250 to 2000 ms. Fixed microphones reopen their exact endpoint. Semantic-default sources resolve the declared role's current default when reopened. An already-open stream is not automatically moved merely because the default changed; that condition is separately reported. Process loopback reopens only for the same process instance. Legacy sources retain their compatibility retry policy.

A typed track places silence on its common QPC sample grid when a source has no delivering data. Its outer audio worker does not fill that interval a second time. Reacquisition time is part of the outage. On applicable reactivation the physical device-clock baseline and compensation controller are reset; a restarted device counter must not look like enormous drift.

Healthy loopback can deliver **no packets**, rather than silent packets, when nothing plays. That is not degradation. Finite stop may seal a healthy idle reader only after a successful empty native poll exceeds the requested QPC endpoint by the declared publication-latency envelope and the resampler has no buffered frames. WASAPI sources derive that envelope from the reported maximum stream latency plus the actual endpoint buffer duration. An unknown envelope cannot prove an idle endpoint. This polling watermark does not stand in for delivered samples: delayed pre-stop samples remain readable and the finite track pads only the missing interval. A blocked acquire, degraded source or buffered tail retains the bounded drain failure contract. Missing intervals and reported discontinuities are reconciled without counting the same loss twice. Bounded gaps advance the logical DSP with zero input. Long gaps reset bounded DSP history with an explicit discontinuity instead of iterating over an unbounded silence interval. Missing QPC remains unknown and never becomes an invented timestamp.

Device loss, legitimate silence and user mute remain distinct diagnostics. Only measured capture failure raises source-degradation status. Real device unplug, default-follow behavior and partially recovered multi-source outages still require targeted live/fixture verification; no sample reconstruction can recover audio that was never captured.

## Implementation and tests

See [runtime graph](../../libs/engine/src/audio_runtime_graph.h), [physical capture registry](../../libs/engine/src/shared_audio_capture.h), [logical source](../../libs/engine/src/logical_audio_source.h), [track mixer](../../libs/engine/src/track_audio_mixer.h), [audio intent](../../libs/engine/include/exosnap/engine/audio_routing_model.h), and [audio worker](../../libs/engine/src/audio_thread.cpp). Codec and DSP tests live in [engine tests](../../libs/engine/tests); gain/routing/preset tests also live in [capability tests](../../libs/capability/tests) and [application tests](../../app/tests).
