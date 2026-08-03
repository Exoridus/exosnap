#pragma once

// EditPlayerEngine -- demux/decode engine for the Edit-page video player
// (docs/superpowers/specs/2026-07-14-edit-video-player-design.md).
//
// UI-agnostic (no Qt types) per CLAUDE.md. Opens the MKV edit master
// (EditContext::mkv_master_path), decodes video frames to ready-to-paint BGRA
// (internally reusing yuv_to_bgra.h -- a private recorder_core header never
// exposed here, since decoders emit fully-planar YUV420/YUV420P10LE that this
// engine converts before handing anything to a caller), and decodes audio to
// a fixed 48 kHz stereo interleaved float32 PCM stream (matching the
// product's own internal mix-bus format). A recording carrying several audio
// tracks -- system and microphone sound kept separate -- plays ALL of them,
// summed into that one stream.
//
// This header covers Open/Close/stream-discovery/single-frame seek-decode
// (the scrub and trim-handle-drag path) as well as continuous playback decode
// (StartPlaybackDecode/StopPlaybackDecode).

#include <recorder_core/color_metadata.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace recorder_core {

// One decoded video frame, ready to paint: top-down BGRA8888, already
// color-converted using the SOURCE FILE's own container color tags (falls
// back to ColorMetadata::Sdr709() when the container's tags are unspecified,
// matching the product's own SDR default).
struct DecodedVideoFrame {
    int64_t pts_us = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride_bytes = 0; // == width * 4, no padding
    // Shared (not unique) because frames cross from the engine's decode
    // thread to the UI thread via a queued call, which copies the callback's
    // arguments -- a shared_ptr avoids an extra full-frame copy on that hop.
    //
    // A raw array rather than a vector on purpose: the conversion overwrites
    // every byte immediately, so a vector's value-initialization would memset
    // the whole frame (~15 MB at 1440p) for nothing on every single frame.
    std::shared_ptr<const uint8_t[]> bgra;
};

// One block of decoded audio: 48 kHz stereo interleaved float32 PCM. When the
// file carries several audio tracks this is their MIX, not one of them -- the
// engine hands out a single stream whatever the recording's track count.
struct DecodedAudioBlock {
    int64_t pts_us = 0;
    uint32_t frame_count = 0; // sample frames (2 floats each)
    std::shared_ptr<const std::vector<float>> interleaved_stereo;
};

// One audio track of an open file. Recordings written before track names were
// muxed carry no name; a caller labels those positionally instead of inferring
// a source from the track order, which the container does not guarantee.
struct AudioTrackDescription {
    int stream_index = -1;
    std::string name;
};

using VideoFrameCallback = std::function<void(DecodedVideoFrame)>;
using AudioBlockCallback = std::function<void(DecodedAudioBlock)>;

// Pixel format of a RawDecodedVideoFrame's planes -- the decoder's own layout,
// not yet color-converted. Mirrors the three formats IsConvertibleFrame
// already discriminates on in edit_player_engine.cpp.
enum class DecodedPixelFormat : uint8_t {
    Yuv420P8,  // AV_PIX_FMT_YUV420P
    Yuv420P10, // AV_PIX_FMT_YUV420P10LE (10-bit codes in [0,1023], no P010 <<6 justification)
    Yuv444P8,  // AV_PIX_FMT_YUV444P
};

