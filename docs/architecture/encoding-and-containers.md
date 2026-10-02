# Encoding and containers

This document owns encoder selection, capability/compatibility policy and packet-to-container contracts. [Color management](color-management.md) owns color values and metadata. The [product matrix](../product-spec.md#4-container--codec--audio-matrix) owns offered combinations.

## Backend and capability boundaries

Video encoding uses the NVIDIA NVENC SDK directly with D3D11 resources. No AMD, Intel or software video encoder is implemented as a recording fallback. Native SDK access preserves resource ownership, precise capability probing, forced keyframes, rate-control mapping and vendor error detail without wrapping every feature in a generic FFmpeg backend.

The video encoder interface/factory isolates the video worker from backend construction. The worker dispatches using the PCI vendor of its actual D3D11 adapter and allocates slots from the selected encoder's capacity. Unsupported adapters fail explicitly; no NVIDIA fallback is attempted. A persisted encoder-device preference (Auto or a PCI fingerprint, never the boot-scoped LUID) names the preferred device for the post-capture processing plus encoding path; it is resolved app-side against the scanned adapters into a complete capture/processing/encoder assignment. The worker verifies the resolution against its actual capture adapter and refuses an explicit device on a different adapter, because the surfaces belong to the capture D3D11 device. The common interface owns codec, canonical quality/rate control, surfaces, packets, asynchronous completion, color and keyframes. Backend-specific tuning is a typed alternative (`BackendTuning`, currently `NvencTuning` with P1-P7); only the selected backend's factory consumes it, and generic `RecorderConfig` carries no vendor preset. Resolved diagnostics carry an opaque backend token and keep capture, processing and encoder adapter attribution separate. These values imply no equivalence to future AMF or oneVPL tuning. Codec and hardware support come from measured capability data, merged with the application's support policy. The compatibility registry is a separate question: a GPU being able to encode a stream does not imply the product offers it in every container.

## Pipeline adapter roles

The pipeline names three physical-adapter roles: capture, processing and encoder. A role is a pipeline fact, never a vendor inference. The runtime `PipelineAdapterAssignment` carries the boot-scoped packed LUID, PCI vendor, implemented backend and persistent fingerprint for each role; it is a runtime result, so nothing in it is persisted and the capture role derives from the actual source while processing and encoder come from the resolved preference. The 0.10 build executes only `capture == processing == encoder`, the adapter that owns the source and has an implemented backend that satisfies the request. The assignment still represents a split topology (capture on one adapter, processing/encoder on another), and that desired assignment is never collapsed back onto the capture adapter: it fails with the structured transport or backend reason.

`CaptureSurfaceTransportFor` is the one policy that turns adapter identity comparison into a transport verdict. It answers `SameAdapter` for one physical adapter, `CrossAdapterUnsupported` for two, and `CrossAdapterShared` names a future real transport. Resolver validation and session-start admission consume that seam; no other code compares adapter LUIDs to decide transport. D3D12 heaps, shared cross-adapter textures and CPU staging do not exist yet and are not stubbed.

Intended ownership once a split topology is executable:

| Role | Owns |
|---|---|
| Capture | DDA/WGC acquisition, source timing, retained-source and CFR selection state, and the minimal copy/crop needed to reduce transfer volume |
| Processing | Composition, cursor and webcam composition, HDR analysis/tone mapping, scaling, color conversion and RGB to the backend's input format |
| Encoder | The hardware encoder, its input resources and backend synchronization |

The capture stage keeps source timing and selects the recording frame before any transfer, so a future split does not move unselected source frames between adapters. Processing and encoder remain separate fields even while the intended offload path runs them on one adapter. Diagnostics key telemetry by unique physical adapter: one adapter carrying all three roles produces one NVML target and one DXGI-memory target, and PresentMon presentation evidence stays independent of the assignment.

Recommended and allowed combinations may be selectable. Experimental/fallback/prohibited combinations are not silently promoted merely because a muxer can theoretically write them. HEVC and 10-bit paths can be implemented yet lack broad cross-generation/player verification. Preserve that distinction rather than describing every selectable cell as universally validated.

Settings options carry `{value, label, selectable, reason}` from the C++ owners. The UI must not infer availability from a codec label or vendor name. Reconciliation runs when the container changes, followed by sanitization, and admission validates again before recording.

## Rate control and encoder preset

The product's constant-quality value is a canonical scale, not universal CRF. NVENC maps it to per-codec constant quantizers. H.264/HEVC use their native QP representation; AV1 uses the canonical-to-native curve, not a linear range ratio. The inter-picture offset is applied in the canonical domain before conversion. VBR and CBR map bitrate fields explicitly.

P1–P7 are NVENC-only speed/quality presets and apply across supported NVENC codecs. P4 is the default. Read the SDK preset configuration, then apply the application's explicit rate-control, color, GOP and feature choices. Otherwise a driver-provided default could silently change product behavior.

The current stream has no B-frames or lookahead; spatial and temporal AQ are explicitly off. Probed support for these features is information, not an enabled setting. Changing a quality default requires a representative measurement, not an assumption that a more expensive preset improves every capture. See the [quality workflow](../dev/encoder-quality-matrix.md).

## NVENC resource lifetime

Input textures remain mapped/owned until their submissions complete. The input ring has eight slots. Async output resources have a four-slot allocation ceiling and an active depth of two. A GPU without async capability uses the synchronous path. Active depth is internal, not a user setting or a universal throughput guarantee.

Each pending submission carries its source slot, media PTS, unique input timestamp, submit time, keyframe prediction and output resource. An accepted submission can return no output, including `NEED_MORE_INPUT`. This is buffering, not evidence the frame was rejected. A rejected submission removes **its own** pending record, not the oldest still-in-flight frame.

Completion waits and EOS drain are bounded. Reuse or teardown cannot occur while the driver still owns a resource. A completion timeout yields an honest encode/finalization failure instead of a permanent wait.

A completed bitstream's `outputTimeStamp` must match the expected unique submission timestamp. A mismatch is fatal: stop before muxing a packet whose PTS association is no longer trustworthy. Keyframe prediction mismatch is different and remains a warning. The driver's actual picture-type flag is authoritative for muxing. Since a timestamp mismatch aborts before normal packet accounting, a perpetually zero counter must not be presented as a useful success metric.

## Keyframes and split boundaries

The keyframe cadence is based on media time. A configured two-second interval must stay two seconds when a VFR source produces more pictures than the nominal recording rate. Forced IDRs provide the cadence; the NVENC frame-count GOP is a backstop for CFR and is disabled for the relevant VFR configuration. A segment boundary independently requests an IDR so the new file begins with a usable access point.

HDR mastering metadata follows keyframes; placement prediction does not overrule actual keyframe flags. Output timing, stream initialization and forced-keyframe behavior must be checked together when changing buffering or introducing reordering.

## Recording and delivery containers

The recording writer is libmatroska/libebml for MKV and WebM. MP4 delivery records a Matroska source and uses libavformat stream copy to produce progressive MP4 with faststart. There is no Media Foundation recording muxer or live fragmented-MP4 writer.

Container initialization requires the actual codec-private data from the encoder. Do not synthesize a fixed profile or bit-depth description for all streams. PCM is the deliberate empty-private exception, identified by a readiness flag rather than by nonempty bytes.

HEVC-in-MP4 uses the `hvc1` sample entry and hvcC initialization. The tag alone is not proof of complete bitstream/player conformance; real-file and target-player validation remain part of acceptance. MP4 does not offer AV1, Opus, PCM or FLAC in the current product matrix. In particular, a writable PCM sample entry is not automatically a broadly interoperable PCM-in-MP4 feature.

Markers are external JSON sidecars, never embedded chapters. The shared sidecar-path helper replaces the media extension, so `clip.mp4` maps to `clip.markers.json`. Multiple media files with the same stem therefore share that path convention. Export derives the sidecar from the final published output, not from temporary staging.

## Distribution boundary

The bundled FFmpeg is a shared LGPL build providing avformat, avcodec, avutil and swresample. It supports native AAC encoding and media decode/remux. It does not ship libx264/libx265 or the analysis filters used by developer quality tools. No software H.264/HEVC encoder is linked into first-party release binaries. A distribution change requires dependency, license and patent review rather than treating a library's open-source license as patent clearance.

Exact dependency versions, license texts and linkage belong to [third-party notices](../../THIRD_PARTY_NOTICES.md), not duplicated legal declarations here.

## Implementation and tests

See [NVENC](../../libs/engine/src/nvenc_encoder.h), [codec model](../../libs/engine/include/exosnap/engine/codec_types.h), [compatibility registry](../../libs/capability/src/container_compat_registry.cpp), [Matroska writer](../../libs/engine/src/matroska_stream_writer.h), [remuxer](../../libs/engine/src/mp4_remuxer.cpp), and [FFmpeg build pin](../../cmake/VendorFFmpeg.cmake). Adjacent tests cover pure mappings, packet/codec-private framing and real synthetic container output.

## Material completion and durability

Accepting bytes into a library or CRT buffer is not successful publication. Streaming output propagates payload, cluster, trailer/backpatch, final CRT flush and close failures through the mux result and coordinator. A failed segment cannot become Saved, and its salvageable source remains available for recovery. The streaming Segment uses an explicit unknown-size marker until finalization, so a flushed prefix remains independently readable.

An OS durability request is separate from CRT flush and close. Its failure is reported as durability evidence without changing successful material completion into a write failure. Cumulative ordinary write, CRT flush and OS durability times are separate measurements in snapshots and session reports.

Remux completion includes the AVIO buffered error and output-close result. Only then may publication replace the destination and release the protected source. Split remux uses one storage worker with a pending queue, completion publication and a final drain. Queue age, logical AVIO bytes and packet-write latency describe its work; they are not physical disk bandwidth or a durability guarantee.
