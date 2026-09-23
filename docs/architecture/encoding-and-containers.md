# Encoding and containers

This document owns encoder selection, capability/compatibility policy and packet-to-container contracts. [Color management](color-management.md) owns color values and metadata. The [product matrix](../product-spec.md#4-container--codec--audio-matrix) owns offered combinations.

## Backend and capability boundaries

Video encoding uses the NVIDIA NVENC SDK directly with D3D11 resources. No AMD, Intel or software video encoder is implemented as a recording fallback. Native SDK access preserves resource ownership, precise capability probing, forced keyframes, rate-control mapping and vendor error detail without wrapping every feature in a generic FFmpeg backend.

The video encoder interface/factory isolates the video worker from backend construction. Codec and hardware support come from measured capability data, merged with the application's support policy. The compatibility registry is a separate question: a GPU being able to encode a stream does not imply the product offers it in every container.

Recommended and allowed combinations may be selectable. Experimental/fallback/prohibited combinations are not silently promoted merely because a muxer can theoretically write them. HEVC and 10-bit paths can be implemented yet lack broad cross-generation/player verification. Preserve that distinction rather than describing every selectable cell as universally validated.

Settings options carry `{value, label, selectable, reason}` from the C++ owners. The UI must not infer availability from a codec label or vendor name. Reconciliation runs when the container changes, followed by sanitization, and admission validates again before recording.

## Rate control and encoder preset

The product's constant-quality value is a canonical scale, not universal CRF. NVENC maps it to per-codec constant quantizers. H.264/HEVC use their native QP representation; AV1 uses the canonical-to-native curve, not a linear range ratio. The inter-picture offset is applied in the canonical domain before conversion. VBR and CBR map bitrate fields explicitly.

P1–P7 are speed/quality presets and apply across supported codecs. P4 is the default. Read the SDK preset configuration, then apply the application's explicit rate-control, color, GOP and feature choices. Otherwise a driver-provided default could silently change product behavior.

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
