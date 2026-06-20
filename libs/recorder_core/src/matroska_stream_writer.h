#pragma once

// MatroskaStreamWriter: incremental, constant-RAM Matroska/WebM container writer.
//
// Replaces the previous batch-collect-then-write-once muxer body. Instead of
// accumulating the ENTIRE recording in RAM and writing it in one pass at EOS,
// this writer:
//
//   1. Open()     — writes EBML head, Segment head (unknown size), a reserved
//                   SeekHead placeholder, Segment Info (Duration placeholder),
//                   and the Tracks element. The file is open and streaming.
//   2. Push()     — enqueues a packet into a small bounded reorder window kept
//                   sorted by PTS. As the window advances, packets that have
//                   fallen behind the window horizon are emitted into Matroska
//                   clusters and their backing bytes are freed immediately.
//                   Peak RAM is O(window seconds), NOT O(session).
//   3. Finalize() — drains the remaining window, writes the Cues element (one
//                   entry per video keyframe — accumulated incrementally, tiny),
//                   back-patches the real Duration, replaces the SeekHead
//                   placeholder, and back-patches the Segment size, then closes.
//
// The writer is deliberately decoupled from SessionState / threading so it can
// be unit-tested on synthetic packet streams with no GPU and no live session,
// exactly like test_matroska_mux_structure.cpp. mux_thread.cpp owns the thread,
// queue draining, codec-private readiness, A/V epoch alignment, and Annex-B ->
// AVCC conversion, then feeds packets here one at a time.

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

// Forward declarations of the libebml / libmatroska types we hold by pointer so
// this header does not drag third-party headers (and their MSVC warnings) into
// every translation unit that only needs the interface.
namespace libebml {
class IOCallback;
class EbmlVoid;
} // namespace libebml
namespace libmatroska {
class KaxSegment;
class KaxInfo;
class KaxTracks;
class KaxTrackEntry;
class KaxCluster;
class KaxCues;
} // namespace libmatroska

namespace recorder_core {

// One encoded packet handed to the writer. track_num uses the same placeholder
// scheme as the old muxer: 1 = video, 2+n = audio track n.
struct MuxPacket {
    uint64_t pts_ns = 0;
    uint64_t track_num = 0;
    bool is_key = false;
    std::vector<uint8_t> bytes;
};

// Audio codec discriminator for the Matroska track header. Replaces the old
// audio_is_opus bool so PCM can be represented explicitly.
enum class StreamAudioCodec {
    Aac,  // A_AAC
    Opus, // A_OPUS
    Pcm,  // A_PCM/INT_LIT (16-bit signed little-endian)
    Flac, // A_FLAC (CodecPrivate = native fLaC header: marker + STREAMINFO)
};

// Per-audio-track codec private payload.
struct StreamAudioTrack {
    std::vector<uint8_t> codec_private;
};

// Static configuration resolved once before Open().
struct MatroskaStreamConfig {
    std::string output_path;

    // Video
    std::string video_codec_id;               // "V_MPEG4/ISO/AVC" or "V_AV1"
    std::vector<uint8_t> video_codec_private; // AVCC record or AV1 config record
    uint32_t encode_width = 0;
    uint32_t encode_height = 0;
    uint32_t frame_rate_num = 0;
    uint32_t frame_rate_den = 0;

    // Audio
    StreamAudioCodec audio_codec = StreamAudioCodec::Aac;
    uint32_t audio_track_count = 0;
    std::array<StreamAudioTrack, 3> audio_tracks{}; // CodecPrivateData::kMaxAudioTracks

    // Reorder/interleave window bounds. A packet is only emitted into a cluster
    // once a packet at least window_ns newer (across all tracks) has arrived, so
    // A/V streams whose PTS interleave within this horizon get sorted correctly.
    // The window is ALSO bounded by count to cap RAM if one track stalls.
    uint64_t reorder_window_ns = 3ULL * 1000000000ULL; // 3 seconds
    size_t reorder_window_max_packets = 4096;          // hard ceiling
};

class MatroskaStreamWriter {
  public:
    MatroskaStreamWriter();
    ~MatroskaStreamWriter();

    MatroskaStreamWriter(const MatroskaStreamWriter&) = delete;
    MatroskaStreamWriter& operator=(const MatroskaStreamWriter&) = delete;

    // Open the output file and write the container preamble. Returns false on
    // I/O error or invalid config (e.g. incomplete Opus CodecPrivate). On
    // failure error() carries a human-readable reason and the file is closed.
    bool Open(const MatroskaStreamConfig& config);

