#pragma once

// Internal shared state for a live RecorderSession.
// Not part of the public API.

#include <recorder_core/codec_types.h>
#include <recorder_core/error_types.h>
#include <recorder_core/packet_types.h>
#include <recorder_core/recorder_session.h>
#include <recorder_core/session_stats.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <windows.h>

namespace recorder_core {

// ---------------------------------------------------------------------------
// Mux queue item types
// ---------------------------------------------------------------------------

struct VideoEosSentinel {};
struct AudioEosSentinel {};

struct MuxItem {
    // Discriminated union: encoded packet or EOS sentinel
    std::variant<EncodedVideoPacket, EncodedAudioPacket, VideoEosSentinel, AudioEosSentinel> payload;
};

// ---------------------------------------------------------------------------
// Codec private data (shared between threads via SessionState)
// ---------------------------------------------------------------------------

struct CodecPrivateData {
    uint8_t av1_codec_private[4] = {};
    bool av1_ready = false;

    uint8_t aac_codec_private[2] = {};
    bool aac_ready = false;
};

// ---------------------------------------------------------------------------
// First-error-wins failure recorder
// ---------------------------------------------------------------------------

struct SessionFailure {
    int32_t error_code = 0; // HRESULT on Windows; 0 == success
    ErrorPhase error_phase = ErrorPhase::None;
    std::string error_detail;
};

// ---------------------------------------------------------------------------
// SessionState — central shared state passed to worker threads
// ---------------------------------------------------------------------------

struct SessionState {
    // Cooperative stop token set by Stop() or any fatal worker failure
    std::atomic<bool> stop_requested{false};

    // First-error-wins: only the first failing thread records here
    mutable std::mutex failure_mutex;
    bool failure_recorded = false;
    SessionFailure failure;

    // Pre-mux buffering queues: video and audio are buffered here until
    // codec-private data from both tracks is available and Matroska tracks
    // can be initialized.
    //
    // Limits:
    //   video_premux limit: 120 packets
    //   audio_premux limit: 600 packets
    //
    // Overflow causes an ErrorPhase::Mux failure.

    static constexpr size_t kVideoPremuxLimit = 120;
    static constexpr size_t kAudioPremuxLimit = 600;

    std::mutex premux_mutex;
    std::condition_variable premux_cv;

    std::deque<EncodedVideoPacket> video_premux;
    std::deque<EncodedAudioPacket> audio_premux;

    // Codec private ready flags (signal from video/audio thread to mux thread)
    CodecPrivateData codec_private;

    // Mux queue (post-premux phase): after tracks are initialized, encoded
    // packets are pushed here directly.
    std::mutex mux_mutex;
    std::condition_variable mux_cv;
    std::deque<MuxItem> mux_queue;

    // Live stats (written by worker threads, read by stats timer)
    std::mutex stats_mutex;
    SessionStats stats;

    // Stats callback (set before Record())
    StatsCallback stats_callback;

    // Record config captured at Record() time
    RecorderConfig config;

    // Video dimensions (set by VideoThread during prepare)
    uint32_t encode_width = 0;
    uint32_t encode_height = 0;

    // ---------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------

    // Record first failure; triggers stop_requested.
    // hr: platform error code (HRESULT on Windows); 0 == success.
    void RecordFailure(int32_t hr, ErrorPhase phase, const std::string& detail) {
        {
            std::lock_guard lk(failure_mutex);
            if (!failure_recorded) {
                failure_recorded = true;
                failure.error_code = hr;
                failure.error_phase = phase;
                failure.error_detail = detail;
            }
        }
        stop_requested.store(true);
        premux_cv.notify_all();
        mux_cv.notify_all();
    }

    bool HasFailure() const {
        // Relaxed: failure_recorded is only read after stop_requested is checked
        std::lock_guard lk(failure_mutex);
        return failure_recorded;
    }
};

} // namespace recorder_core
