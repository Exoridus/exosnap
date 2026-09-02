#pragma once

// Internal shared state for a live RecorderSession.
// Not part of the public API.

#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/error_types.h>
#include <exosnap/engine/interfaces/VideoEncoderFactory.h>
#include <exosnap/engine/packet_types.h>
#include <exosnap/engine/pipeline_diagnostics.h>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/session_stats.h>
#include <exosnap/engine/webcam_placement.h>

#include "pipeline_diagnostics_aggregator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <windows.h>

namespace exosnap::engine {

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

    // HEVC: VPS+SPS+PPS in Annex-B (for hvcC construction in MuxThread)
    std::vector<uint8_t> hevc_vps_sps_pps;
    bool hevc_ready = false;

    static constexpr uint32_t kMaxAudioTracks = 3;

    std::array<AudioCodecPrivateSlot, kMaxAudioTracks> audio_codec_private{};
    std::array<bool, kMaxAudioTracks> audio_track_ready{};

    [[nodiscard]] bool VideoReady(VideoCodec codec) const noexcept {
        if (codec == VideoCodec::H264)
            return h264_ready;
        if (codec == VideoCodec::Hevc)
            return hevc_ready;
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

// How a producer's wait for mux-queue room ended.
enum class MuxQueueWait {
    Ready,    // Room is available; push the packet.
    Stopping, // The wait ran out while the session was stopping: no room appeared
              // and this packet is not written. Not a backpressure diagnosis --
              // a mux that stopped consuming is what the shutdown policy reports.
    Failed,   // Someone already recorded a cause; do not add a second one.
    TimedOut, // Real backpressure: the destination cannot keep up.
};

// ---------------------------------------------------------------------------
// SessionState — central shared state passed to worker threads
// ---------------------------------------------------------------------------

struct SessionState {
    SessionState() : stop_event(CreateEventW(nullptr, /*manual reset*/ TRUE, FALSE, nullptr)) {
    }

    ~SessionState() {
        if (stop_event) {
            CloseHandle(stop_event);
            stop_event = nullptr;
        }
    }

    SessionState(const SessionState&) = delete;
    SessionState& operator=(const SessionState&) = delete;

    // Cooperative stop token set by Stop() or any fatal worker failure
    std::atomic<bool> stop_requested{false};

    // When the CAPTURE ended, as a steady_clock count in nanoseconds. 0 until a
    // stop is requested; the first request wins, so a failure that arrives while
    // a user stop is already draining cannot move it.
    //
    // The reported duration is measured to this instant and not to the moment
    // Record() returns. Everything between them -- the producer drain and the
    // container finalize -- is work done AFTER the last captured frame, and it is
    // O(duration) and disk-bound: on a long recording finalising to a slow disk
    // it is seconds. Measured to the return, the elapsed time jumped forward at
    // Stop by exactly that much, and the file was reported longer than it is.
    std::atomic<int64_t> capture_end_ns{0};

    // Idempotent: only the first caller records the instant.
    void NoteCaptureEnded() noexcept {
        int64_t expected = 0;
        const auto now =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        capture_end_ns.compare_exchange_strong(expected, now, std::memory_order_relaxed);
    }

    // Win32 mirror of stop_requested for producers that block in
    // WaitForMultipleObjects (the event-driven WASAPI capture drain): set
    // wherever stop_requested is raised through Stop()/RecordFailure() so a
    // blocked wait wakes immediately instead of on its timeout. Manual-reset;
    // Record()'s state reset re-arms it. May be null if event creation failed —
    // waiters must treat that as "poll instead".
    HANDLE stop_event = nullptr;

    void SignalStopEvent() noexcept {
        if (stop_event) {
            SetEvent(stop_event);
        }
    }

    // Cooperative pause token — threads drain their source but discard output.
    // Video threads adjust their epoch on resume so PTS continues seamlessly.
    std::atomic<bool> pause_requested{false};

    // Live mute, one bit per AudioSourceKind (AudioSourceKindBit). A muted
    // source keeps its track running at full length and contributes silence for
    // as long as the mute stands.
    //
    // Silence rather than a gap on purpose: a muxer places a track by its first
    // packet, so a track that started at the first unmute would carry a
    // container-level offset, and neither Matroska nor MP4 can express a hole
    // inside a track at all. One continuous timeline is the only shape that
    // survives the container and stays trivially trimmable afterwards.
    std::atomic<uint32_t> audio_mute_mask{0};

    // Nanoseconds this session has spent paused, accumulated by the stats
    // collector as it observes `pause_requested` change.
    //
    // Elapsed time is what the user reads as "how long is my recording", and a
    // paused capture writes no frames -- a clock that kept counting through a
    // pause promised a file longer than the one that lands. The collector is
    // where it is measured because it is the one thing already ticking; the
    // session end reads it so the final result agrees with what the UI showed.
    std::atomic<long long> paused_ns{0};

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

    // Cumulative bytes committed by the active segment's Matroska writer, published
    // by the mux thread (streaming loop AND during the blocking Finalize()). The
    // shutdown sequence samples this to distinguish a finalize that is slow but
    // still writing (keep waiting) from one that has genuinely stalled. Reset when
    // a new segment writer opens; the progress-based wait tolerates that drop.
    std::atomic<uint64_t> mux_bytes_written{0};

    // Set before Record(); invoked from the mux thread as each segment finalizes.
    SegmentCallback segment_callback;

    // Encoder dispatch seam (IVideoEncoder-refactor design spec). VideoThread
    // calls video_encoder_factory->Create(vendor) instead of constructing a
    // concrete encoder itself. Production code never overrides this default;
    // tests substitute a factory subclass that returns a FakeVideoEncoder to
    // exercise VideoThread's slot/error-escalation paths without real NVENC
    // hardware.
    std::shared_ptr<VideoEncoderFactory> video_encoder_factory = std::make_shared<VideoEncoderFactory>();

    // Frame snapshot (CaptureFrame) — VideoThread reads the flag on its next real frame,
    // or from the last completed encode surface while paused, performs a one-shot
    // NV12→BGRA readback, fires the callback, then clears the flag.
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
    //
    // The queue is bounded in steady state (mirror of the premux limits): when
    // the writer's destination volume cannot keep up (slow NAS, AV scan, low
    // disk), an unbounded queue grows until OOM with no diagnosis. Producers
    // block briefly when the queue is full (WaitForMuxQueueSpace) and record an
    // ErrorPhase::Mux failure if no room appears within the timeout — packets
    // are never silently dropped. EOS/split sentinels bypass the bound: they
    // are tiny and shutdown depends on their delivery.
    static constexpr size_t kMuxQueuePacketLimit = 2048;
    static constexpr size_t kMuxQueueByteLimit = 256ull * 1024 * 1024;
    static constexpr unsigned kMuxQueueFullTimeoutMs = 10000;

    // Effective bounds; production leaves the defaults. Tests shrink them to
    // exercise the overflow path deterministically. Set before Record().
    size_t mux_queue_packet_limit = kMuxQueuePacketLimit;
    size_t mux_queue_byte_limit = kMuxQueueByteLimit;
    unsigned mux_queue_full_timeout_ms = kMuxQueueFullTimeoutMs;

    std::mutex mux_mutex;
    std::condition_variable mux_cv;       // signals the consumer: items available
    std::condition_variable mux_space_cv; // signals producers: room freed (or failure)
    std::deque<MuxItem> mux_queue;
    size_t mux_queue_bytes = 0; // payload bytes queued; guarded by mux_mutex

    // Payload size of a queued item (sentinels count as 0).
    [[nodiscard]] static size_t MuxItemPayloadBytes(const MuxItem& item) noexcept {
        if (const auto* v = std::get_if<EncodedVideoPacket>(&item.payload)) {
            return v->bytes.size();
        }
        if (const auto* a = std::get_if<EncodedAudioPacket>(&item.payload)) {
            return a->bytes.size();
        }
        return 0;
    }

    // Enqueue one item and wake the consumer. Requires mux_mutex to be held.
    // Does NOT wait for room — payload producers call WaitForMuxQueueSpace
    // first; sentinel pushes use this directly (they bypass the bound).
    void PushMuxItemLocked(MuxItem&& item) {
        mux_queue_bytes += MuxItemPayloadBytes(item);
        mux_queue.push_back(std::move(item));
        mux_cv.notify_one();
    }

    // Dequeue-side accounting: call with mux_mutex held, passing the item just
    // popped, so blocked producers wake as room frees up.
    void OnMuxItemPopped(const MuxItem& item) {
        const size_t bytes = MuxItemPayloadBytes(item);
        mux_queue_bytes -= (bytes <= mux_queue_bytes) ? bytes : mux_queue_bytes;
        mux_space_cv.notify_all();
    }

    // Block (bounded) until the mux queue has room for one more payload packet.
    // lk must own mux_mutex.
    //
    // Stopping is reported separately from TimedOut because a stop is not a
    // backpressure fault. A queue that is full when the user stops (slow NAS, AV
    // scan) stays full: the consumer is draining, not accepting. Collapsing that
    // into the timeout meant the producer sat here for the full ten seconds and
    // then recorded "the output destination cannot keep up" for what was an
    // ordinary stop -- and ten seconds is also the producers' join budget, so the
    // same stop could instead be reported as a worker hang.
    [[nodiscard]] MuxQueueWait WaitForMuxQueueSpace(std::unique_lock<std::mutex>& lk) {
        const auto full = [&] {
            return mux_queue.size() >= mux_queue_packet_limit || mux_queue_bytes >= mux_queue_byte_limit;
        };
        if (!full()) {
            return MuxQueueWait::Ready;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(mux_queue_full_timeout_ms);
        while (full()) {
            if (HasFailure()) {
                return MuxQueueWait::Failed; // teardown in progress — never deadlock a producer on the bound
            }
            if (mux_space_cv.wait_until(lk, deadline) == std::cv_status::timeout) {
                if (!full()) {
                    return MuxQueueWait::Ready;
                }
                if (HasFailure()) {
                    return MuxQueueWait::Failed;
                }
                return stop_requested.load() ? MuxQueueWait::Stopping : MuxQueueWait::TimedOut;
            }
        }
        return MuxQueueWait::Ready;
    }

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

    // WYSIWYG preview shared-texture callback (set before Record(); bridged from
    // RecorderSession::SetPreviewSharedHandleCallback). VideoThread fires it once
    // when the shared preview texture is ready, passing the NT handle whose
    // ownership transfers to the consumer, plus the display transform the
    // consumer must apply (preview_tap.h — FP16 scRGB taps need a tone-map).
    // Unset == the tap is disabled at zero cost (the shared texture is never
    // created).
    std::function<void(HANDLE, uint32_t, uint32_t, PreviewTapDesc)> preview_shared_handle_cb;

    // Per-frame publish edge for the same consumer (RecorderSession::
    // SetPreviewFramePublishedCallback). Fired only after TryPublish actually
    // wrote a frame, so a consumer can schedule exactly one redraw per new
    // frame instead of polling the keyed mutex at its own render cadence.
    std::function<void()> preview_frame_published_cb;

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

    // Measured audio epoch per track (100 ns QPC units, same basis as the two
    // above): the instant the endpoint recorded the first captured sample of
    // that track at, published by AudioThread from the device timing WASAPI
    // attributes to a capture packet. 0 == never measured (a merged track mixes
    // several device clocks and attributes none); the muxer then falls back to
    // the session baseline, which is what it always used before.
    std::array<std::atomic<uint64_t>, CodecPrivateData::kMaxAudioTracks> audio_epoch_qpc_100ns{};

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

        if (!std::isfinite(static_cast<double>(overlay.opacity))) {
            overlay.opacity = 1.0f;
        }
        overlay.opacity = std::clamp(overlay.opacity, 0.0f, 1.0f);
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
        overlay.opacity = config.opacity;
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

    // Clear everything the PREVIOUS recording left behind, so a session object
    // reused by the next Record() call starts indistinguishable from a fresh one.
    //
    // Reuse is the normal path: Record() only swaps in a new SessionState after a
    // worker had to be abandoned. Every mutable member above must therefore
    // satisfy exactly one of three rules, and a new member is not finished until
    // it does:
    //   1. it is immutable for the object's lifetime (stop_event, the encoder
    //      factory seam, the queue bounds), or
    //   2. this Record() call installs its value anyway (config, the callbacks,
    //      audio_track_count, the session QPC baseline, the diagnostics
    //      aggregator), or
    //   3. it is cleared here.
    // The list below used to live inline in Record(), and three members had
    // quietly missed it -- each one a stale value the next recording inherited.
    void ResetForNewRecording() {
        // Rule 3 members, in declaration order.

        // A capture end instant is recorded once and only once (NoteCaptureEnded
        // is a compare-exchange against 0), so a value left from the last
        // recording made the NEXT stop a no-op: the new session reported the old
        // session's end, and its duration -- measured from this session's start
        // to that earlier instant -- came out negative and was clamped to 0.
        capture_end_ns.store(0, std::memory_order_relaxed);
        pause_requested.store(false);
        // Live mute is deliberately per-session: a new recording starts from the
        // source rows, so a mute still standing when the last recording stopped
        // must not silently start the next one's microphone muted.
        audio_mute_mask.store(0, std::memory_order_relaxed);
        // Time the LAST session spent paused would otherwise be subtracted from
        // this one's elapsed time, reporting a recording shorter than the file.
        paused_ns.store(0, std::memory_order_relaxed);
        split_request_seq.store(0);
        split_last_trigger.store(static_cast<uint32_t>(SplitTriggerSource::ManualButton));
        // An "armed but unconsumed" size split from a session that ended before
        // the mux's begin_new_segment reset it must not suppress the first
        // size-based split of the next recording.
        size_split_armed.store(false);
        // The finalize-stall detector samples this for byte progress; a stale
        // total from the previous file would read as progress this one has not
        // made yet.
        mux_bytes_written.store(0, std::memory_order_relaxed);
        {
            std::lock_guard lk(snapshot_callback_mutex);
            snapshot_requested.store(false);
            snapshot_callback = nullptr;
        }
        {
            std::lock_guard lk(failure_mutex);
            failure_recorded = false;
            failure = {};
        }
        {
            std::lock_guard lk(premux_mutex);
            video_premux.clear();
            audio_premux.clear();
            codec_private = {};
        }
        {
            std::lock_guard lk(mux_mutex);
            mux_queue.clear();
            mux_queue_bytes = 0;
        }
        {
            std::lock_guard lk(stats_mutex);
            stats = {};
        }
        encode_width = 0;
        encode_height = 0;
        video_epoch_qpc_100ns.store(0);
        for (auto& epoch : audio_epoch_qpc_100ns) {
            epoch.store(0); // a previous session's measured audio zero point must not leak in
        }
    }

    // Raise the stop token with every invariant a stop carries. THE only way to
    // stop a session: a bare stop_requested.store(true) reaches the flag but
    // leaves the rest undone, and each omission is its own defect -- a capture
    // end instant that stays 0 (so the duration is measured to the end of
    // finalize instead of to the last frame), a WASAPI producer asleep in
    // WaitForMultipleObjects until its timeout, and workers still blocked on a
    // condition variable that nobody notified.
    void RequestCleanStop() noexcept {
        // Before the flag, so no worker can observe the stop and start draining
        // while the instant that stop happened is still unrecorded.
        NoteCaptureEnded();
        stop_requested.store(true);
        SignalStopEvent();
        premux_cv.notify_all();
        mux_cv.notify_all();
        mux_space_cv.notify_all(); // wake producers blocked on the queue bound
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
        RequestCleanStop();
    }

    bool HasFailure() const {
        // Relaxed: failure_recorded is only read after stop_requested is checked
        std::lock_guard lk(failure_mutex);
        return failure_recorded;
    }
};

} // namespace exosnap::engine
