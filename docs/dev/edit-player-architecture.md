# Edit player engine: decode and playback architecture

Living reference for `EditPlayerEngine`/`EditPlayerSession` (`libs/engine/src/edit_player_engine.cpp`, `libs/engine/include/exosnap/engine/edit_player_session.h`) and the pacing/GPU-conversion pieces around them. This carries the currently valid contracts only. It is not a history of how the engine got here.

## Thread and ownership model

Three threads, each owning one exclusive FFmpeg context:

```
        +-------------+   video packets   +--------------+  RawDecodedVideoFrame  +--------------+
        |             +------------------>|    video     +----------------------->| render/paint |
av_read |    demux    |                   |   decode     |                        +--------------+
------->|   thread    |                   +--------------+
        |             |   audio packets   +--------------+
        |             +------------------>| audio decode +------> WASAPI ring
        +-------------+                   |  + resample  |
                                           +--------------+
```

The demux thread owns the `AVFormatContext` exclusively, and each decode thread owns its own `AVCodecContext` exclusively. No context is touched by two threads.

- **Demux** is paced by playback position, not queue occupancy: it reads until both streams are one second ahead of the clock (`ShouldDemuxMorePackets`, `edit_playback_pacing.h`), then waits for the clock to advance. Pacing on packet-queue occupancy was measured and rejected: a single demuxer reads in container interleave order, so a wait in front of a video packet also withholds the audio packet behind it, and any occupancy bound saturates within tens of milliseconds and gates the demuxer's forward progress on the slowest consumer.
- **Packet queues** (one per stream) hold a soft capacity of 1 s and a hard capacity of 8 s / 8192 packets. At or above soft capacity the demuxer keeps inserting as long as the *other* stream is below a 250 ms low-water mark, and only waits at hard capacity. The rule is symmetric between streams.
- **Audio** is paced by the WASAPI ring exactly as playback output: `PushSamples()` blocks when the ring is full. That blocks only the audio decode thread.
- **Video has no bounded frame queue.** Decoded frames go straight from the decode thread into the renderer's single-slot newest-wins mailbox; decode-ahead pacing is entirely the demux thread's clock-based read-ahead gate above, and stale-frame dropping happens at present time (below), not by queue depth.

Under sustained overload the video decoder is allowed to skip work: `AVCodecContext::skip_frame` is raised to `AVDISCARD_NONREF` while the video thread is behind the clock and lowered again once it catches up.

## Audio render clock is the playback master clock

Audio playback does not depend on video keeping up: video conversion and presentation are clock-gated, but audio delivery paces itself against the WASAPI ring only. The master clock is `WasapiAudioRenderer::FramesPlayed()`, read from the endpoint's own play cursor (`IAudioClock::GetPosition`), not from how many frames have been written into the render buffer. `Stop()` calls `IAudioClient::Reset()` to flush the endpoint buffer so a paused session's cursor and the audio actually queued for playback agree.

A decoded video frame is converted and presented only while its PTS is still in the future relative to this clock. A frame the clock has already passed is dropped before touching the GPU. This scales the expensive conversion/presentation work with the display's presentation rate, not the clip's frame rate.

**No audio stream in the file** (every audio source was muted or disabled while recording): no render client is opened, and playback falls back to a wall-clock-driven seek loop instead of the continuous decode/present path. `PlaybackDeliversAudio()` reports whether **at least one** audio track is actually delivering audio. A caller pacing off the audio clock must consult it instead of `HasAudioStream()`, since a track whose resampler fails to build is dropped from the run without making the flag false as long as another track still advances the clock.

## Multi-track audio

`EditPlayerEngine::Open()` opens every audio stream the file carries, one decoder each, and sums them before the renderer with soft limiting (not division by track count, so a single active track keeps its natural level). The engine exposes what it found:

```cpp
struct AudioTrackDescription {
    int stream_index;
    std::string name;   // from KaxTrackName; empty for older recordings
};
[[nodiscard]] std::vector<AudioTrackDescription> AudioTracks() const;
```

Callers must tolerate an empty name and an empty vector. A track whose decoder fails to open is still listed, since the question the list answers is what the recording carries, not whether every track succeeded.

## RawDecodedVideoFrame: no CPU color conversion in the engine