// One decoded video frame, NOT yet color-converted: the raw planes plus enough
// metadata for a GPU converter to do it. Deliberately FFmpeg-free (no AVFrame*,
// no libavutil types) so this header stays includable from Qt/app code without
// pulling in FFmpeg headers -- see `backing_frame` below.
struct RawDecodedVideoFrame {
    int64_t pts_us = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    DecodedPixelFormat format = DecodedPixelFormat::Yuv420P8;
    // Row pitches in bytes, as reported by the decoder (may exceed the
    // tight width-derived size when the source buffer is padded) -- same
    // convention as FullPlanarYuv420Frame/FullPlanar444Frame in yuv_to_bgra.h.
    uint32_t y_stride_bytes = 0;
    uint32_t u_stride_bytes = 0;
    uint32_t v_stride_bytes = 0;
    const uint8_t* y_plane = nullptr;
    const uint8_t* u_plane = nullptr;
    const uint8_t* v_plane = nullptr;
    // True for a natively-HDR10 (PQ) source that needs the tone-map path
    // rather than the ordinary matrix/range conversion -- mirrors
    // IsPqTonemapSource in edit_player_engine.cpp. Only meaningful when
    // format == Yuv420P10.
    bool is_pq_source = false;
    // Color matrix/range to convert with (from the container's own tags, or
    // ColorMetadata::Sdr709() when unspecified) -- same meaning as today's
    // YuvToBgraParams-driven BGRA path.
    MatrixCoefficients matrix = MatrixCoefficients::Bt709;
    ColorRange range = ColorRange::Limited;
    // Keeps the underlying decoder buffer (an FFmpeg AVFrame's ref-counted
    // data) alive for as long as any copy of this struct references the plane
    // pointers above. The pointee is meaningless to callers and MUST NOT be
    // interpreted -- it exists only so the shared_ptr's deleter runs
    // av_frame_free when the last reference drops. Callers must not retain
    // y_plane/u_plane/v_plane past this frame's own lifetime.
    std::shared_ptr<void> backing_frame;
};

using RawVideoFrameCallback = std::function<void(RawDecodedVideoFrame)>;

class EditPlayerEngine {
  public:
    EditPlayerEngine();
    ~EditPlayerEngine();

    EditPlayerEngine(const EditPlayerEngine&) = delete;
    EditPlayerEngine& operator=(const EditPlayerEngine&) = delete;

    // Opens `path` (the MKV edit master) and locates its video/audio streams.
    // Returns false with a human-readable message in out_error on failure
    // (missing file, unreadable container, no decodable video stream).
    bool Open(const std::filesystem::path& path, std::string& out_error);

    // Closes the file and stops any running playback decode (see Task 6).
    void Close();

    [[nodiscard]] bool HasVideoStream() const noexcept;
    // Whether the open file has AT LEAST ONE audio track whose decoder opened.
    // Says nothing about how many, nor about whether a given playback run
    // actually delivers audio -- see AudioTracks() and PlaybackDeliversAudio().
    [[nodiscard]] bool HasAudioStream() const noexcept;

    // The opened clip's own frame rate in frames per second, or 0.0 when it is
    // unknown (not open, no video stream, or a container that declares no
    // usable rate). Callers use it to pace presentation and to size buffers
    // that hold "some number of frames worth of time" -- neither may assume a
    // fixed rate, since ExoSnap records anything the user configures.
    [[nodiscard]] double VideoFrameRate() const noexcept;

    // The opened clip's coded frame size, or 0 when it is unknown (not open,
    // no video stream). Callers use it to size buffers by memory rather than
    // by frame count -- a depth that is sensible at 1080p can be a gigabyte at
    // 2160p, and the decoded frames this engine delivers are BGRA.
    [[nodiscard]] int VideoWidth() const noexcept;
    [[nodiscard]] int VideoHeight() const noexcept;

    // Whether the CURRENT playback run produces audio blocks AT ALL. Valid
    // once StartPlaybackDecode() has returned; false before any run.
    //
    // Distinct from HasAudioStream(), which only says the file has a decodable
    // audio track: building a playback resampler happens per run and can fail
    // on its own, after which this engine delivers video only. A caller that
    // paces video off an audio clock MUST consult this rather than
    // HasAudioStream(), or it will wait forever on a clock nothing advances.
    //
    // With several audio tracks the answer is deliberately "at least one", not
    // "all of them": one track failing to build its resampler must not silence
    // the tracks that did, and the audio clock this exists to protect advances
    // just as well on one track as on three. The delivered blocks are then the
    // mix of the tracks that DID initialize -- the failing one is logged and
    // stays silent for the run. A caller cannot tell from this return value
    // how many tracks are audible, and does not need to: the question it
    // answers is only "will a clock advance if I start one".
    [[nodiscard]] bool PlaybackDeliversAudio() const noexcept;