    // Enqueue one packet. May write zero or more clusters as the window advances.
    // Returns false if a write error occurred (state then becomes failed; further
    // Push/Finalize calls are no-ops returning false).
    bool Push(MuxPacket packet);

    // Drain the window, write Cues, patch Duration/SeekHead/Segment size, close.
    // Returns false on any I/O error; the file is closed regardless so a partial
    // file is never left with a dangling handle.
    bool Finalize();

    // True once a write error has been recorded.
    [[nodiscard]] bool failed() const noexcept {
        return m_failed;
    }
    [[nodiscard]] const std::string& error() const noexcept {
        return m_error;
    }

    // Diagnostics for tests / logging: peak number of packets and bytes held in
    // the reorder window at any instant. Proves constant-RAM behavior.
    [[nodiscard]] size_t peak_window_packets() const noexcept {
        return m_peak_window_packets;
    }
    [[nodiscard]] uint64_t peak_window_bytes() const noexcept {
        return m_peak_window_bytes;
    }
    [[nodiscard]] size_t current_window_packets() const noexcept {
        return m_window.size();
    }
    [[nodiscard]] uint64_t current_window_bytes() const noexcept {
        return m_cur_window_bytes;
    }

    // Filesystem write-boundary diagnostics. bytes_written is the cumulative file
    // offset advanced by cluster renders; last_flush_ms is the most recent cluster
    // Render() call duration. This is buffered write-call latency (stdio), NOT
    // physical-media durability.
    [[nodiscard]] uint64_t bytes_written() const noexcept {
        return m_bytes_written;
    }
    [[nodiscard]] uint64_t flush_count() const noexcept {
        return m_flush_count;
    }
    [[nodiscard]] double last_flush_ms() const noexcept {
        return m_last_flush_ms;
    }

  private:
    // Sorted-by-PTS reorder window entry.
    struct WindowEntry {
        uint64_t pts_ns = 0;
        uint64_t track_num = 0;
        bool is_key = false;
        std::vector<uint8_t> bytes;
    };

    // Emit window entries whose PTS is behind the horizon (force=true drains all).
    bool DrainWindow(bool force);
    // Write a single resolved packet into the current/next cluster.
    bool EmitPacket(const WindowEntry& e);
    bool FlushCluster();
    void Fail(const std::string& reason);
    void CloseIo();

    MatroskaStreamConfig m_config;

    // libebml/libmatroska objects whose lifetime must span Open()..Finalize().
    libebml::IOCallback* m_io = nullptr;
    std::unique_ptr<libmatroska::KaxSegment> m_segment;
    std::unique_ptr<libebml::EbmlVoid> m_seekhead_void;
    std::unique_ptr<libmatroska::KaxInfo> m_info;
    std::unique_ptr<libmatroska::KaxTracks> m_tracks;
    std::unique_ptr<libmatroska::KaxCues> m_cues;
    std::vector<libmatroska::KaxTrackEntry*> m_track_entries; // owned by m_tracks

    uint64_t m_segment_data_start = 0;

    // Reorder window, kept sorted ascending by PTS (insertion sort on push — the
    // window is small so this is cheap and keeps emission strictly monotone).
    std::deque<WindowEntry> m_window;
    uint64_t m_max_pushed_pts_ns = 0; // newest PTS seen across all tracks

    // Active cluster state.
    libmatroska::KaxCluster* m_cluster = nullptr;
    bool m_first_cluster = true;
    uint64_t m_cluster_start_ms = 0;
    // Backing bytes for blocks in the current cluster must outlive its Render().
    std::vector<std::vector<uint8_t>> m_cluster_bytes;
    // Video keyframe cue points pending for the current cluster.
    std::vector<uint64_t> m_pending_cue_ms;

    uint64_t m_max_emitted_pts_ns = 0; // for final Duration
    bool m_opened = false;
    bool m_finalized = false;
    bool m_failed = false;
    std::string m_error;

    size_t m_peak_window_packets = 0;
    uint64_t m_peak_window_bytes = 0;
    uint64_t m_cur_window_bytes = 0;

    // Filesystem write-boundary counters (see bytes_written()/last_flush_ms()).
    uint64_t m_bytes_written = 0;
    uint64_t m_flush_count = 0;
    double m_last_flush_ms = 0.0;
};

} // namespace recorder_core