The decode thread does not convert pixels. It hands off the decoded `AVFrame` itself, ref-counted via `av_frame_ref`/`av_frame_unref` (no extra allocation: the frame stays in the decoder's own frame pool), wrapped as a `RawDecodedVideoFrame`: planar `y/u/v_plane` pointers with strides, a `DecodedPixelFormat` of `Yuv420P8` / `Yuv420P10` / `Yuv444P8`, an `is_pq_source` flag, and colour matrix/range. Color conversion (and, for HDR10 clips, tone-mapping) is the caller's `EditFrameGpuConverter`, a GPU shader pass, never a CPU `swscale`/vectorised path in the interactive playback pipeline. The CPU reference conversion functions stay in the tree as pinned test oracles, not as a production path.

The frame's own `backing_frame` (`std::shared_ptr<void>` with an `av_frame_free` deleter) is the entire cross-thread lifetime model. No additional ownership scheme sits on top of it.

## Hardware decode: D3D11VA with CPU readback

`EditPlayerEngine::Open()` attempts `AV_HWDEVICE_TYPE_D3D11VA` hardware decode generically for every clip it opens, decided once at open time, with a clean fallback to software decode:

- `TryAttachD3D11VA` creates a D3D11VA device and installs a `get_format` callback preferring `AV_PIX_FMT_D3D11`. If device creation fails, decode proceeds exactly as before with no hardware attempt. If a device is created but negotiation later rejects the stream's specific profile/chroma/bit-depth, `avcodec_open2` is retried once without `hw_device_ctx` and the clip plays back entirely on software decode. There is no mid-stream hardware-to-software switch.
- D3D11VA reaches the same NVDEC silicon a direct CUDA hwaccel would, through the vendor-generic Windows API this codebase already uses end to end (capture, GPU color conversion, encode). It also works on Intel/AMD decoders where available, with no per-vendor gate.
- **CPU readback, not GPU-native texture sharing.** A hardware-decoded frame is read back via `av_hwframe_transfer_data` into system memory, de-interleaved into the three separate planes `RawDecodedVideoFrame` already expects, and handed to the unchanged `WrapRawDecodedFrame`/`EditFrameGpuConverter`/`EditPlayerRenderer` pipeline. This was chosen over GPU-native zero-copy (sharing one D3D11 device between decoder and renderer, keeping frames as `AV_PIX_FMT_D3D11` textures end to end) because the dominant cost eliminated is decode itself, not the one-plane-sized readback copy; zero-copy remains a later increment if that copy ever proves to be the bottleneck.
- This is shared code: `Open()` is used by both the interactive playback engine and the separate `EditPlayerEngine` instance `TimelineThumbnailSource` opens for its own tile decoding, so any `AV_PIX_FMT_D3D11` frame the decoder returns must go through the same readback path regardless of caller.

### P010 rescale invariant

`av_hwframe_transfer_data` hands 10-bit streams back as `AV_PIX_FMT_P010LE`: FFmpeg/DXGI convention left-justifies each 10-bit sample into a 16-bit word (`raw16 = sample << 6`). This codebase's `DecodedPixelFormat::Yuv420P10` is defined against the *opposite* convention: plain values in `[0, 1023]`, no `<<6` left-justification. Every consumer of `Yuv420P10` relies on that (the SDR path's `y_scale`/`c_scale` divide by 1023/876/896, while the HDR path's dequantization expects codes in roughly 64-960). The hardware readback path must therefore right-shift every 10-bit sample by 6 as part of de-interleaving. This is a required conversion step, not an optional tuning value: skipping it renders every hardware-decoded 10-bit clip with values roughly 64x too large, and any HDR10 clip that reaches the PQ tonemap path reads garbage codes.

## Failure and fallback boundaries

- Hardware decode negotiation failure at open time falls back to software decode for that stream, silently and completely (see above), never as a per-frame failure signal.
- A file that cannot be decoded at all (unexpected or corrupt codec) falls back to a "Preview unavailable" placeholder rather than crashing; trim, scrub, markers and export are independent of decode success, since export is pure stream-copy.
- A track whose playback resampler fails to build is dropped from the audio mix and logged; the remaining tracks still play and still advance the master clock.

## Shutdown ordering

Producer-to-consumer: demux, then video, then audio, with every blocking wait woken explicitly before its thread is joined. The two demuxer waits that depend on state their own condition variable does not own (the peer queue's level, and the playback clock) are time-bounded as a backstop, so no missed notification can turn either into a permanent stall.

## Related decisions and tests

- ADR 0022 (edit output/save surface) and ADR 0061 (Qt Quick editor player and timeline): the Quick-side consumer of `RawDecodedVideoFrame` and the present-gate contract.
- `libs/engine/tests/test_edit_player_engine.cpp`, `test_playback_clock.cpp`: the pure pacing/sizing functions described above.
- `tools/probes/probe_edit_playback`: the only harness that exercises the real three-thread topology end to end. There is no unit-test seam for thread topology itself without a real file and a real device.
