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
// product's own internal mix-bus format).
//
// This header covers Open/Close/stream-discovery/single-frame seek-decode
// (the scrub and trim-handle-drag path). Continuous playback decode
// (StartPlaybackDecode/StopPlaybackDecode) is declared here too but
// implemented alongside this class's .cpp in the next task.

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

// One block of decoded audio: 48 kHz stereo interleaved float32 PCM.
struct DecodedAudioBlock {
    int64_t pts_us = 0;
    uint32_t frame_count = 0; // sample frames (2 floats each)
    std::shared_ptr<const std::vector<float>> interleaved_stereo;
};

using VideoFrameCallback = std::function<void(DecodedVideoFrame)>;
using AudioBlockCallback = std::function<void(DecodedAudioBlock)>;

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
    [[nodiscard]] bool HasAudioStream() const noexcept;

    // The opened clip's own frame rate in frames per second, or 0.0 when it is
    // unknown (not open, no video stream, or a container that declares no
    // usable rate). Callers use it to pace presentation and to size buffers
    // that hold "some number of frames worth of time" -- neither may assume a
    // fixed rate, since ExoSnap records anything the user configures.
    [[nodiscard]] double VideoFrameRate() const noexcept;

    // Seeks to the keyframe at or before target_us and decodes forward to the
    // first frame at or after target_us. Synchronous; intended for the scrub
    // / trim-handle-drag path, called from a caller-owned worker thread (see
    // EditPlayerSession, Task 8) so it never blocks the UI thread. Returns
    // nullopt if not open, there is no video stream, or decode fails.
    [[nodiscard]] std::optional<DecodedVideoFrame> DecodeFrameAt(int64_t target_us);

    // ---- Continuous playback decode (implemented in Task 6) ----

    // Starts a background thread that decodes forward continuously from
    // `start_us`, delivering frames/audio via the callbacks below until
    // StopPlaybackDecode() is called. No-op if not open or already running.
    void StartPlaybackDecode(int64_t start_us, VideoFrameCallback on_video, AudioBlockCallback on_audio);

    // Stops and joins the playback decode thread, if running. Safe to call
    // even if not running (no-op). Called from Close() and the destructor.
    void StopPlaybackDecode();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace recorder_core
