# Edit timeline rendering and export

This document owns the application-lifetime edit workspace, decoder/render ownership, playback pacing and export transaction. The [product specification](../product-spec.md#8-recording-lifecycle) owns the workspace and interaction contract.

## Separate playback from export

Edit is a resident top-level page. Its C++ workspace owns assets, typed video/audio tracks, linked clips, selection and playhead independently of QML. Imported files remain in place. Navigation pauses playback and retains the workspace; application exit discards it because project persistence is not implemented. Completed recordings reuse retained masters where available. Split recordings add ordered segment assets. Unavailable segments with known duration retain their intervals. Segments whose duration cannot be probed remain in Media with an explicit warning and are not placed on the timeline.

A finalized MKV master is available for explicit editing while MP4 delivery continues. The editor holds a source lease, so retention defers its rename until the reader can release the path. MP4 remains pending until publication finishes. Opening the master does not create a successful History entry.

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

`EditSessionAdapter` exposes the typed workspace to QML. Workspace commands validate a candidate before publishing it. Rejected and successful no-op commands preserve IDs, history and revision. Applied commands and undo/redo advance a monotonic revision. Immutable metadata snapshots can be retained outside the UI thread. Undo keeps metadata deltas under a 64 MiB budget and evicts complete oldest entries, retaining shared source descriptors rather than copying media.

Time is signed 64-bit microseconds, with a checked rational output frame grid. Result range, playhead and selection are separate values. Ripple deletion removes the union of selected intervals once and maps the result range through that union. If a surviving clip would straddle removed time or the resulting geometry violates a track constraint, the whole command is rejected. Group moves can create destination tracks in the same transaction. Per-stream clips retain validated source identity and global container index separately from track identity. Initial import enablement is retained separately from authored mute/gain state for original-copy assessment.

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

Recording and Edit render export resolve NVENC preset, tuning and lookahead policy through the same settings helper and `ResolveNvencTuning`. Capability reconciliation uses the selected encoder adapter, and rendering opens that resolved adapter. Explicit presets remain the requested presets when supported.

Unsupported rendering fails closed with a reason. It does not silently tone-map or relabel sources, and does not affect eligible lossless exports.

Valid linked hard-cut timelines use the same render reader when stream copy is ineligible. Copy remains preferred when eligible. Before writing staging media, export prepares an immutable outcome showing the container, codecs and retained or mixed audio tracks. Render uses the explicitly confirmed frozen monitor selection, including deliberate silence. Changed sources, selections or timeline recipes require a new review. Overwrite confirmation is separate from outcome confirmation.

Export writes a sibling temporary and publishes atomically after successful flush/finalization. Overwrite needs explicit confirmation. Cancel enters Cancelling until `QThread::finished` without blocking the UI. Application teardown explicitly requests cancellation and joins the worker before destroying the QML engine or other application services. Adapter destruction also joins, so staging cleanup completes even after the event loop has stopped. The worker captures shared run state and immutable media/configuration values, never the page or adapter. The GUI polls atomic progress every 100 ms, so export cannot flood its event queue.

The page uses one vertical SplitView with contextual Media and Details panels. Panels start closed and remain overlays until explicitly pinned. Pin eligibility reserves 480 logical pixels for the viewer after subtracting panel widths and gaps. Resize can unpin a panel but never pins it automatically. Layout preferences are independent of workspace contents.

Marker times are filtered to the trim range, rebased to zero, and staged with the final media filename using the shared model. Publication persists a bounded recovery journal before installing companions or replacing completed media. Existing companions are backed up, including a stale marker sidecar when no markers survive. Media rename is the commit point. Startup recovery uses native file identities to restore companions before commit or clean backups after commit. Changed or ambiguous files retain the journal and staging for investigation. A publisher lock excludes recovery during active publication. This ordering is not a filesystem-wide atomic transaction and does not prove power-loss durability. A metadata failure retains the recovery source and must not be reported as success. Sidecar paths replace the media extension: `clip.mkv` and `clip.mp4` each derive `clip.markers.json`. No chapters are written into the container.

## Audio identity and monitoring

Audio metadata carries stable track IDs, physical container mappings, names, roles, processing provenance and default monitoring. Matroska tags are authoritative only when they agree with the actual tracks and codec formats. MP4 and the WebM sidecar fallback use a sidecar bound to final file identity, length and full SHA-256 after container updates. File publication completes only after both media and identity sidecar are available; interrupted publication retains recovery artifacts.

Missing, invalid or stale metadata requires an explicit stopped-state monitoring selection. Legacy selections are bound to exact source bytes before their IDs can be reused after reopening. Preview and render consume the same immutable selection. Confirmed empty selections yield video-only render output. A rendered PCM24 stereo mix receives a new track ID and records its input track/media lineage; stream copy preserves identities while updating the actual output mapping.

## Recording clip jobs and storage ownership

Recording Clip creation is unavailable in v1.3. `product::kRecordingClipsAvailable` in `app/models/RecordingCapabilities.h` is the compile-time authority. The coordinator rejects requests with `ProductCapabilityUnavailable` before consulting committed media or allocating a request, and does not construct a ClipJob worker. Confirm/cancel cannot admit work. The adapter clamps availability and rejects invokable plan/confirm/cancel calls even with stale positive state. The control policy rejects request/confirm with `unavailable`, and dormant QML controls follow the adapter's constant capability. Runtime settings cannot override it.

The following clip implementation remains compiled and directly testable. Its regression results are engineering evidence, not qualification of a shipping creation feature. Enabling it requires the inclusive 33,554,432-byte per-active-job contract, codec/long-duration decode evidence and recording/isolation qualification. That memory contract remains unresolved. Previously saved clips use normal import, History, playback and editing/export. Background index, file identity, leases, publication recovery and MP4 delivery retain their ordinary recording/editor ownership.

Recording clips consume a finite committed index snapshot and independently leased encoded source files. They do not share capture buffers, GPU surfaces, encoder contexts or the recorder's output cursor. Each lease pins the native file identity and readable extent through a separate read handle. The registry lock protects metadata only: file opening, hashing, callbacks and worker joins happen outside it. Renaming or retention cannot substitute another file behind a clip or editor reader. An editor that still needs the original path defers master renaming. Occupied media or companion destinations retain the original source rather than replacing unrelated files.

The planner finds proven closed video access and committed coverage for every track in recording time. Container keyframe flags alone are insufficient. Segment-local PTS/DTS map to a common recording origin, including negative decode timestamps and verified Opus preroll. A final boundary requires the matching successful segment seal. When an end RAP is the first picture of the next segment, the plan pins that segment solely as a boundary witness. The worker reads and verifies the witness without copying its picture or audio into the clip. It streams the finite interval through bounded extent pages and preserves actual copied source ranges in verified metadata. PCM and Opus have copy paths. AAC/FLAC and MP4 clips fail explicitly rather than entering an unqualified copy or render path.

The committed reader traverses bounded Matroska block elements directly. It rejects oversized encoded payloads before allocation and requires exact packet timestamp additions. It does not probe a live file through a decoder. The immutable descriptor supplies the validated codec and track configuration. Index validation completes before the writer opens. Selected byte extents are spooled to an exclusively owned temporary file and replayed in bounded pages, so decoder metadata does not overlap the writer packet windows. After header initialization, a writer without an index releases its configuration codec and manifest copies. The decision to emit exact audio timestamps is retained independently of those copies. Clip output streams cue entries through an exclusively owned temporary spool and retains every keyframe cue without accumulating the complete cue table in memory. Closed output is checked against its authored manifest and full content identity before publication; this check does not substitute for independent decode qualification.

When admitted through direct engineering entry points, the retained service owns one clip worker and at most three outstanding requests. Review holds source leases and storage reservations without keeping encoded payloads. Admission bounds retained marker storage and the closed-segment seal vector to 2 MiB each, including unused vector capacity. A unique private staging directory owns the attempt's output and companions. Completion follows trailer close, metadata and content-identity verification, and non-overwriting atomic publication. Failed or cancelled staging cleanup removes only files owned by that attempt. Clip errors do not alter the recorder outcome. Recording stop and clip cancellation are independent, while application teardown closes admission, cancels work and joins outside registry locks.

A shared token bucket admits index transfers, clip media and metadata I/O, and segment MP4 remux I/O at 16 MiB/s with a 4 MiB burst and maximum physical transfer. It also covers MP4 faststart's second pass. Recording payload writes remain outside this background budget. Available recorder diagnostics pause background work under critical health, disk bottlenecks, excessive queue depths or measured slow writes. Cancellation and shutdown wake blocked callers. Clip reservations use the union of selected committed byte extents, extent-spool entries and bounded output/staging overhead. Large absolute source offsets do not reserve an entire recording. Reservations also protect other outstanding clips, the primary recording safety floor and pending primary delivery storage. Packet, reorder, cluster, extent and metadata limits constrain components; they do not establish a measured aggregate memory bound or live game-frame-time qualification.

## Shutdown

Stop/wake producers before joining consumers: demux, video, then audio, with every blocking wait notified. Waits tied to another queue or clock have a bounded backstop so a missed notification cannot hang shutdown. UI/session destruction must not release a backing frame still owned by the render mailbox or a context still used by a worker.

## Implementation and tests

See [edit engine](../../libs/engine/src/edit_player_engine.cpp), [session API](../../libs/engine/include/exosnap/engine/edit_player_session.h), [playback pacing](../../libs/engine/src/edit_playback_pacing.h), [Quick edit item](../../app/quick/ExoSnap/Quick/ExoEditPlayerItem.h), [edit/export adapters](../../app/quick/ExoSnap/Quick/EditExportAdapter.cpp) and [marker sidecar model](../../app/models/MarkerSidecar.h). Pure pacing tests do not prove the three-thread topology with real media/audio hardware; use the [playback probe workflow](../dev/harness-and-tracing.md#deterministic-visual-capture).
