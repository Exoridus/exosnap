# Edit timeline rendering and export

This document owns the application-lifetime edit workspace, decoder/render ownership, playback pacing and export transaction. The [product specification](../product-spec.md#8-recording-lifecycle) owns the workspace and interaction contract.

## Separate playback from export

Edit is a resident top-level page. Its C++ workspace owns assets, typed video/audio tracks, linked clips, selection and playhead independently of QML. Imported files remain in place. Navigation pauses playback and retains the workspace; application exit discards it because project persistence is not implemented. Completed recordings reuse retained masters where available. Split recordings add ordered segment assets. Unavailable segments with known duration retain their intervals. Segments whose duration cannot be probed remain in Media with an explicit warning and are not placed on the timeline.

Pressing Export commits an immutable snapshot to an independent operation. Leaving Edit does not cancel that operation. Its completion/failure reaches notifications. Eligible stream copy does not depend on successful preview decoding, GPU presentation or audio playback. Crossfade rendering uses the same timeline evaluator, source reader and GPU compositor as preview.

A decode failure therefore leaves an honest Preview unavailable/Timeline previews unavailable state while preserving any valid trim/export operations. It must not be reported as corruption of the source merely because one preview decoder could not open it.

## Decoder ownership and queues

There are three workers: demux, video decode, and audio decode/resample. The demux worker exclusively owns its `AVFormatContext`; each decoder owns its codec context. No FFmpeg context is concurrently manipulated by multiple workers.

Demux reads against the playback clock up to approximately one second ahead. Packet-queue occupancy alone is not the pacing clock: container interleaving can put an audio packet behind a blocked video packet and starve audio. Per-stream queues have soft one-second and hard eight-second/8192-packet bounds. A stream can exceed its soft bound while the peer is below its low-water mark; the hard bound still prevents unbounded growth.

Audio output is paced by the WASAPI render ring, which can block only the audio worker. Video has no separate bounded decoded-frame queue. A newest-wins mailbox carries the latest decoded frame, and the presentation gate drops late frames before GPU conversion. Under sustained overload the decoder can skip nonreference pictures and restore normal decoding when it catches up.

## Playback clock and multiple audio tracks

The master clock is the endpoint's actual play cursor (`IAudioClock`), not the number of samples written into a render buffer. Stop/reset flushes the endpoint so clock and queued playback agree. A caller asks whether audio is **delivering**, not merely whether the file declares an audio stream.

Every audio stream is inspected and independently decoded where possible. Playback sums tracks with soft limiting, not division by track count, so a single active track keeps its level. This is intentionally different from the recording mixer's configured-source weighting. A failed audio decoder/resampler can be omitted while the remaining tracks continue; an audio-track description still answers what the file carries, not only what decoded successfully.

A file with no delivering audio uses the supported wall-clock fallback rather than waiting for an audio cursor that cannot advance. Seeking resets the applicable queues, clock origin and decoder state together. Frames decoded for an earlier generation must not appear after a newer seek.

## Decoded frame representation

The video worker passes a refcounted `AVFrame` through `RawDecodedVideoFrame`: planar pointers and strides, `Yuv420P8`, `Yuv420P10` or `Yuv444P8`, range, matrix, primaries, transfer and a PQ-source flag. The backing shared pointer with the `av_frame_free` deleter is the cross-thread lifetime owner. A borrowed plane pointer must not outlive it. Explicit transfer metadata is required for linear-light rendering; a BT.709 matrix does not establish a BT.709 transfer function.

Interactive color conversion runs on the GPU using the caller's D3D11 device/context. CPU reference conversions remain test oracles, not a second interactive conversion policy. Qt imports the converted texture into a scene-graph image node; no child player HWND exists.

The presentation gate runs before upload/conversion. Converting every high-frame-rate source picture when the endpoint/display clock will discard most of them wastes GPU work and can itself cause audio/video contention.

## Hardware decode boundary

Open attempts D3D11VA hardware decoding without a vendor-specific preference. Failed device creation or unsupported stream negotiation falls back to software at open. There is no arbitrary mid-stream hardware/software switch.

This path is not zero-copy. Hardware frames are read back using `av_hwframe_transfer_data`, deinterleaved and passed through the same plane-based converter. FFmpeg owns the decoder device and does not supply the Record preview's shared NT-handle transport. A future GPU-native decoder/renderer path needs its own explicit device/surface ownership contract.

P010 hardware readback is left-justified. Internal ten-bit planar samples are not: shift each value by six bits while deinterleaving. This path is used by both interactive playback and the separate thumbnail decoder, so neither may assume the other already performed the conversion.

## Timeline and export transaction

`EditSessionAdapter` exposes the typed workspace to QML. Insert, move, trim, split, delete and ripple delete validate source bounds and reject unrelated overlaps on a track. Only the exact overlap owned by a valid Crossfade relationship is allowed. Recording video/audio clips share linked-group identities and mutate together. Undo/redo stores clip deltas and transition relationships rather than duplicating media assets. The model supports additional tracks; the initial page presents linked video/audio rows.

Timeline edits are not keyframe-limited. Snapping aligns moves to neighboring boundaries and the playhead. QML creates clip delegates and adaptive ruler ticks only for the visible time interval. One timeline coordinate mapping drives clips, ruler scrubbing, drops and the foreground playhead. Horizontal zoom preserves a visible playhead anchor; two-axis scrolling retains the ruler and track-label gutter. The adapter projects linked-group selection so both rows share selection and drag feedback. Audio rows do not claim decoded waveforms. Existing thumbnail, keyframe and marker infrastructure remains available to the adapters.

The pure `EvaluateTimeline` function owns half-open intervals, source timestamp mapping, transition progress and complementary video/audio weights. Both preview and export consume it. Hard cuts reopen the existing engine session; Crossfade preview keeps at most two single-owner source engines through `EditTimelineReader`. The item holds one newest-wins frame payload, and request generations reject results from superseded seeks. Linked Float32 audio is mixed on the same timeline, resampled to 48 kHz stereo and limited by the existing brickwall limiter. A missing audio side is silence. WASAPI's played-frame cursor drives preview with audio; silent timelines use elapsed time.

Decoder slots are keyed by video clip, including two clips referencing the same asset. Linked audio uses that clip's slot. Audio decode preserves sample continuity across coarse container timestamp ticks and flushes the resampler tail. Preview prebuffers before starting its clock. Timestamped writes discard samples already replaced by endpoint underrun silence, preventing a decode stall from becoming a permanent A/V delay. This does not promise glitch-free audio under overload.

The toolbar offers Match source and a native save dialog rooted in the configured recording output folder. Filename, folder and MKV/MP4 container are user choices; no sibling `_edit` location is imposed. YouTube and Archive profiles remain unavailable. Render output fixes geometry and exact rational frame rate from the first video source before decoding frames. Other dimensions use aspect-preserving Contain into that output.

`ClassifyEditExport` resolves StreamCopy, Render or Unsupported. Eligible single-source trims and compatible full-source concatenations retain the existing stream-copy implementation, including coalesced contiguous splits. Color restrictions on rendering do not restrict that path. Stream copy is keyframe-accurate, not arbitrary frame-accurate: cuts can retain dependency pictures to the next random-access boundary. Native exact PTS/DTS and existing generic-container restrictions remain authoritative.

Crossfade defaults to 500 ms. Adding it shifts the incoming linked group and following suffix earlier by its duration. The outgoing group and source-in/out stay unchanged. Removing it restores the hard cut; changing its duration applies the corresponding suffix delta. Duration is clamped to real clip intervals and neighboring transitions so no three-way overlap exists. Deleting an endpoint first removes/restores its incident transitions, then applies ordinary or ripple deletion. These changes are atomic undo commands. Markers map through the resulting clip positions.

`EditTimelineCompositor` reuses the existing plane uploader/YUV converter. Two persistent FP16 working textures hold BT.709-linearized sources; one D3D11 fullscreen pass applies Contain, complementary weights and the shared BT.709 output transfer. A persistent BGRA output feeds presentation or the shared SDR VideoProcessor color setup. Blend/raster/depth/shader state is explicitly established. There is no CPU BGRA blend or readback between layers. Texture sizes are stable until geometry changes, and failed initialization/device removal returns an error.

Render export resolves generic `RecorderConfig` intent and calls `VideoEncoderFactory`/`IVideoEncoder`. One NV12 surface per encoder slot is created and registered once. Acquire/reap/flush and pending-frame completion preserve the encoder's reordered PTS/DTS contract. `IAudioEncoder` uses the shipping PCM24 encoder, and `MatroskaStreamWriter` receives video/audio packets and codec private data. Audio is rounded to the final CFR frame boundary, with an explicit mux duration. MP4 rendering is refused because the initial supported render profile uses PCM in Matroska; existing lossless MP4 delivery is unchanged.

| Render contract | Support |
| --- | --- |
| Source color | Explicit BT.709 primaries, transfer and matrix; 8-bit planar 4:2:0; known limited/full range |
| Output color | Limited-range BT.709 SDR, 8-bit 4:2:0 |
| Video backend | Selected/Auto same-adapter shipping encoder through the generic factory; currently NVENC |
| Audio/container | 48 kHz stereo PCM24 in Matroska |
| Unsupported | HDR/PQ/HLG, 10-bit, 4:4:4, unknown/non-BT.709 color, independent audio edits, resolution profiles, render MP4 |

Unsupported rendering fails closed with a reason. It does not silently tone-map or relabel sources, and does not affect eligible lossless exports.

The render workflow is enabled for timelines containing Crossfade. Other non-copy recipes without a transition remain explicitly unsupported; they do not silently take a render path whose preview still uses the legacy hard-cut presentation.

Export writes a sibling temporary and publishes atomically after successful flush/finalization. Overwrite needs explicit confirmation. Cancel enters Cancelling until `QThread::finished`; there is no GUI-thread join. The worker captures shared run state and immutable media/configuration values, never the page or adapter. The GUI polls atomic progress every 100 ms, so export cannot flood its event queue.

The page uses nested native SplitViews. Normalized source-width and timeline-height fractions live in QSettings UI preferences, independently of workspace contents. Restoration clamps against minimum pane sizes and current geometry; handles support native pointer resizing and keyboard arrows.

Only after media publication succeeds does the sidecar operation reflect the exported recipe. Marker times are filtered to the trim range, rebased to zero, and serialized with the shared model. An empty surviving set removes a stale destination sidecar instead of writing an empty artifact. Sidecar paths replace the media extension: `clip.mkv` and `clip.mp4` each derive `clip.markers.json`. No chapters are written into the container. A sidecar failure must not be confused with failure to create the already-published media.

## Shutdown

Stop/wake producers before joining consumers: demux, video, then audio, with every blocking wait notified. Waits tied to another queue or clock have a bounded backstop so a missed notification cannot hang shutdown. UI/session destruction must not release a backing frame still owned by the render mailbox or a context still used by a worker.

## Implementation and tests

See [edit engine](../../libs/engine/src/edit_player_engine.cpp), [session API](../../libs/engine/include/exosnap/engine/edit_player_session.h), [playback pacing](../../libs/engine/src/edit_playback_pacing.h), [Quick edit item](../../app/quick/ExoSnap/Quick/ExoEditPlayerItem.h), [edit/export adapters](../../app/quick/ExoSnap/Quick/EditExportAdapter.cpp) and [marker sidecar model](../../app/models/MarkerSidecar.h). Pure pacing tests do not prove the three-thread topology with real media/audio hardware; use the [playback probe workflow](../dev/harness-and-tracing.md#deterministic-visual-capture).
