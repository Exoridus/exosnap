# Edit playback and lossless export

This document owns the application-lifetime edit workspace, decoder/render ownership, playback pacing and export transaction. The [product specification](../product-spec.md#8-recording-lifecycle) owns the workspace and interaction contract.

## Separate playback from export

Edit is a resident top-level page. Its C++ workspace owns assets, typed video/audio tracks, linked clips, selection and playhead independently of QML. Imported files remain in place. Navigation pauses playback and retains the workspace; application exit discards it because project persistence is not implemented. Completed recordings reuse retained masters where available. Split recordings add ordered segment assets. Unavailable segments with known duration retain their intervals. Segments whose duration cannot be probed remain in Media with an explicit warning and are not placed on the timeline.

Pressing Export commits an immutable snapshot to an independent operation. Leaving Edit does not cancel that operation. Its completion/failure reaches notifications. Export is stream copy and does not depend on successful preview decoding, GPU presentation or audio playback.

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

The video worker passes a refcounted `AVFrame` through `RawDecodedVideoFrame`: planar pointers and strides, `Yuv420P8`, `Yuv420P10` or `Yuv444P8`, range/matrix and a PQ-source flag. The backing shared pointer with the `av_frame_free` deleter is the cross-thread lifetime owner. A borrowed plane pointer must not outlive it.

Interactive color conversion runs on the GPU using the caller's D3D11 device/context. CPU reference conversions remain test oracles, not a second interactive conversion policy. Qt imports the converted texture into a scene-graph image node; no child player HWND exists.

The presentation gate runs before upload/conversion. Converting every high-frame-rate source picture when the endpoint/display clock will discard most of them wastes GPU work and can itself cause audio/video contention.

## Hardware decode boundary

Open attempts D3D11VA hardware decoding without a vendor-specific preference. Failed device creation or unsupported stream negotiation falls back to software at open. There is no arbitrary mid-stream hardware/software switch.

This path is not zero-copy. Hardware frames are read back using `av_hwframe_transfer_data`, deinterleaved and passed through the same plane-based converter. FFmpeg owns the decoder device and does not supply the Record preview's shared NT-handle transport. A future GPU-native decoder/renderer path needs its own explicit device/surface ownership contract.

P010 hardware readback is left-justified. Internal ten-bit planar samples are not: shift each value by six bits while deinterleaving. This path is used by both interactive playback and the separate thumbnail decoder, so neither may assume the other already performed the conversion.

## Timeline and export transaction

`EditSessionAdapter` exposes the typed workspace to QML. Insert, move, trim, split, delete and ripple delete validate source bounds and reject overlaps on a track. Recording video/audio clips share linked-group identities and mutate together. Undo/redo stores clip deltas rather than duplicating media assets. The model supports additional tracks; the initial page presents linked video/audio rows.

Timeline edits are not keyframe-limited. Snapping aligns moves to neighboring boundaries and the playhead. QML creates clip delegates for the visible time interval and provides horizontal zoom and two-axis scrolling. Audio rows do not claim decoded waveforms. Existing thumbnail, keyframe and marker infrastructure remains available to the adapters.

The playback worker evaluates the active workspace clip and maps timeline time to its source interval. Hard cuts reopen the existing engine session with generation-safe presentation; gaps and unavailable assets clear the frame. This reuses the decoder, hardware fallback and clock contracts below the adapter rather than introducing a second decoder stack. It does not composite concurrent video tracks or provide independent audio-track mixing controls.

The toolbar offers Match source and a native save dialog rooted in the configured recording output folder. Filename, folder and MKV/MP4 container are user choices; no sibling `_edit` location is imposed. YouTube and Archive profiles expose a typed resolution seam but remain unavailable with a render-required reason.

Lossless export accepts an eligible single-source trim or compatible full-clip concatenation. Contiguous pieces of the same source can coalesce, so a split alone need not require rendering. Compatibility is checked before packet copying. Concatenation of independently primed AAC sources requires render export. Generic Matroska B-frame sources without usable DTS are refused; native ExoSnap private exact timestamps and valid-DTS B-frame MP4 sources are supported. Gaps, unsupported partial concatenations and incompatible media require a render path and are rejected explicitly. Stream copy is keyframe-accurate, not arbitrary frame-accurate: cuts can retain dependency pictures to the next random-access boundary.

Transitions are typed relationships between neighboring clip edges. The Transitions tab offers no effect until preview and export both implement it. Crossfade is not implemented. The next render task is a two-source overlap evaluator and D3D11 blend feeding the existing `IVideoEncoder` and mux path, with matching preview and export timing. This preserves the encoder abstraction for NVENC and future AMF/QSV implementations.

Export remuxes into a sibling temporary and publishes atomically on success. Overwrite needs explicit confirmation. Cancel enters Cancelling and remains running until the worker actually stops; a second Export/Retry cannot join an active worker on the GUI thread. Progress is throttled before crossing threads.

Only after media publication succeeds does the sidecar operation reflect the exported recipe. Marker times are filtered to the trim range, rebased to zero, and serialized with the shared model. An empty surviving set removes a stale destination sidecar instead of writing an empty artifact. Sidecar paths replace the media extension: `clip.mkv` and `clip.mp4` each derive `clip.markers.json`. No chapters are written into the container. A sidecar failure must not be confused with failure to create the already-published media.

## Shutdown

Stop/wake producers before joining consumers: demux, video, then audio, with every blocking wait notified. Waits tied to another queue or clock have a bounded backstop so a missed notification cannot hang shutdown. UI/session destruction must not release a backing frame still owned by the render mailbox or a context still used by a worker.

## Implementation and tests

See [edit engine](../../libs/engine/src/edit_player_engine.cpp), [session API](../../libs/engine/include/exosnap/engine/edit_player_session.h), [playback pacing](../../libs/engine/src/edit_playback_pacing.h), [Quick edit item](../../app/quick/ExoSnap/Quick/ExoEditPlayerItem.h), [edit/export adapters](../../app/quick/ExoSnap/Quick/EditExportAdapter.cpp) and [marker sidecar model](../../app/models/MarkerSidecar.h). Pure pacing tests do not prove the three-thread topology with real media/audio hardware; use the [playback probe workflow](../dev/harness-and-tracing.md#deterministic-visual-capture).
