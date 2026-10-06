# Encoding and containers

This document owns encoder selection, capability/compatibility policy and packet-to-container contracts. [Color management](color-management.md) owns color values and metadata. The [product matrix](../product-spec.md#4-container--codec--audio-matrix) owns offered combinations.

## Backend and capability boundaries

Video encoding uses the NVIDIA NVENC SDK directly with D3D11 resources. No AMD, Intel or software video encoder is implemented as a recording fallback. Native SDK access preserves resource ownership, precise capability probing, forced keyframes, rate-control mapping and vendor error detail without wrapping every feature in a generic FFmpeg backend.

The video encoder interface/factory isolates the video worker from backend construction. The worker dispatches using the PCI vendor of its actual D3D11 adapter and allocates slots from the selected encoder's capacity. Unsupported adapters fail explicitly; no NVIDIA fallback is attempted. A persisted encoder-device preference (Auto or a PCI fingerprint, never the boot-scoped LUID) names the preferred device for the post-capture processing plus encoding path; it is resolved app-side against the scanned adapters into a complete capture/processing/encoder assignment. The worker verifies the resolution against its actual capture adapter and refuses an explicit device on a different adapter, because the surfaces belong to the capture D3D11 device. The common interface owns codec, canonical quality/rate control, surfaces, packets, asynchronous completion, color and keyframes. Backend-specific tuning is a typed alternative (`BackendTuning`, currently `NvencTuning` with P1-P7 and advanced NVENC features); only the selected backend's factory consumes it, and generic `RecorderConfig` carries no vendor preset. Resolved diagnostics carry an opaque backend token and keep capture, processing and encoder adapter attribution separate. These values imply no equivalence to future AMF or oneVPL tuning. Codec and hardware support come from measured capability data, merged with the application's support policy. The compatibility registry is a separate question: a GPU being able to encode a stream does not imply the product offers it in every container.

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

P1-P7 are NVENC-only speed/quality presets and apply across supported NVENC codecs. P4 is the default. Read the SDK preset configuration, then apply the application's explicit rate-control, color, GOP and feature choices. Otherwise a driver-provided default could silently change product behavior.

Advanced NVENC tuning includes B-frames, reference mode, lookahead/depth, spatial/temporal AQ and multipass. Expert controls resolve through the capability policy against the selected adapter and codec. B-reference capability values contain supported-mode flags: mask value 1 permits Each and value 2 permits Middle. Only modes represented by the pinned SDK are offered. A newer hierarchical-reference bit does not grant an unknown mode. The pinned SDK implements at most seven AV1 B-frames, even when a newer driver reports 31 for its hierarchical mode. The raw hardware fact remains available in diagnostics; the executable count follows both hardware and implemented SDK limits. Lookahead depth is bounded jointly with B-frames by the pinned SDK header. Temporal AQ has no lookahead prerequisite. Spatial AQ is an SDK configuration feature without a separate capability ID. Multipass is rate-control work for VBR/CBR, not CQ. All fields are explicitly pinned after preset configuration. Conservative defaults remain off/single-pass. Changing a quality default requires representative measurement, independently of whether a supported feature is offered as Expert. See the [quality workflow](../dev/encoder-quality-matrix.md).

## NVENC resource lifetime

Input textures remain mapped/owned until their submissions complete. The conservative configuration has eight input slots, four allocated async outputs and active output depth two. Reordering/lookahead configurations allocate `max(8, B-frames + 5 + Lookahead depth)` input slots and `B-frames + 5 + Lookahead depth` active output resources. The conservative synchronous fallback uses four active outputs to accommodate preset buffering. A GPU without async capability uses the synchronous path. Active depth is internal, not a user setting or a universal throughput guarantee. VFR keeps one independent processed-picture cache. When the encoder still buffers pictures and no fresh input arrives for one second, the worker submits that cached picture to advance the encoder. The cache never aliases an encoder-owned input slot. These submissions increment `vfr_encoder_heartbeats`, separately from CFR duplicates and real capture/processing loss.

Each pending submission carries its source slot, media PTS, unique input timestamp, submit time, keyframe prediction and output resource. An accepted submission can return no output, including `NEED_MORE_INPUT`. This is buffering, not evidence the frame was rejected. A rejected submission removes **its own** pending record, not the oldest still-in-flight frame.

Completion waits and EOS drain are bounded. Reuse or teardown cannot occur while the driver still owns a resource. A completion timeout yields an honest encode/finalization failure instead of a permanent wait.

The output-resource queue is consumed in submission order, as required by NVENC. A separate picture-metadata lookup associates the completed bitstream's `outputTimeStamp` with its unique input timestamp. The returned picture can differ from the input associated with the output resource when frames reorder. A mismatch is fatal: stop before muxing a packet whose PTS association is no longer trustworthy. Keyframe prediction mismatch is different and remains a warning. The driver's actual picture-type flag is authoritative for muxing. Since a timestamp mismatch aborts before normal packet accounting, a perpetually zero counter must not be presented as a useful success metric.

## Keyframes and split boundaries

The keyframe cadence is based on media time. A configured two-second interval must stay two seconds when a VFR source produces more pictures than the nominal recording rate. Forced IDRs provide the cadence. The NVENC frame-count GOP is a backstop for CFR. VFR without B-frames uses the infinite-GOP sentinel. VFR with B-frames uses the largest finite GOP to satisfy the SDK constraint while preventing ordinary frame-count insertion from replacing the media-time cadence. A segment boundary independently requests an IDR so the new file begins with a usable access point. Audio waits behind a bounded presentation watermark until the actual IDR establishes the segment boundary. Packets before that PTS remain in the old segment, while packets at or after it belong to the new segment. Alignment is applied once. Each hold is bounded by 256 MiB and 60 seconds of media time, with an explicit failure instead of unbounded growth. EOS drains the remaining audio.

A VFR worker with no pending encoded picture publishes an ordered `VideoProgressSentinel` at most four times per second after the first video packet has been routed. Its lower bound permits idle audio progress. Later capture PTS is clamped above the committed floor, so a future segment cannot retroactively claim committed audio. A pending split blocks idle progress until its actual keyframe is routed. Buffered VFR instead advances through the cached-picture submissions described above.

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

## Packet ordering

`EncodedVideoPacket::pts_ns` is presentation time. Its optional signed `dts_ns` is decode time on the same media timeline. Negative initial decode times represent encoder reordering delay. An absent DTS preserves the existing no-reorder packet contract. The packet sequence and decode timestamps come from the backend, so generic mux code does not need NVENC-specific knowledge.

Premux preserves video packet order. The Matroska writer interleaves by decode time (or PTS when DTS is absent), while block timecodes remain PTS. This keeps reference pictures before dependent B-frames without changing presentation. Matroska has no standard DTS field. Video packets with explicit DTS use BlockGroups and a private, versioned EXDT BlockAdditionMapping to retain exact PTS/DTS in nanoseconds alongside the normal block PTS. This is an opaque extension, not a registered standard DTS mapping. Ordinary players use block order and presentation timestamps and can ignore it. Codec parsing can underestimate reordering with B-reference chains, so MP4 delivery restores supplied timestamps instead of relying on inferred DTS. MP4 sample decode order and composition offsets then preserve presentation. EXDT-aware Matroska rewrites reuse the recording writer so trim and recovery retain the extension. A start trim uses the selected IDR as the common video/audio origin and preserves negative initial decode time. Legacy files without it keep the existing libavformat path. A backend may provide configured sequence-header bytes before its first input. The video worker converts these to codec-private data and publishes them before buffering can delay the first access unit. NVENC requires this header query to succeed. Backends with no early header retain first-random-access extraction. Audio packet timestamp semantics are unchanged.

Zero-B-frame streams keep the existing PTS-based path. Reordered streams require real-file decode/remux, split, seeking and editor qualification in addition to synthetic ordering tests. A capability probe establishes hardware feature availability, not broad player compatibility.

## Second-backend integration boundary

| Area | Ready for a second backend? | Work still required |
|---|---|---|
| Generic lifecycle | Interface seam available | Implement native initialization, asynchronous completion, bounded drain and destruction |
| Packet ordering | Explicit PTS/optional DTS contract | Produce truthful timestamps and qualify real reordered files |
| Factory dispatch | Existing backend dispatch seam | Add dispatch only when a real backend implementation exists |
| Backend tuning | Typed alternative available | Add vendor-owned tuning and migrate app preferences when a second backend requires it |
| Native surface input | GPU texture seam available | Map D3D11 formats and synchronization to AMF or oneVPL |
| Capability probing | Adapter-scoped model available | Add authoritative vendor queries and codec-specific mappings |
| Quality calibration | Canonical product quality intent available | Measure vendor/codec native quality curves |
| Rate-control mapping | Generic CQ/VBR/CBR intent available | Map and qualify backend rate-control behavior |
| Error mapping | Backend failure path available | Define vendor status, device-loss and recovery mappings |
| Diagnostics | Generic running facts and backend tokens available | Populate actual vendor facts without implying tuning equivalence |
| Cross-adapter transport | SameAdapter only | Implement transport separately before split-adapter execution |

The app/preset model still owns NVENC-specific P1-P7 preferences. This is an intentional remaining migration seam. A second real backend should drive its redesign. Initial AMF or QSV integration can use AMD capture/processing/AMF or Intel capture/processing/QSV on the same adapter. Cross-adapter GPU transfer is not required for those topologies and is not implemented here. Output downscaling already uses the D3D11 VideoProcessor with aspect-preserving Contain; alternate scalers and Fill/Crop are separate product work.