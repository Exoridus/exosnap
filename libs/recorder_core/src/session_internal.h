#pragma once

// Internal shared state for a live RecorderSession.
// Not part of the public API.

#include <recorder_core/codec_types.h>
#include <recorder_core/error_types.h>
#include <recorder_core/packet_types.h>
#include <recorder_core/pipeline_diagnostics.h>
#include <recorder_core/recorder_session.h>
#include <recorder_core/session_stats.h>
#include <recorder_core/webcam_placement.h>

#include "pipeline_diagnostics_aggregator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
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
struct AudioEosSentinel {
    uint32_t track_id = 0;
};

// Marks a segment boundary in the mux queue. Enqueued by VideoThread immediately
// BEFORE the first packet of the new segment (the forced keyframe), so the mux
// thread finalizes the current container and opens the next one in stream order.
// new_segment_index is the 0-based index of the segment that begins after this
// boundary; trigger records why for logging only.
struct SplitSentinel {
    uint32_t new_segment_index = 0;
    SplitTriggerSource trigger = SplitTriggerSource::ManualButton;
};

struct MuxItem {
    // Discriminated union: encoded packet or EOS/split sentinel
    std::variant<EncodedVideoPacket, EncodedAudioPacket, VideoEosSentinel, AudioEosSentinel, SplitSentinel> payload;
};

// ---------------------------------------------------------------------------
// Codec private data (shared between threads via SessionState)
// ---------------------------------------------------------------------------

struct AudioCodecPrivateSlot {
    std::vector<uint8_t> bytes;
};

struct CodecPrivateData {
    // AV1: 4-byte AV1CodecConfigurationRecord (for WebM/MKV)
    uint8_t av1_codec_private[4] = {};
    bool av1_ready = false;

    // H264: SPS+PPS in Annex-B (for MF_MT_MPEG_SEQUENCE_HEADER in IMFSinkWriter)
    std::vector<uint8_t> h264_sps_pps;
    bool h264_ready = false;

    static constexpr uint32_t kMaxAudioTracks = 3;

    std::array<AudioCodecPrivateSlot, kMaxAudioTracks> audio_codec_private{};
    std::array<bool, kMaxAudioTracks> audio_track_ready{};

    [[nodiscard]] bool VideoReady(VideoCodec codec) const noexcept {
        if (codec == VideoCodec::H264Nvenc)
            return h264_ready;
        return av1_ready;
    }

    [[nodiscard]] bool AudioAllReady(uint32_t track_count) const {
        if (track_count == 0) {
            return true;
        }
        if (track_count > kMaxAudioTracks) {
            return false;
        }

        for (uint32_t i = 0; i < track_count; ++i) {
            if (!audio_track_ready[i]) {
                return false;
            }
        }
        return true;
    }
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

    // Cooperative pause token — threads drain their source but discard output.
    // Video threads adjust their epoch on resume so PTS continues seamlessly.
    std::atomic<bool> pause_requested{false};

    // ---------------------------------------------------------------------------
    // Split recording coordination (SPLIT-RECORDING-R1 / SPLIT-BY-SIZE-R1)
    // ---------------------------------------------------------------------------
    //
    // A manual split is requested by incrementing split_request_seq (monotone).
    // VideoThread tracks the last value it has acted upon; a higher value means a
    // new manual split is pending. Coalescing falls out naturally: many requests
    // before the boundary is reached collapse to one observed delta. The trigger
    // of the most recent request is recorded for structured logging only.
    //
    // VideoThread owns the actual boundary decision (it owns the media timeline
    // via encoded-frame PTS and the forced-IDR arming point) for BOTH manual and
    // automatic (duration-threshold) splits. When it decides to split it arms a
    // forced keyframe on the encoder, enqueues a SplitSentinel into the mux queue,
    // and the mux thread finalizes the current container + opens the next one.
    //
    // Size-based splits (SPLIT-BY-SIZE-R1): the mux thread monitors bytes_written()
    // on the active segment writer. When it exceeds the configured size threshold it
    // bumps split_request_seq (setting split_last_trigger to AutomaticSize) via the
    // same path as a manual request, so VideoThread arms the keyframe and the normal
    // SplitSentinel rollover fires. size_split_armed prevents repeated bumps for the
    // same segment overage; the mux thread resets it when begin_new_segment runs.
    std::atomic<uint64_t> split_request_seq{0};
    std::atomic<uint32_t> split_last_trigger{0}; // SplitTriggerSource of latest request
    std::atomic<bool> size_split_armed{false};   // mux has requested a size split; reset on transition

    // Set before Record(); invoked from the mux thread as each segment finalizes.
    SegmentCallback segment_callback;

    // Frame snapshot (CaptureFrame) — VideoThread reads the flag on its next real frame,
    // performs a one-shot NV12→BGRA readback, fires the callback, then clears the flag.
    // At most one pending request is allowed; a second request while one is pending is ignored.
    std::atomic<bool> snapshot_requested{false};
    std::mutex snapshot_callback_mutex;
    std::function<void(bool, uint32_t, uint32_t, std::vector<uint8_t>, std::string)> snapshot_callback;