    // Seeks to the keyframe at or before target_us and decodes forward to the
    // first frame at or after target_us. Synchronous; intended for the scrub
    // / trim-handle-drag path, called from a caller-owned worker thread (see
    // EditPlayerSession, Task 8) so it never blocks the UI thread. Returns
    // nullopt if not open, there is no video stream, or decode fails.
    [[nodiscard]] std::optional<DecodedVideoFrame> DecodeFrameAt(int64_t target_us);

    // Same contract as DecodeFrameAt, but returns the frame unconverted (raw
    // decoder planes) for the GPU conversion path
    // (docs/superpowers/specs/2026-08-03-editor-playback-gpu-render-design.md)
    // instead of CPU-converted BGRA. Not yet implemented -- returns nullopt.
    [[nodiscard]] std::optional<RawDecodedVideoFrame> DecodeFrameAtRaw(int64_t target_us);

    // Every audio track the open file carries, in stream order. `name` comes
    // from the container's track name and is empty for recordings written
    // before names were muxed — callers fall back to a positional label rather
    // than guessing a source from the track order. An empty vector means the
    // file has no audio at all.
    //
    // A track whose decoder could not be opened is still listed: the question
    // is what the RECORDING carries, and leaving a track out would
    // misrepresent the recording rather than the failure. Such a track
    // contributes nothing to playback (HasAudioStream() answers that side).
    //
    // Returns a copy, not a reference to the member: callers read this from
    // another thread than the one running playback, and Close() clears the
    // member -- a reference handed across that boundary could outlive what it
    // points at. The vector holds at most a handful of entries.
    // cppcheck-suppress returnByReference
    [[nodiscard]] std::vector<AudioTrackDescription> AudioTracks() const;

    // ---- Continuous playback decode ----

    // Starts decoding forward continuously from `start_us`, delivering frames
    // and audio via the callbacks below until StopPlaybackDecode() is called.
    // No-op if not open or already running.
    //
    // Runs on THREE threads -- demux, video decode+convert, audio
    // decode+resample+mix -- so that audio never depends on video keeping up
    // (docs/superpowers/specs/2026-08-01-edit-player-decoupled-decode-design.md).
    // Every audio track decodes on that one audio thread and is summed there,
    // so the track count changes what a block CONTAINS, never how many arrive.
    // Consequences for callers:
    //
    // - on_video and on_audio are invoked from two DIFFERENT threads and may
    //   run concurrently. Each is called serially with respect to itself.
    // - Both callbacks MAY BLOCK; that is how the caller paces this engine.
    //   on_audio blocking (a full audio ring) no longer holds up video, and
    //   on_video blocking (a full frame queue) no longer holds up audio.
    // - Because of that, a caller whose callback can block must release it
    //   before calling StopPlaybackDecode(), or the join inside will hang: the
    //   engine can wake its own waits, not the caller's. See
    //   EditPlayerSession::Pause().
    //
    // current_media_time_us reports the playback clock in absolute media time
    // (the caller's audio clock), or any NEGATIVE value when no clock is
    // available. The video thread discards a decoded frame before the colour
    // conversion when that clock has already passed the frame's timestamp --
    // the conversion is the expensive part and the frame would only be dropped
    // on presentation anyway. With no clock, nothing is discarded. An empty
    // std::function is treated the same as "no clock".
    void StartPlaybackDecode(int64_t start_us, VideoFrameCallback on_video, AudioBlockCallback on_audio,
                             std::function<int64_t()> current_media_time_us);

    // Same contract as StartPlaybackDecode, but delivers unconverted
    // (RawDecodedVideoFrame) video for the GPU conversion path instead of
    // CPU-converted BGRA. Not yet implemented -- a no-op.
    void StartPlaybackDecodeRaw(int64_t start_us, RawVideoFrameCallback on_video, AudioBlockCallback on_audio,
                                std::function<int64_t()> current_media_time_us);

    // Stops and joins the playback decode threads, if running. Safe to call
    // even if not running (no-op). Called from Close() and the destructor.
    void StopPlaybackDecode();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace recorder_core
