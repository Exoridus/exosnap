# Color management

This document owns the interpretation of captured pixels, GPU conversion and encoded color signaling. [Product behavior](../product-spec.md#6-video-model) owns the exposed choices; [known limitations](../../KNOWN_LIMITATIONS.md) owns support qualifications.

## One resolved color description

A session resolves primaries, transfer function, matrix, range, bit depth and HDR metadata before encoding. That description drives three consumers: pixel conversion, the encoder's bitstream signaling and the container's color elements. A container tag cannot repair pixels converted with a different matrix, and bitstream metadata cannot be omitted on the assumption that every decoder reads the container.

SDR output is BT.709. Limited range is the default; Full is explicit and carries a compatibility advisory. NVENC H.264/HEVC VUI and AV1 color fields receive the same description as Matroska. MP4 remux preserves the corresponding metadata rather than inventing defaults from resolution.

D3D11 video conversion uses explicit input/output color-space configuration, including the VideoContext1 path where required to make nominal range effective. Driver-default conversion is not a contract. Native shader paths and CPU reference/test conversions must agree on coefficients, quantization and sample representation.

## Capture and working representations

HDR-active displays can produce linear scRGB FP16 capture textures. WGC negotiates an FP16 pool when the selected window's hosting display requires HDR capture. The normal display path negotiates the appropriate duplication format. SDR RGB10A2 precision is not itself evidence of HDR: format and transfer function are different facts.

The source display is resolved at recording start. Its color state determines frame-pool format, tone-map/native decision, encoder input depth and output description. Moving a recorded window to another display does not silently redefine that session's color space. A change incompatible with the admitted pipeline ends the recording, preserving the finalized output where possible. Correct automatic color rollover would require a new capture/conversion/encoder boundary, not merely a mux split.

Windows SDR-content brightness affects interpretation of the composed desktop and the brightness of SDR overlays inside HDR. On supported Windows versions a color-state notification prompts a refresh; a fallback polling path re-reads the facts. This is not an instantaneous guarantee. Changes can leave a short interval exposed according to the previous value, and the notification latency is not established by a unit test.

## Output modes

| Mode | Conversion and signaling |
|---|---|
| SDR source/output | Explicit BT.709 conversion, selected Full/Limited range, 8-bit NV12 by default |
| HDR source to SDR | Tone-map the linear HDR source to BT.709, then encode the selected SDR output format |
| Native HDR10 | PQ transfer, BT.2020 primaries/matrix, Limited range and 10-bit P010; HEVC or AV1 only |
| SDR 10-bit | P010 precision with SDR signaling; not mislabeled as HDR10 |
| SDR 4:4:4 | 8-bit packed AYUV for supported H.264/HEVC profiles, with the same BT.709/range policy |

H.264 cannot carry the native HDR10 mode offered here. AV1 4:4:4, 10-bit 4:4:4 and 4:2:2 are not implemented product paths, irrespective of what a newer GPU SDK might support.

The already-PQ 10-bit desktop path is an explicit exception to the normal linear composition pipeline. It cannot composite webcam/cursor or provide the usual shared pre-encode image. The application must disclose omitted overlays and avoid showing a PiP that the file will not contain.

## HDR metadata

Mastering-display metadata is known from the display facts before encoding. It is written in the container and in-band on keyframes: HEVC Mastering Display Colour Volume SEI or AV1 HDR metadata OBU. Encoder packet/keyframe truth remains authoritative when deciding stream structure.

Content-light values MaxCLL/MaxFALL are measured from frames and written at container level. Their final whole-stream maxima are not available when early keyframes are being emitted, so the application does not claim final in-band content-light metadata. Remux carries the container values into MP4. With splits, accumulated values can reflect a highlight from an earlier segment; they are not an independently reset per-file analysis guarantee.

Do not infer interoperability from a metadata field merely existing. Acceptance inspects real files and uses the relevant players/editors, especially for HEVC sample entries, HDR metadata and range handling.

## Preview and edit conversion

Recording preview shares the composed source and tone-maps native HDR for SDR display. It does not show native-HDR output on an HDR-aware presentation swap chain. The approximation is therefore explicitly a display rendering of the recorded source, not proof of an external player's output.

Edit playback receives planar decoded frames and converts them on the GPU. Unambiguously PQ-tagged 10-bit content uses the HDR tone-map path; unsupported/ambiguous metadata must not accidentally trigger it. The edit path uses the reference display peak rather than querying whichever monitor hosts the editor, so it is not display-adaptive HDR preview.

Hardware decode readback returns P010 samples left-justified in 16-bit words. The internal `Yuv420P10` planar contract instead holds numeric samples in 0–1023. Deinterleaving **must shift by six bits**. Omitting it causes a roughly 64-fold code-value error in both SDR dequantization and PQ conversion. This is a representation conversion, not an exposure adjustment.

## Implementation and tests

The shared [color model](../../libs/engine/include/exosnap/engine/color_metadata.h), [video worker](../../libs/engine/src/video_thread.cpp), [NVENC mapping](../../libs/engine/src/nvenc_encoder.h), [Matroska writer](../../libs/engine/src/matroska_stream_writer.cpp), [MP4 remuxer](../../libs/engine/src/mp4_remuxer.cpp), and [edit decoder](../../libs/engine/src/edit_player_engine.cpp) establish the path. Their [engine tests](../../libs/engine/tests) verify mappings and reference conversions. GPU output and display appearance need live tests in addition to those checks.