    // First-error-wins: only the first failing thread records here
    mutable std::mutex failure_mutex;
    bool failure_recorded = false;
    SessionFailure failure;

    // Pre-mux buffering queues: video and audio are buffered here until
    // codec-private data from the video track and all expected audio tracks
    // is available and Matroska tracks can be initialized.
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

    // Meter callback — high-cadence (~30 Hz) per-track RMS (set before Record())
    MeterCallback meter_callback;

    // Live pipeline-diagnostics aggregator (worker-fed counters/timers) and its
    // ~5 Hz callback. The aggregator owns its own mutex; worker inputs are cheap and
    // never throw. Set/Reset before Record().
    PipelineDiagnosticsAggregator diagnostics;
    DiagnosticsCallback diagnostics_callback;

    // Record config captured at Record() time
    RecorderConfig config;

    mutable std::mutex webcam_overlay_mutex;
    WebcamOverlayLive webcam_overlay;

    // Number of expected output audio tracks for mux readiness/routing.
    uint32_t audio_track_count = 0;

    // Video dimensions (set by VideoThread during prepare)
    uint32_t encode_width = 0;
    uint32_t encode_height = 0;

    // Session QPC baseline and video epoch (used by diagnostics and stats).
    // session_start_qpc_100ns: set by RecorderSession::Record before threads start.
    // video_epoch_qpc_100ns: set by VideoThread when first WGC frame arrives (100ns units,
    // same basis as SystemRelativeTime).
    uint64_t session_start_qpc_100ns = 0;
    std::atomic<uint64_t> video_epoch_qpc_100ns{0};

    // ---------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------

    static WebcamOverlayLive SanitizeWebcamOverlay(WebcamOverlayLive overlay) noexcept {
        WebcamPlacement placement;
        placement.x = overlay.overlay_x_norm;
        placement.y = overlay.overlay_y_norm;
        placement.w = overlay.overlay_w_norm;
        placement.h = overlay.overlay_h_norm;
        placement.mirror = overlay.mirror;
        placement = SanitizeWebcamPlacement(placement);

        overlay.overlay_x_norm = placement.x;
        overlay.overlay_y_norm = placement.y;
        overlay.overlay_w_norm = placement.w;
        overlay.overlay_h_norm = placement.h;
        overlay.mirror = placement.mirror;

        if (!std::isfinite(static_cast<double>(overlay.chroma_tolerance))) {
            overlay.chroma_tolerance = 0.40f;
        }
        if (!std::isfinite(static_cast<double>(overlay.chroma_softness))) {
            overlay.chroma_softness = 0.15f;
        }
        if (!std::isfinite(static_cast<double>(overlay.chroma_spill_reduction))) {
            overlay.chroma_spill_reduction = 0.30f;
        }
        overlay.chroma_tolerance = std::clamp(overlay.chroma_tolerance, 0.0f, 1.0f);
        overlay.chroma_softness = std::clamp(overlay.chroma_softness, 0.0f, 1.0f);
        overlay.chroma_spill_reduction = std::clamp(overlay.chroma_spill_reduction, 0.0f, 1.0f);
        return overlay;
    }

    static WebcamOverlayLive LiveOverlayFromConfig(const WebcamConfig& config) noexcept {
        WebcamOverlayLive overlay;
        overlay.enabled = config.enabled;
        overlay.overlay_x_norm = config.overlay_x_norm;
        overlay.overlay_y_norm = config.overlay_y_norm;
        overlay.overlay_w_norm = config.overlay_w_norm;
        overlay.overlay_h_norm = config.overlay_h_norm;
        overlay.mirror = config.mirror;
        overlay.chroma_key_enabled = config.chroma_key_enabled;
        overlay.chroma_r = config.chroma_r;
        overlay.chroma_g = config.chroma_g;
        overlay.chroma_b = config.chroma_b;
        overlay.chroma_tolerance = config.chroma_tolerance;
        overlay.chroma_softness = config.chroma_softness;
        overlay.chroma_spill_reduction = config.chroma_spill_reduction;
        return SanitizeWebcamOverlay(overlay);
    }

    void SeedWebcamOverlayFromConfig() {
        const WebcamOverlayLive overlay = LiveOverlayFromConfig(config.webcam);
        std::lock_guard lk(webcam_overlay_mutex);
        webcam_overlay = overlay;
    }

    void UpdateWebcamOverlay(WebcamOverlayLive overlay) {
        overlay = SanitizeWebcamOverlay(overlay);
        std::lock_guard lk(webcam_overlay_mutex);
        webcam_overlay = overlay;
    }

    [[nodiscard]] WebcamOverlayLive SnapshotWebcamOverlay() const {
        std::lock_guard lk(webcam_overlay_mutex);
        return webcam_overlay;
    }

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
