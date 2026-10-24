#include "exosnap/engine/edit_player_engine.h"
#include <array>
#include <capability/translatable.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exosnap/engine/audio_limits.h>
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/edit_types.h>
#include <exosnap/engine/performance_measurements.h>
#include <exosnap/engine/recording_media_types.h>
#include <filesystem>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "audio_media_manifest_adapter.h"
#include "edit_audio_mix.h"
#include "edit_playback_pacing.h"
#include "edit_player_hw_decode.h"
#include "exosnap/engine/color_metadata.h"
#include "exosnap/engine/logging/logging.h"
#include "hdr_preview.h"
#include "hdr_tonemap.h"
#include "media_identity_sidecar.h"
#include "media_timestamp_origin.h"
#include "playback_clock.h"
#include "yuv_to_bgra.h"
#include <cmath>
#include <exosnap/engine/audio_track_model.h>
#include <set>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/defs.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

// MSVC + C++: av_err2str uses a C99 compound literal, not valid C++. Mirrors
// the override already used in mp4_remuxer.cpp.
static inline const char* av_err2str_cpp(int errnum) noexcept {
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}
#ifdef av_err2str
#undef av_err2str
#endif
#define av_err2str(e) av_err2str_cpp(e)

namespace exosnap::engine {

namespace {

constexpr const char* kLogComponent = "edit_player_engine";

void LogWarn(const char* msg) {
    logging::log(logging::LogLevel::Warn, kLogComponent, msg);
}

bool SameUnverifiedAsset(const UnverifiedAudioAssetBinding& frozen, const UnverifiedAudioAssetBinding& current,
                         bool canonical_live_lease = false) {
    const std::array<uint8_t, 32> pending{};
    const bool provisional = frozen.sha256 == pending || current.sha256 == pending;
    if (frozen.file_identity != current.file_identity || frozen.file_size != current.file_size ||
        (provisional && !canonical_live_lease) ||
        (frozen.sha256 != current.sha256 && !(provisional && canonical_live_lease)) ||
        frozen.actual_tracks.size() != current.actual_tracks.size())
        return false;
    std::set<AudioTrackId> ids;
    for (size_t index = 0; index < frozen.actual_tracks.size(); ++index) {
        const auto& before = frozen.actual_tracks[index];
        const auto& after = current.actual_tracks[index];
        const auto& x = before.codec;
        const auto& y = after.codec;
        if (before.track_id == AudioTrackId{} || !ids.insert(before.track_id).second ||
            before.stream_index.value != index || after.stream_index.value != index ||
            before.track_uid != after.track_uid || before.name != after.name || before.role != after.role ||
            before.source_ids != after.source_ids || x.codec_id != y.codec_id || x.codec_private != y.codec_private ||
            x.sample_rate != y.sample_rate || x.channels != y.channels || x.bit_depth != y.bit_depth ||
            x.codec_delay_samples != y.codec_delay_samples || x.channel_layout != y.channel_layout)
            return false;
    }
    return true;
}

// RAII wrapper for AVFormatContext* (input), matching mp4_remuxer.cpp's InputCtxGuard.
struct InputCtxGuard {
    AVFormatContext* ctx = nullptr;
    ~InputCtxGuard() {
        if (ctx)
            avformat_close_input(&ctx);
    }
    InputCtxGuard() = default;
    InputCtxGuard(const InputCtxGuard&) = delete;
    InputCtxGuard& operator=(const InputCtxGuard&) = delete;
};

struct ScopedMediaCancellation {
    std::atomic<const std::atomic<bool>*>& destination;
    explicit ScopedMediaCancellation(std::atomic<const std::atomic<bool>*>& target, const std::atomic<bool>* token)
        : destination(target) {
        destination.store(token, std::memory_order_relaxed);
    }
    ~ScopedMediaCancellation() {
        destination.store(nullptr, std::memory_order_relaxed);
    }
};

// RAII wrapper for one AVCodecContext*.
struct CodecCtxGuard {
    AVCodecContext* ctx = nullptr;
    ~CodecCtxGuard() {
        if (ctx)
            avcodec_free_context(&ctx);
    }
    CodecCtxGuard() = default;
    CodecCtxGuard(const CodecCtxGuard&) = delete;
    CodecCtxGuard& operator=(const CodecCtxGuard&) = delete;
};

// RAII wrapper for one SwrContext*.
struct SwrCtxGuard {
    SwrContext* ctx = nullptr;
    ~SwrCtxGuard() {
        if (ctx)
            swr_free(&ctx);
    }
    SwrCtxGuard() = default;
    SwrCtxGuard(const SwrCtxGuard&) = delete;
    SwrCtxGuard& operator=(const SwrCtxGuard&) = delete;
};

// RAII wrapper for AVPacket*, matching mp4_remuxer.cpp's PacketGuard.
struct PacketGuard {
    AVPacket* pkt = nullptr;
    explicit PacketGuard(AVPacket* p) : pkt(p) {
    }
    ~PacketGuard() {
        if (pkt)
            av_packet_free(&pkt);
    }
    PacketGuard(const PacketGuard&) = delete;
    PacketGuard& operator=(const PacketGuard&) = delete;
};

// RAII wrapper for AVFrame*.
struct FrameGuard {
    AVFrame* frame = nullptr;
    explicit FrameGuard(AVFrame* f) : frame(f) {
    }
    ~FrameGuard() {
        if (frame)
            av_frame_free(&frame);
    }
    FrameGuard(const FrameGuard&) = delete;
    FrameGuard& operator=(const FrameGuard&) = delete;
};

// Maps a container's own CICP color tags to the engine's color-metadata
// enums, falling back to the product's SDR BT.709/Limited default when the
// container left a tag unspecified -- matches the fallback mp4_remuxer.cpp
// already applies when copying color description forward.
MatrixCoefficients MapMatrix(AVColorSpace cs) noexcept {
    switch (cs) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return MatrixCoefficients::Bt601;
    case AVCOL_SPC_BT2020_NCL:
        return MatrixCoefficients::Bt2020Ncl;
    case AVCOL_SPC_BT709:
    case AVCOL_SPC_UNSPECIFIED:
    default:
        return MatrixCoefficients::Bt709;
    }
}

int64_t SourceOriginUs(const AVFormatContext* format) noexcept {
    return format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time;
}

ColorRange MapRange(AVColorRange r) noexcept {
    switch (r) {
    case AVCOL_RANGE_JPEG:
        return ColorRange::Full;
    case AVCOL_RANGE_MPEG:
        return ColorRange::Limited;
    case AVCOL_RANGE_UNSPECIFIED:
    default:
        return ColorRange::Limited; // product SDR default
    }
}

// Whether this clip is native HDR10 and must be tone-mapped for an SDR screen
// rather than converted as if its samples were plain gamma-encoded.
//
// The test is deliberately narrow -- unambiguous PQ tagging AND the 10-bit
// pixel format PQ is only ever recorded in. A file carrying odd or missing
// color_trc tags then falls through to the SDR path (BT.709/Limited by
// MapMatrix/MapRange), which is the recoverable direction: an SDR clip run
// through the tone-mapper would look wrong, while a mistagged HDR clip looks
// exactly as flat as it does today.
bool IsPqTonemapSource(AVColorTransferCharacteristic trc, int pix_fmt) noexcept {
    return trc == AVCOL_TRC_SMPTE2084 && pix_fmt == AV_PIX_FMT_YUV420P10LE;
}

// ---- Bounded packet queue (demux thread -> one decode thread) -------------
//
// How much media each queue holds. The SOFT capacity is the normal buffer
// target from the design: one second per stream, comfortably longer than any
// hitch this topology exists to absorb. The HARD capacity is a pure memory
// backstop -- generous enough that it never binds in normal playback -- and is
// the only limit the demuxer will genuinely wait at while the other stream is
// running dry (see ShouldAdmitDemuxedPacket).
//
// Bounded by buffered DURATION rather than packet count on purpose -- a second
// of Opus is ~50 tiny packets and a second of 1440p60 video is 60 much larger
// ones, so any single count would either starve one stream or over-buffer the
// other. The packet COUNT bound exists only for containers that declare no
// packet durations at all (pkt->duration == 0), where the duration bound can
// never trip.
constexpr PacketQueueLimits kPacketQueueLimits{
    /*soft_capacity_us=*/1'000'000,
    /*hard_capacity_us=*/8'000'000,
    /*hard_capacity_packets=*/8192,
    /*peer_low_water_us=*/250'000,
};

// How far ahead of the playback position the demuxer reads. Matches the soft
// queue capacity: in normal playback both streams' consumers keep up, so the
// demuxer settles about one second ahead of the clock either way.
constexpr int64_t kDemuxReadAheadUs = 1'000'000;

// Bounds on the demuxer's read-ahead wait. The playback clock is a plain
// function, not an event source, so there is nothing to be notified by -- but
// the gate it closes can only reopen once the clock has advanced by the surplus
// the demuxer is ahead by, and that is a duration this thread can compute
// (DemuxReadAheadWaitUs) instead of a cadence it has to guess. The floor keeps a
// gate that closed by a hair from spinning; the cap is what a clock that stops
// moving costs before the gate is looked at again. A stop does not wait for
// either: it wakes the thread through demux_wait_cv.
constexpr int64_t kDemuxReadAheadWaitFloorUs = 2'000;
constexpr int64_t kDemuxReadAheadWaitCapUs = 50'000;

// Upper bound on how long a Push waits before re-evaluating its admission
// rule. That rule depends on the OTHER queue's level, which changes without
// this queue's condition variable being involved; the peer notifies it (see
// Pop), and this timeout makes a missed notification a 5 ms delay rather than
// a stall.
constexpr auto kPacketQueuePushRecheckInterval = std::chrono::milliseconds(5);

enum class PopResult {
    Packet,      // a packet was moved into the caller's AVPacket
    EndOfStream, // the demuxer finished and the queue is drained -- go drain the decoder
    Aborted,     // shutdown: stop immediately
};

// One stream carried by a PacketQueue.
//
// A queue can carry more than one: every audio track of the file shares a
// single queue and a single consumer, because they must be summed anyway and
// the file already delivers them in one interleaved order. That costs the
// queue two things it would not need for a single stream -- each packet's own
// time base to account for its duration, and the number of streams sharing it,
// since PacketQueueLimits is written per stream while the buffer holds the sum
// over all of them.
struct QueuedStream {
    int stream_index = -1;
    AVRational time_base{1, AV_TIME_BASE};
};

// Single-producer / single-consumer queue of refcounted packets. Packets are
// handed over with av_packet_move_ref, never copied.
//
// The two queues of one playback run are PEERED (SetPeer): the producer's
// admission rule reads the other queue's level, because a wait here withholds
// the other stream's packets too. That cross-read goes through a lock-free
// atomic mirror of the level, never through the peer's mutex, so the two
// mutexes are never held at once and no lock cycle can exist.
//
// EVERY wait below includes `aborted_`, and Abort() notifies both condition
// variables: a thread still parked in here when its join runs would be a hang,
// so waking it is not optional (see the design's shutdown section). The
// producer's wait is additionally time-bounded, so no admission-rule or
// notification mistake can turn into a permanent stall.
class PacketQueue {
  public:
    explicit PacketQueue(std::vector<QueuedStream> streams) : streams_(std::move(streams)) {
    }
    ~PacketQueue() {
        std::lock_guard<std::mutex> lock(mutex_);
        ClearLocked();
    }

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    // Set once, before any thread is spawned.
    void SetPeer(PacketQueue* peer) noexcept {
        peer_ = peer;
    }

    // Lock-free readers used by the peer's admission rule.
    [[nodiscard]] int64_t LevelUs() const noexcept {
        return level_us_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool ConsumerAlive() const noexcept {
        return consumer_alive_.load(std::memory_order_relaxed);
    }

    // Producer side. Takes ownership of *src's payload on success (src is left
    // blank, as if unreferenced). Returns false when the queue is aborted or
    // its consumer has already left -- the caller then drops the packet.
    bool Push(AVPacket* src) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            if (aborted_ || consumer_gone_)
                return false;
            if (ShouldAdmitDemuxedPacket(AdmissionStateLocked(), kPacketQueueLimits))
                break;
            ScopedPerformanceMeasurement measurement(PerformanceStage::QueueWait);
            not_full_.wait_for(lock, kPacketQueuePushRecheckInterval);
        }
        AVPacket* owned = av_packet_alloc();
        if (owned == nullptr)
            return false;
        av_packet_move_ref(owned, src);
        buffered_us_ += DurationUs(owned);
        packets_.push_back(owned);
        level_us_.store(PerStreamLevelUsLocked(), std::memory_order_relaxed);
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    // Producer side: no more packets will ever arrive for this stream.
    void SignalEndOfStream() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            end_of_stream_ = true;
        }
        not_empty_.notify_all();
    }

    // Consumer side. Moves the next packet into *dst on PopResult::Packet.
    PopResult Pop(AVPacket* dst) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return aborted_ || !packets_.empty() || end_of_stream_; });
        if (aborted_)
            return PopResult::Aborted;
        if (packets_.empty())
            return PopResult::EndOfStream;
        AVPacket* front = packets_.front();
        packets_.pop_front();
        buffered_us_ -= DurationUs(front);
        if (buffered_us_ < 0)
            buffered_us_ = 0;
        level_us_.store(PerStreamLevelUsLocked(), std::memory_order_relaxed);
        av_packet_move_ref(dst, front);
        av_packet_free(&front);
        lock.unlock();
        not_full_.notify_one();
        // This pop may have taken THIS stream below its low-water mark, which
        // is a condition the producer evaluates while parked on the OTHER
        // queue. Nothing else would wake it there.
        NotifyPeerProducer();
        return PopResult::Packet;
    }

    // Shutdown: wakes every waiter, on both sides.
    void Abort() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            aborted_ = true;
            ClearLocked();
        }
        not_empty_.notify_all();
        not_full_.notify_all();
        NotifyPeerProducer();
    }

    // Consumer side, on the way out. A decode thread that ends early (decode
    // error, cancel) must not strand the demuxer inside a Push nothing will
    // ever drain again.
    void CloseForConsumer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            consumer_gone_ = true;
            consumer_alive_.store(false, std::memory_order_relaxed);
            ClearLocked();
        }
        not_full_.notify_all();
        NotifyPeerProducer();
    }

  private:
    void NotifyPeerProducer() noexcept {
        if (peer_ != nullptr)
            peer_->not_full_.notify_one();
    }

    [[nodiscard]] PacketAdmissionState AdmissionStateLocked() const noexcept {
        PacketAdmissionState state;
        state.aborted = aborted_;
        state.queued_us = PerStreamLevelUsLocked();
        state.queued_packets = packets_.size();
        state.peer_consuming = peer_ != nullptr && peer_->ConsumerAlive();
        state.peer_queued_us = (peer_ != nullptr) ? peer_->LevelUs() : 0;
        return state;
    }

    // Buffered media time PER STREAM, which is what PacketQueueLimits and the
    // peer low-water mark are expressed in. A queue carrying two audio tracks
    // holds two packets' worth of duration for every second of media, so
    // reporting the raw sum would silently halve the demuxer's read-ahead the
    // moment a recording has a second track.
    [[nodiscard]] int64_t PerStreamLevelUsLocked() const noexcept {
        const auto stream_count = static_cast<int64_t>(std::max<size_t>(streams_.size(), 1));
        return buffered_us_ / stream_count;
    }

    [[nodiscard]] int64_t DurationUs(const AVPacket* pkt) const noexcept {
        if (pkt->duration <= 0)
            return 0; // unknown: the packet-count backstop bounds this case
        for (const QueuedStream& stream : streams_) {
            if (stream.stream_index == pkt->stream_index)
                return av_rescale_q(pkt->duration, stream.time_base, AVRational{1, AV_TIME_BASE});
        }
        return 0; // not a stream this queue carries: cannot happen, costs nothing to survive
    }

    void ClearLocked() noexcept {
        for (AVPacket* pkt : packets_)
            av_packet_free(&pkt);
        packets_.clear();
        buffered_us_ = 0;
        level_us_.store(0, std::memory_order_relaxed);
    }

    // Set once at construction; a linear scan over one or two entries costs
    // less than a map and keeps the queue allocation-free per packet.
    std::vector<QueuedStream> streams_;
    std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<AVPacket*> packets_;
    int64_t buffered_us_ = 0;
    bool end_of_stream_ = false;
    bool aborted_ = false;
    bool consumer_gone_ = false;
    // Peer-readable mirrors of the two fields the other queue's admission rule
    // needs, so that read never touches this queue's mutex.
    std::atomic<int64_t> level_us_{0};
    std::atomic<bool> consumer_alive_{true};
    PacketQueue* peer_ = nullptr;
};

// How many consecutive already-late frames the video thread tolerates before
// it asks the decoder itself to cut work (AVDISCARD_NONREF). Short enough to
// react within a few frames, long enough that a single hitch does not trip it.
constexpr int kLateFramesBeforeSkippingNonRef = 4;

// One audio track of the open file, with everything the playback path needs to
// turn its packets into the engine's fixed output format.
//
// Held by unique_ptr in Impl: the guards below are deliberately non-copyable
// and non-movable, so a vector of them could not reallocate.
struct AudioTrack {
    int stream_index = -1;
    AVRational time_base{1, AV_TIME_BASE};
    CodecCtxGuard codec;
    // Rebuilt per playback run. Owned by the audio thread while it runs; only
    // ever freed or rebuilt after that thread has been joined.
    SwrCtxGuard resampler;
};

// D3D11 accepting a stream does not imply that its readback layout is
// supported by our planar conversion, particularly for 4:4:4 surfaces.
enum AVPixelFormat SelectD3D11HwFormat(AVCodecContext* context, const enum AVPixelFormat* pix_fmts) {
    const AVPixelFormat selected =
        SelectSupportedDecodeFormat(context->sw_pix_fmt, pix_fmts, context->hw_device_ctx != nullptr);
    if (selected != AV_PIX_FMT_D3D11)
        return selected;

    AVBufferRef* parameters = nullptr;
    const int result = avcodec_get_hw_frames_parameters(context, context->hw_device_ctx, selected, &parameters);
    const bool supported =
        result >= 0 && parameters &&
        IsSupportedHwReadbackFormat(reinterpret_cast<const AVHWFramesContext*>(parameters->data)->sw_format);
    av_buffer_unref(&parameters);
    if (!supported) {
        LogWarn("D3D11 decoder readback layout is unsupported; selecting software decoding");
        return SelectSupportedDecodeFormat(context->sw_pix_fmt, pix_fmts, false);
    }
    return selected;
}

// Attempts to enable D3D11VA hardware decode for `vctx`, called immediately
// before avcodec_open2 in Open() below. Creates a device-only (no shared
// EditPlayerRenderer device -- see the design doc's "Why CPU-readback
// interop") AV_HWDEVICE_TYPE_D3D11VA context and installs the get_format
// callback above. Returns false, leaving vctx completely untouched, only
// when no D3D11VA-capable adapter exists at all -- a true return says
// nothing about whether THIS stream's profile/chroma/bit-depth will actually
// be accepted; that negotiation happens lazily inside avcodec_open2/the
// first avcodec_receive_frame, which is why Open() still checks
// avcodec_open2's own return value afterwards and retries without hardware
// on failure.
bool TryAttachD3D11VA(AVCodecContext* vctx) {
    AVBufferRef* device_ctx = nullptr;
    if (av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0)
        return false;
    vctx->hw_device_ctx = device_ctx; // ownership transferred; avcodec_free_context releases it
    vctx->get_format = SelectD3D11HwFormat;
    return true;
}

} // namespace

struct EditPlayerEngine::Impl {
    std::filesystem::path source_path;
    uint64_t audio_seeks = 0;
    uint64_t audio_decoded_frames = 0;
    std::vector<AudioSampleSpan> audio_decoded_coverage;
    struct AudioTail {
        int64_t at = 0;
        std::vector<float> samples;
    };
    std::deque<AudioTail> audio_tails;
    bool audio_only = false;
    bool audio_cursor_valid = false;
    bool audio_cursor_started = false;
    bool audio_cursor_eof = false;
    std::optional<uint32_t> audio_cursor_stream;
    int64_t audio_cursor_next = 0;
    int64_t audio_cursor_through = 0;
    std::optional<int64_t> audio_cursor_next_decoded;
    VideoReadMeasurements video_read_measurements;
    std::optional<RawDecodedVideoFrame> cursor_frame;
    std::optional<RawDecodedVideoFrame> cursor_lookahead;
    int64_t cursor_target_us = -1;
    uint64_t cursor_generation = 0;
    bool cursor_valid = false;
    bool cursor_draining = false;
    std::atomic<const std::atomic<bool>*> io_cancelled{nullptr};
    std::shared_ptr<void> media_read_lease;
    std::shared_ptr<const MediaReadCapability> source_access;
    std::unique_ptr<LeasedMediaAvio> leased_input;
    InputCtxGuard fmt;
    CodecCtxGuard video_codec;
    std::vector<std::unique_ptr<AudioTrack>> audio_tracks;
    AudioMetadataReadResult audio_metadata;
    MonitorSelectionSnapshot monitor_selection;
    // Snapshot of what AudioTracks() reports, taken at Open while no playback
    // thread exists. A snapshot rather than a live read of the format context,
    // because that context belongs to the demux thread for the whole of a
    // playback run and this getter is callable at any time.
    std::vector<AudioTrackDescription> audio_track_descriptions;
    int video_stream_idx = -1;
    ColorRange range = ColorRange::Limited;
    MatrixCoefficients matrix = MatrixCoefficients::Bt709;
    // Non-null only while a natively-HDR10 clip is open. Its transfer tables are
    // built once per clip (they depend only on the display peak), never per
    // frame, and it is read-only from every decode thread afterwards.
    std::unique_ptr<P010PqMonitorConverter> pq_converter;
    AVRational frame_rate{0, 1}; // the opened clip's own rate; {0,1} when unknown

    // Playback runs on three threads with strictly separate ownership (see
    // docs/architecture/edit-and-export.md):
    // demux owns `fmt`, video owns `video_codec`, audio owns every entry of
    // `audio_tracks`. No context is touched by two threads, which keeps the
    // engine's single-writer contract intact instead of covering it with locks.
    // Everything below is set up (seek, flushes, resampler build) on the
    // caller's thread inside StartPlaybackDecode, while no thread exists.
    //
    // Several audio tracks stay on ONE thread and ONE queue rather than
    // becoming a thread each: they arrive in a single interleaved read anyway,
    // they have to be summed on one thread in the end, and audio decode is
    // cheap next to the video side that this topology exists to decouple from.
    std::thread demux_thread;
    std::thread video_thread;
    std::thread audio_thread;
    std::unique_ptr<PacketQueue> video_packets;
    std::unique_ptr<PacketQueue> audio_packets;
    std::atomic<int> threads_alive{0};
    std::atomic<bool> playback_running{false};
    std::atomic<bool> playback_cancel{false};
    // Whether the run started by StartPlaybackDecode produces audio from AT
    // LEAST ONE track. Written there before any thread is spawned, read by the
    // caller after it returns -- see EditPlayerEngine::PlaybackDeliversAudio.
    std::atomic<bool> playback_audio_active{false};
    std::mutex playback_mutex; // guards start/stop against concurrent calls

    // The demux thread's read-ahead wait. Its bound comes from the gate's own
    // arithmetic rather than from a poll interval, so teardown has to wake it
    // instead of waiting that bound out.
    std::mutex demux_wait_mutex;
    std::condition_variable demux_wait_cv;

    [[nodiscard]] bool IsOpen() const noexcept {
        return fmt.ctx != nullptr;
    }

    bool DecodeAudioRange(std::optional<uint32_t> stream, int64_t first_sample, uint32_t frame_count,
                          std::vector<float>& output, std::string& error, bool continuous = false);

    [[nodiscard]] float MonitorGain(int stream_index) const {
        const auto description =
            std::find_if(audio_track_descriptions.begin(), audio_track_descriptions.end(),
                         [stream_index](const auto& value) { return value.stream_index == stream_index; });
        if (description == audio_track_descriptions.end() || !description->track_id)
            return 0;
        for (const auto& selection : monitor_selection.tracks)
            if (selection.track_id == *description->track_id)
                return selection.muted ? 0 : selection.gain_linear;
        return 0;
    }

    // Tears playback down in producer-to-consumer order -- demux, video, audio
    // -- waking every blocking wait before the thread holding it is joined.
    // Call with playback_mutex held.
    //
    // The two waits this cannot reach are the ones outside the engine: the
    // audio thread parked in a full WASAPI ring, and the video thread parked in
    // the caller's bounded frame queue. Both are the caller's to wake first --
    // which is why EditPlayerSession::Pause() calls audio.Stop() and releases
    // its frame queue BEFORE StopPlaybackDecode().
    void JoinPlaybackThreads() {
        // playback_cancel is already set by the caller, and the waiter tests it
        // under this mutex, so taking it here cannot slip between that test and
        // the wait it leads to.
        {
            std::lock_guard<std::mutex> lock(demux_wait_mutex);
            demux_wait_cv.notify_all();
        }
        if (video_packets)
            video_packets->Abort();
        if (audio_packets)
            audio_packets->Abort();
        if (demux_thread.joinable())
            demux_thread.join();
        if (video_thread.joinable())
            video_thread.join();
        if (audio_thread.joinable())
            audio_thread.join();
        video_packets.reset();
        audio_packets.reset();
        threads_alive.store(0);
    }

    // Called by each playback thread as it leaves. The last one out clears
    // playback_running, so a run that ended on its own (EOF) leaves the engine
    // ready for the next StartPlaybackDecode without a Stop in between.
    void NotePlaybackThreadFinished() noexcept {
        if (threads_alive.fetch_sub(1) == 1)
            playback_running.store(false);
    }
};

EditPlayerEngine::EditPlayerEngine() : impl_(std::make_unique<Impl>()) {
}

EditPlayerEngine::~EditPlayerEngine() {
    Close();
}

bool EditPlayerEngine::Open(const std::filesystem::path& path, std::string& out_error,
                            const std::atomic<bool>* cancelled) {
    return OpenSource(path, out_error, cancelled, std::nullopt);
}

bool EditPlayerEngine::OpenSource(const std::filesystem::path& path, std::string& out_error,
                                  const std::atomic<bool>* cancelled, std::optional<uint32_t> audio_stream,
                                  const std::shared_ptr<const MediaReadCapability>& source,
                                  std::optional<uint32_t> video_stream) {
    Close();
    struct ResetOpeningCancellation {
        std::atomic<const std::atomic<bool>*>& token;
        ~ResetOpeningCancellation() {
            token.store(nullptr, std::memory_order_relaxed);
        }
    } reset{impl_->io_cancelled};
    impl_->io_cancelled.store(cancelled, std::memory_order_relaxed);
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        out_error = "Media opening cancelled.";
        return false;
    }

    if (!source) {
        auto lease = AcquireMediaReadLease(path);
        if (!lease.succeeded()) {
            out_error = "Cannot retain an immutable media read identity.";
            return false;
        }
        impl_->media_read_lease = std::move(*lease.value());
    }
    impl_->source_access = source;
    impl_->source_path = path;
    impl_->audio_only = audio_stream.has_value();

    const std::string path_str = path.string();

    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        out_error = "Cannot allocate the media input context.";
        Close();
        return false;
    }
    // Protocol I/O retains a copy of this callback after Open returns.
    // The owned indirection is cleared before each borrowed operation token expires.
    fmt_ctx->interrupt_callback = {[](void* opaque) {
                                       const auto* token =
                                           static_cast<Impl*>(opaque)->io_cancelled.load(std::memory_order_relaxed);
                                       return token && token->load(std::memory_order_relaxed) ? 1 : 0;
                                   },
                                   impl_.get()};
    if (source) {
        impl_->leased_input = LeasedMediaAvio::Open(source, [impl = impl_.get()] {
            const auto* token = impl->io_cancelled.load(std::memory_order_relaxed);
            return !token || !token->load(std::memory_order_relaxed);
        });
        if (!impl_->leased_input) {
            avformat_free_context(fmt_ctx);
            out_error = "Cannot open admitted media input.";
            Close();
            return false;
        }
        fmt_ctx->pb = impl_->leased_input->context();
        fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    int ret = avformat_open_input(&fmt_ctx, source ? nullptr : path_str.c_str(), nullptr, nullptr);
    if (ret < 0) {
        out_error = std::string("avformat_open_input failed: ") + av_err2str(ret);
        Close();
        return false;
    }
    impl_->fmt.ctx = fmt_ctx;

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        out_error = std::string("avformat_find_stream_info failed: ") + av_err2str(ret);
        Close();
        return false;
    }

    if (audio_stream && (*audio_stream >= fmt_ctx->nb_streams ||
                         fmt_ctx->streams[*audio_stream]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)) {
        out_error = "The selected container stream is not audio.";
        Close();
        return false;
    }
    if (video_stream && (*video_stream >= fmt_ctx->nb_streams ||
                         fmt_ctx->streams[*video_stream]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)) {
        out_error = EXOSNAP_TRANSLATABLE("EditRender", "The selected container stream is not video.");
        Close();
        return false;
    }
    if (!audio_stream) {
        // The best video stream (av_find_best_stream picks the most likely primary
        // stream of a type -- correct for our own single-video-track recordings).
        // Audio descriptions enumerate actual streams even when provenance is
        // unavailable. Monitoring is selected separately after identity validation.
        const int video_idx = video_stream ? static_cast<int>(*video_stream)
                                           : av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

        if (video_idx < 0) {
            out_error = "no decodable video stream found";
            Close();
            return false;
        }

        AVStream* vst = fmt_ctx->streams[video_idx];
        const AVCodec* vcodec = avcodec_find_decoder(vst->codecpar->codec_id);
        if (vcodec == nullptr) {
            out_error = "no decoder available for the video codec";
            Close();
            return false;
        }
        AVCodecContext* vctx = avcodec_alloc_context3(vcodec);
        bool vctx_ready = vctx != nullptr && avcodec_parameters_to_context(vctx, vst->codecpar) >= 0;
        if (vctx_ready) {
            // 0 = let libavcodec pick the count itself: it derives one from
            // av_cpu_count() (which honours this process's affinity mask) and caps
            // it at its own MAX_AUTO_THREADS, then writes the resolved value back
            // into thread_count during avcodec_open2. Deliberately NOT the host's
            // raw std::thread::hardware_concurrency(): that removes the cap, and
            // on a high-core-count machine more frame-threading depth means more
            // in-flight decode latency on the latency-sensitive scrub path plus
            // proportionally more decoder frame-pool memory at 4K, for no measured
            // throughput gain. Frame threading costs a few frames of latency
            // either way, which is irrelevant for a player paced by an audio clock.
            vctx->thread_count = 0;
            vctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            // Attempt hardware decode for every clip, not narrowly gated to a
            // specific codec/chroma combination -- see the design doc for why
            // this is safe to try generically. TryAttachD3D11VA leaves vctx
            // untouched when it cannot create a device at all; the per-stream
            // profile/chroma/bit-depth negotiation happens inside avcodec_open2
            // itself, so a hardware-incompatible stream simply fails to open
            // here and falls back once, cleanly, to the exact call this codebase
            // already made before hardware decode existed.
            const char* software_decode = std::getenv("EXOSNAP_EDIT_SOFTWARE_DECODE");
            if (!software_decode || std::string_view(software_decode) != "1")
                TryAttachD3D11VA(vctx);
            vctx_ready = avcodec_open2(vctx, vcodec, nullptr) >= 0;
            if (!vctx_ready && vctx->hw_device_ctx != nullptr) {
                av_buffer_unref(&vctx->hw_device_ctx);
                vctx_ready = avcodec_open2(vctx, vcodec, nullptr) >= 0;
            }
        }
        if (!vctx_ready) {
            if (vctx)
                avcodec_free_context(&vctx);
            out_error = "failed to open the video decoder";
            Close();
            return false;
        }
        impl_->video_codec.ctx = vctx;
        impl_->video_stream_idx = video_idx;
        // The clip's own frame rate drives the caller's presentation cadence and
        // queue sizing (see VideoFrameRate). av_guess_frame_rate prefers the
        // container's r_frame_rate/avg_frame_rate and falls back to the codec
        // time base, so it stays sane for files whose header lies.
        impl_->frame_rate = av_guess_frame_rate(fmt_ctx, vst, nullptr);
        impl_->matrix = MapMatrix(vst->codecpar->color_space);
        impl_->range = MapRange(vst->codecpar->color_range);
        // Assigned unconditionally, so opening an SDR clip after an HDR one cannot
        // leave the tone-mapper standing.
        //
        // Same reference tone-map curve as the capture preview and snapshot path
        // (the shared reference curve, not an independent tone-map). Display peak:
        // the engine is UI-agnostic and has no screen to ask, so this is the
        // reference-peak fallback (kHdrFallbackPeakNits) -- that shifts only where
        // the highlight roll-off begins, not whether the image is readable at all.
        // Recorded in KNOWN_LIMITATIONS.
        impl_->pq_converter = IsPqTonemapSource(vst->codecpar->color_trc, vst->codecpar->format)
                                  ? std::make_unique<P010PqMonitorConverter>(HdrPeakScale(false, 0.0f, 0.0f))
                                  : nullptr;
    }

    impl_->audio_metadata = ReadAudioMediaMetadata(path, fmt_ctx, {}, {}, cancelled, source, !source);
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        out_error = "Media opening cancelled.";
        Close();
        return false;
    }
    impl_->monitor_selection.media_id = impl_->audio_metadata.selection_media_id;
    if (impl_->audio_metadata.status == AudioMetadataStatus::Verified && impl_->audio_metadata.manifest &&
        !impl_->audio_metadata.monitoring_confirmation_required)
        for (const auto& id : impl_->audio_metadata.manifest->default_monitor_track_ids)
            impl_->monitor_selection.tracks.push_back({id, 1.0f, false});
    size_t audio_ordinal = 0;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
        AVStream* ast = fmt_ctx->streams[i];
        if (ast->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;

        // Reported whether or not its decoder opens below. The description is
        // an answer about the FILE -- a track that exists but cannot be
        // decoded is still a track the recording carries, and hiding its row
        // would misrepresent the recording rather than the failure.
        AudioTrackDescription description;
        description.stream_index = static_cast<int>(i);
        if (audio_ordinal < impl_->audio_metadata.actual_tracks.size()) {
            const auto& metadata_track = impl_->audio_metadata.actual_tracks[audio_ordinal];
            description.track_id = metadata_track.track_id;
            description.role = metadata_track.role;
            description.name = metadata_track.name;
        }
        ++audio_ordinal;
        // Metadata parsing also recognizes MP4's actual handler name. A missing
        // name remains empty rather than being invented from the track ordinal.
        if (const AVDictionaryEntry* title = av_dict_get(ast->metadata, "title", nullptr, 0);
            title != nullptr && title->value != nullptr)
            description.name = title->value;
        impl_->audio_track_descriptions.push_back(std::move(description));

        if (audio_stream && i != *audio_stream)
            continue;

        const AVCodec* acodec = avcodec_find_decoder(ast->codecpar->codec_id);
        if (acodec == nullptr) {
            LogWarn("no decoder available for an audio track's codec -- that track stays silent");
            continue;
        }
        AVCodecContext* actx = avcodec_alloc_context3(acodec);
        if (actx == nullptr || avcodec_parameters_to_context(actx, ast->codecpar) < 0 ||
            avcodec_open2(actx, acodec, nullptr) < 0) {
            if (actx)
                avcodec_free_context(&actx);
            LogWarn("failed to open an audio track's decoder -- that track stays silent");
            continue;
        }
        auto track = std::make_unique<AudioTrack>();
        track->stream_index = static_cast<int>(i);
        track->time_base = ast->time_base;
        track->codec.ctx = actx;
        impl_->audio_tracks.push_back(std::move(track));
    }

    if (audio_stream && impl_->audio_tracks.empty()) {
        out_error = "The selected audio stream has no usable decoder.";
        Close();
        return false;
    }
    return true;
}

bool EditPlayerEngine::OpenVideoStream(const std::filesystem::path& path, uint32_t container_index,
                                       std::string& out_error, const std::atomic<bool>* cancelled) {
    return OpenSource(path, out_error, cancelled, {}, {}, container_index);
}

bool EditPlayerEngine::OpenVideoStream(const std::shared_ptr<const MediaReadCapability>& source,
                                       uint32_t container_index, std::string& out_error,
                                       const std::atomic<bool>* cancelled) {
    if (!source) {
        Close();
        out_error = "No admitted media source.";
        return false;
    }
    return OpenSource(source->Path(), out_error, cancelled, {}, source, container_index);
}
bool EditPlayerEngine::OpenAudioStream(const std::filesystem::path& path, uint32_t container_index,
                                       std::string& out_error, const std::atomic<bool>* cancelled) {
    return OpenSource(path, out_error, cancelled, container_index);
}

bool EditPlayerEngine::Open(const std::shared_ptr<const MediaReadCapability>& source, std::string& out_error,
                            const std::atomic<bool>* cancelled) {
    if (!source) {
        out_error = "No admitted media source.";
        return false;
    }
    return OpenSource(source->Path(), out_error, cancelled, std::nullopt, source);
}
bool EditPlayerEngine::OpenAudioStream(const std::shared_ptr<const MediaReadCapability>& source,
                                       uint32_t container_index, std::string& out_error,
                                       const std::atomic<bool>* cancelled) {
    if (!source) {
        out_error = "No admitted media source.";
        return false;
    }
    return OpenSource(source->Path(), out_error, cancelled, container_index, source);
}

bool EditPlayerEngine::DecodeAudioStreamRange(uint32_t container_index, int64_t first_sample, uint32_t frame_count,
                                              std::vector<float>& output, std::string& error) {
    return impl_->DecodeAudioRange(container_index, first_sample, frame_count, output, error);
}

bool EditPlayerEngine::ReadAudioStreamSequential(uint32_t container_index, int64_t first_sample, uint32_t frame_count,
                                                 std::vector<float>& output, std::string& error,
                                                 const std::atomic<bool>* cancelled) {
    ScopedMediaCancellation cancellation(impl_->io_cancelled, cancelled);
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        error = "Source audio read cancelled.";
        return false;
    }
    if (!impl_->audio_only) {
        impl_->audio_decoded_coverage.clear();
        error = "Continuous analysis requires an audio-only source.";
        return false;
    }
    return impl_->DecodeAudioRange(container_index, first_sample, frame_count, output, error, true);
}
bool EditPlayerEngine::ValidateContentIdentity(const std::array<uint8_t, 32>& expected, std::string& error,
                                               const std::atomic<bool>* cancelled) const {
    if (!impl_->IsOpen() || (!impl_->media_read_lease && !impl_->source_access)) {
        error = "The media source is not open.";
        return false;
    }
    const auto identity = impl_->source_access ? [&]() -> Outcome<MediaIdentitySnapshot> {
        const auto proof = VerifyMediaIdentity(impl_->source_access, expected, {}, {}, cancelled);
        return proof.succeeded() ? Outcome<MediaIdentitySnapshot>{(*proof.value())->Identity()}
                                 : Outcome<MediaIdentitySnapshot>{*proof.error()};
    }()
        : HashFinalMedia(impl_->source_path, {}, {}, cancelled, MediaHashCause::DecoderValidation);
    if (!identity.succeeded()) {
        error = identity.error()->context;
        return false;
    }
    if (identity.value()->sha256 != expected) {
        error = "The media source content changed since validation.";
        return false;
    }
    return true;
}
uint64_t EditPlayerEngine::AudioSeekCount() const noexcept {
    return impl_->audio_seeks;
}
uint64_t EditPlayerEngine::AudioDecodedFrameCount() const noexcept {
    return impl_->audio_decoded_frames;
}
const std::vector<AudioSampleSpan>& EditPlayerEngine::AudioDecodedCoverage() const noexcept {
    return impl_->audio_decoded_coverage;
}
void EditPlayerEngine::Close() {
    StopPlaybackDecode();
    // Safe only after the join above: the audio thread owns every track's
    // decoder and resampler while it runs.
    impl_->cursor_valid = false;
    impl_->cursor_frame.reset();
    impl_->cursor_lookahead.reset();
    impl_->video_read_measurements = {};
    impl_->audio_tails.clear();
    impl_->audio_only = false;
    impl_->audio_cursor_valid = false;
    impl_->audio_cursor_started = false;
    impl_->audio_cursor_eof = false;
    impl_->audio_cursor_next_decoded.reset();
    impl_->audio_seeks = 0;
    impl_->audio_decoded_frames = 0;
    impl_->audio_decoded_coverage.clear();
    impl_->source_path.clear();
    impl_->audio_tracks.clear();
    impl_->audio_track_descriptions.clear();
    if (impl_->video_codec.ctx)
        avcodec_free_context(&impl_->video_codec.ctx);
    if (impl_->fmt.ctx)
        avformat_close_input(&impl_->fmt.ctx);
    impl_->leased_input.reset();
    impl_->source_access.reset();
    impl_->media_read_lease.reset();
    impl_->audio_metadata = {};
    impl_->monitor_selection = {};
    impl_->video_stream_idx = -1;
    impl_->frame_rate = AVRational{0, 1};
    impl_->pq_converter.reset();
}

double EditPlayerEngine::VideoFrameRate() const noexcept {
    if (!impl_->IsOpen() || impl_->frame_rate.num <= 0 || impl_->frame_rate.den <= 0)
        return 0.0;
    return av_q2d(impl_->frame_rate);
}

int EditPlayerEngine::VideoWidth() const noexcept {
    if (!impl_->IsOpen() || impl_->video_codec.ctx == nullptr)
        return 0;
    return impl_->video_codec.ctx->width;
}

int EditPlayerEngine::VideoHeight() const noexcept {
    if (!impl_->IsOpen() || impl_->video_codec.ctx == nullptr)
        return 0;
    return impl_->video_codec.ctx->height;
}

bool EditPlayerEngine::PlaybackDeliversAudio() const noexcept {
    return impl_->playback_audio_active.load();
}

bool EditPlayerEngine::HasVideoStream() const noexcept {
    return impl_->IsOpen() && impl_->video_stream_idx >= 0;
}

bool EditPlayerEngine::HasAudioStream() const noexcept {
    return impl_->IsOpen() && !impl_->audio_tracks.empty();
}

namespace {

// True when the decoder emitted a frame layout this engine can convert
// (fully-planar 4:2:0, 8- or 10-bit -- what our h264/hevc/av1 software
// decoders produce for the product's own 4:2:0 recordings; plus fully-planar
// 4:4:4, 8-bit only -- what our h264/hevc software decoders produce for a
// clip recorded with the Expert 4:4:4 chroma option, docs/product-spec.md
// "Chroma": H.264/HEVC 8-bit only, no AV1, no 10-bit, no HDR10).
bool IsConvertibleFrame(const AVFrame* frame) noexcept {
    return frame->width > 0 && frame->height > 0 &&
           (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P ||
            frame->format == AV_PIX_FMT_YUV420P10LE || frame->format == AV_PIX_FMT_YUV444P);
}

// A readback failure ends this decode operation instead of silently discarding
// every frame. Normal negotiation selects software for unsupported layouts.
bool NormalizeHwFrame(AVFrame* frame) {
    if (frame->format != AV_PIX_FMT_D3D11)
        return true;
    if (!frame->hw_frames_ctx ||
        !IsSupportedHwReadbackFormat(
            reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data)->sw_format)) {
        LogWarn("D3D11 decoder produced an unexpected readback layout; video decoding stopped");
        return false;
    }
    const auto transfer = [](AVFrame* destination, AVFrame* source) {
        ScopedPerformanceMeasurement measurement(PerformanceStage::DecodeReadback);
        return av_hwframe_transfer_data(destination, source, 0);
    };
    FrameGuard sw(av_frame_alloc());
    if (sw.frame == nullptr || transfer(sw.frame, frame) < 0 || av_frame_copy_props(sw.frame, frame) < 0) {
        LogWarn("D3D11 decoder readback failed; video decoding stopped");
        return false;
    }
    AVFrame* planar = DeinterleaveHwReadbackFrame(sw.frame);
    if (planar == nullptr) {
        LogWarn("D3D11 decoder readback normalization failed; video decoding stopped");
        return false;
    }
    av_frame_unref(frame);
    av_frame_move_ref(frame, planar);
    av_frame_free(&planar);
    return true;
}

// Converts one held decoder frame to a ready-to-paint DecodedVideoFrame.
//
// `pq` is non-null only for a natively-HDR10 clip (see IsPqTonemapSource); its
// samples then take the tone-mapping path instead of the linear matrix/range
// conversion, which would render PQ material flat and washed out.
DecodedVideoFrame ConvertToDecodedFrame(const AVFrame* frame, int64_t pts_us, MatrixCoefficients matrix,
                                        ColorRange range, const P010PqMonitorConverter* pq) {
    ScopedPerformanceMeasurement measurement(PerformanceStage::CpuConversion);
    YuvToBgraParams params;
    params.matrix = matrix;
    params.range = range;

    DecodedVideoFrame out;
    out.pts_us = pts_us;
    out.width = static_cast<uint32_t>(frame->width);
    out.height = static_cast<uint32_t>(frame->height);
    out.stride_bytes = out.width * 4u;
    // new[] default-initializes (no zero fill); the conversion below writes
    // every byte anyway, so zeroing first would be a pure ~15 MB memset per
    // frame at 1440p -- measured at 2.2 ms/frame, on a 16.7 ms budget.
    const size_t bgra_bytes = static_cast<size_t>(out.stride_bytes) * out.height;
    std::shared_ptr<uint8_t[]> bgra(new uint8_t[bgra_bytes]);

    if (pq != nullptr && frame->format == AV_PIX_FMT_YUV420P10LE) {
        FullPlanarYuv420Frame src;
        src.y_plane = frame->data[0];
        src.y_stride_bytes = static_cast<uint32_t>(frame->linesize[0]);
        src.u_plane = frame->data[1];
        src.u_stride_bytes = static_cast<uint32_t>(frame->linesize[1]);
        src.v_plane = frame->data[2];
        src.v_stride_bytes = static_cast<uint32_t>(frame->linesize[2]);
        src.width = out.width;
        src.height = out.height;
        src.bits_per_sample = 10u;
        pq->Convert(src, bgra.get(), out.stride_bytes);
    } else if (frame->format == AV_PIX_FMT_YUV444P) {
        FullPlanar444Frame src;
        src.y_plane = frame->data[0];
        src.y_stride_bytes = static_cast<uint32_t>(frame->linesize[0]);
        src.u_plane = frame->data[1];
        src.u_stride_bytes = static_cast<uint32_t>(frame->linesize[1]);
        src.v_plane = frame->data[2];
        src.v_stride_bytes = static_cast<uint32_t>(frame->linesize[2]);
        src.width = out.width;
        src.height = out.height;
        ConvertFullPlanar444ToBgra(src, params, bgra.get(), out.stride_bytes);
    } else {
        FullPlanarYuv420Frame src;
        src.y_plane = frame->data[0];
        src.y_stride_bytes = static_cast<uint32_t>(frame->linesize[0]);
        src.u_plane = frame->data[1];
        src.u_stride_bytes = static_cast<uint32_t>(frame->linesize[1]);
        src.v_plane = frame->data[2];
        src.v_stride_bytes = static_cast<uint32_t>(frame->linesize[2]);
        src.width = out.width;
        src.height = out.height;
        src.bits_per_sample = (frame->format == AV_PIX_FMT_YUV420P10LE) ? 10u : 8u;
        ConvertFullPlanarYuv420ToBgra(src, params, bgra.get(), out.stride_bytes);
    }

    out.bgra = std::move(bgra);
    return out;
}

// Wraps one held decoder frame as a RawDecodedVideoFrame instead of
// converting it: ref-counts the AVFrame's own buffer (av_frame_alloc a fresh
// AVFrame*, av_frame_ref it to `frame` -- bumps the buffer refcount, no pixel
// copy) rather than color-converting into a new BGRA allocation. This is the
// entire reason RawDecodedVideoFrame exists for the GPU conversion path
// (docs/architecture/edit-and-export.md):
// ConvertToDecodedFrame's `new uint8_t[bgra_bytes]` per frame above is gone
// on this path, replaced by an AVFrame struct allocation (a few hundred
// bytes), not a multi-megabyte pixel buffer.
//
// `is_pq_source` mirrors ConvertToDecodedFrame's `pq != nullptr` check: true
// only for a natively-HDR10 (PQ) source, so the caller's GPU converter takes
// the tone-map path instead of the ordinary matrix/range conversion.
//
// Returns nullopt on an av_frame_alloc/av_frame_ref failure (OOM) -- matching
// this file's own established pattern (every other av_frame_alloc call site
// here is null-checked via FrameGuard, and DecodeForwardToTarget's finish()
// lambda checks av_frame_ref's return before trusting the ref) rather than
// dereferencing a possibly-null AVFrame*, which would crash inside libavutil.
std::optional<RawDecodedVideoFrame> WrapRawDecodedFrame(AVFrame* frame, int64_t pts_us, MatrixCoefficients matrix,
                                                        ColorRange range, bool is_pq_source) {
    RawDecodedVideoFrame out;
    out.pts_us = pts_us;
    out.width = static_cast<uint32_t>(frame->width);
    out.height = static_cast<uint32_t>(frame->height);
    out.matrix = matrix;
    out.range = range;
    out.is_pq_source = is_pq_source;
    out.primaries = static_cast<ColorPrimaries>(frame->color_primaries);
    out.transfer = static_cast<TransferCharacteristics>(frame->color_trc);
    out.format = (frame->format == AV_PIX_FMT_YUV444P)       ? DecodedPixelFormat::Yuv444P8
                 : (frame->format == AV_PIX_FMT_YUV420P10LE) ? DecodedPixelFormat::Yuv420P10
                                                             : DecodedPixelFormat::Yuv420P8;
    out.y_plane = frame->data[0];
    out.y_stride_bytes = static_cast<uint32_t>(frame->linesize[0]);
    out.u_plane = frame->data[1];
    out.u_stride_bytes = static_cast<uint32_t>(frame->linesize[1]);
    out.v_plane = frame->data[2];
    out.v_stride_bytes = static_cast<uint32_t>(frame->linesize[2]);

    // Ref-count the frame instead of copying its pixel data: av_frame_alloc a
    // fresh AVFrame*, av_frame_ref it to the decoded frame (bumps the buffer
    // refcount, no pixel copy), then hand it to shared_ptr<void> with a
    // deleter that av_frame_free's it. This is the whole reason RawDecodedVideoFrame
    // exists -- ConvertToDecodedFrame's `new uint8_t[bgra_bytes]` per frame is
    // gone; this path allocates only an AVFrame struct (a few hundred bytes),
    // not a multi-megabyte pixel buffer.
    AVFrame* ref = av_frame_alloc();
    if (ref == nullptr)
        return std::nullopt;
    if (av_frame_ref(ref, frame) != 0) {
        av_frame_free(&ref);
        return std::nullopt;
    }
    out.backing_frame = std::shared_ptr<void>(ref, [](void* p) {
        AVFrame* f = static_cast<AVFrame*>(p);
        av_frame_free(&f);
    });
    return out;
}

// Decodes forward from the decoder's current position until a frame with
// pts_us >= target_us is produced (or EOF). Shared by DecodeFrameAt (Task 5)
// and the continuous playback loop (Task 6, which calls the same primitive
// per GOP boundary). Returns nullopt on EOF-with-nothing-decoded/failure.
//
// The candidate frame is HELD as a refcounted AVFrame (av_frame_ref) and
// converted to BGRA exactly once at return: decoding forward from a keyframe
// can cover a whole GOP (100+ frames), and converting every intermediate
// frame (a multi-MB allocation + full-frame color conversion each) would make
// every scrub/trim-drag tick pay orders of magnitude more than the one
// conversion the caller actually needs.
std::optional<DecodedVideoFrame> DecodeForwardToTarget(AVFormatContext* fmt_ctx, AVCodecContext* vctx,
                                                       int video_stream_idx, int64_t target_us,
                                                       MatrixCoefficients matrix, ColorRange range,
                                                       const P010PqMonitorConverter* pq) {
    PacketGuard pkt(av_packet_alloc());
    FrameGuard frame(av_frame_alloc());
    FrameGuard held(av_frame_alloc());
    if (pkt.pkt == nullptr || frame.frame == nullptr || held.frame == nullptr)
        return std::nullopt;

    bool have_candidate = false;
    int64_t candidate_pts_us = 0;
    const AVRational tb = fmt_ctx->streams[video_stream_idx]->time_base;

    const auto finish = [&]() -> std::optional<DecodedVideoFrame> {
        if (!have_candidate || !NormalizeHwFrame(held.frame))
            return std::nullopt;
        return ConvertToDecodedFrame(held.frame, candidate_pts_us, matrix, range, pq);
    };

    while (true) {
        const int read_ret = av_read_frame(fmt_ctx, pkt.pkt);
        if (read_ret < 0) {
            avcodec_send_packet(vctx, nullptr); // enter drain mode (flush)
        } else if (pkt.pkt->stream_index != video_stream_idx) {
            av_packet_unref(pkt.pkt);
            continue;
        } else {
            // EAGAIN cannot happen here: the inner loop below always drains
            // the decoder to EAGAIN before the next send, which the avcodec
            // API contract guarantees leaves the decoder accepting input. A
            // real send error (e.g. a corrupt packet) is logged and skipped;
            // the loop keeps reading forward.
            const int send_ret = avcodec_send_packet(vctx, pkt.pkt);
            av_packet_unref(pkt.pkt);
            if (send_ret < 0)
                LogWarn((std::string("avcodec_send_packet failed: ") + av_err2str(send_ret)).c_str());
        }

        for (;;) {
            const int recv_ret = avcodec_receive_frame(vctx, frame.frame);
            if (recv_ret == AVERROR(EAGAIN)) {
                break; // need more input
            }
            if (recv_ret < 0) {
                // AVERROR_EOF (fully drained) or a real decode error: stop
                // either way with the last frame decoded before it, if any.
                return finish();
            }

            const int64_t pts_us =
                SourceRelativeFrameTimeUs(frame.frame->best_effort_timestamp, tb, SourceOriginUs(fmt_ctx));
            if (frame.frame->format == AV_PIX_FMT_D3D11 || IsConvertibleFrame(frame.frame)) {
                av_frame_unref(held.frame);
                if (av_frame_ref(held.frame, frame.frame) == 0) {
                    have_candidate = true;
                    candidate_pts_us = pts_us;
                }
            }

            if (pts_us >= target_us)
                return finish();
        }

        if (read_ret < 0)
            return finish(); // reached EOF while flushing
    }
}

// Receive before submitting another packet: a previous pull may have stopped
// with additional presentation-order frames still buffered inside the codec.
std::optional<RawDecodedVideoFrame>
ReadForwardToTargetRaw(AVFormatContext* fmt_ctx, AVCodecContext* vctx, int video_stream_idx, int64_t target_us,
                       MatrixCoefficients matrix, ColorRange range, const P010PqMonitorConverter* pq, bool& draining,
                       std::optional<RawDecodedVideoFrame>& selected, std::optional<RawDecodedVideoFrame>& lookahead,
                       VideoReadMeasurements& measurements, const std::atomic<bool>* cancelled) {
    PacketGuard pkt(av_packet_alloc());
    FrameGuard frame(av_frame_alloc());
    FrameGuard held(av_frame_alloc());
    if (!pkt.pkt || !frame.frame || !held.frame)
        return std::nullopt;
    const AVRational tb = fmt_ctx->streams[video_stream_idx]->time_base;
    bool have_candidate = false;
    int64_t candidate_pts_us = 0;
    const auto own = [&](AVFrame* candidate, int64_t pts_us) -> std::optional<RawDecodedVideoFrame> {
        if (!NormalizeHwFrame(candidate))
            return std::nullopt;
        const bool is_pq = pq != nullptr && candidate->format == AV_PIX_FMT_YUV420P10LE;
        return WrapRawDecodedFrame(candidate, pts_us, matrix, range, is_pq);
    };
    const auto finish = [&]() -> std::optional<RawDecodedVideoFrame> {
        if (have_candidate) {
            auto owned = own(held.frame, candidate_pts_us);
            if (!owned)
                return std::nullopt;
            selected = std::move(owned);
        }
        return selected;
    };
    for (;;) {
        if (cancelled && cancelled->load(std::memory_order_relaxed))
            return std::nullopt;
        const int received = avcodec_receive_frame(vctx, frame.frame);
        if (received >= 0) {
            ++measurements.decoded;
            if (frame.frame->format == AV_PIX_FMT_D3D11 || IsConvertibleFrame(frame.frame)) {
                const int64_t pts_us =
                    SourceRelativeFrameTimeUs(frame.frame->best_effort_timestamp, tb, SourceOriginUs(fmt_ctx));
                if (pts_us > target_us) {
                    auto owned = own(frame.frame, pts_us);
                    av_frame_unref(frame.frame);
                    if (!owned)
                        return std::nullopt;
                    lookahead = std::move(owned);
                    return finish();
                }
                av_frame_unref(held.frame);
                if (av_frame_ref(held.frame, frame.frame) < 0)
                    return std::nullopt;
                have_candidate = true;
                candidate_pts_us = pts_us;
                av_frame_unref(frame.frame);
                if (pts_us == target_us)
                    return finish();
            } else {
                av_frame_unref(frame.frame);
            }
            continue;
        }
        if (received == AVERROR_EOF)
            return finish();
        if (received != AVERROR(EAGAIN) || draining)
            return std::nullopt;

        int read = 0;
        do {
            av_packet_unref(pkt.pkt);
            read = av_read_frame(fmt_ctx, pkt.pkt);
        } while (read >= 0 && pkt.pkt->stream_index != video_stream_idx);
        if (read < 0) {
            draining = true;
            if (avcodec_send_packet(vctx, nullptr) < 0)
                return finish();
        } else {
            const int sent = avcodec_send_packet(vctx, pkt.pkt);
            av_packet_unref(pkt.pkt);
            if (sent < 0)
                LogWarn((std::string("avcodec_send_packet failed: ") + av_err2str(sent)).c_str());
        }
    }
}
// ---- Continuous playback decode helpers ----

// Fixed playback audio output format (see DecodedAudioBlock's contract):
// 48 kHz stereo interleaved float32, the product's own internal mix-bus
// format.
constexpr uint32_t kPlaybackOutSampleRate = 48000;
constexpr uint32_t kPlaybackOutChannels = 2;

// Builds the playback audio resampler (decoder-native format -> the fixed
// output format above), following the same swr_alloc_set_opts2 + swr_init
// convention as OutputFormatAudioSrc::BuildSwrContext. Returns nullptr on
// failure; the CALLER decides what that costs -- one track of several drops
// out, the last one leaves the run video-only.
SwrContext* BuildPlaybackResampler(AVCodecContext* actx, bool unity_mono = false) {
    AVChannelLayout out_layout{};
    av_channel_layout_default(&out_layout, static_cast<int>(kPlaybackOutChannels));
    SwrContext* swr = nullptr;
    const int ret = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, static_cast<int>(kPlaybackOutSampleRate),
                                        &actx->ch_layout, actx->sample_fmt, actx->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&out_layout);
    if (ret < 0 || swr == nullptr) {
        LogWarn("swr_alloc_set_opts2 failed -- this audio track cannot be played back");
        return nullptr;
    }
    if (unity_mono && actx->ch_layout.nb_channels == 1) {
        const double matrix[] = {1.0, 1.0};
        if (swr_set_matrix(swr, matrix, 1) < 0) {
            swr_free(&swr);
            return nullptr;
        }
    }
    if (swr_init(swr) < 0) {
        swr_free(&swr);
        LogWarn("swr_init failed -- this audio track cannot be played back");
        return nullptr;
    }
    return swr;
}

// Index of the track carrying `stream_index`, or tracks.size() when none does
// (a stream this run does not play -- the demuxer already filters those out,
// so this is the belt to that pair of braces).
size_t TrackIndexForStream(const std::vector<AudioTrack*>& tracks, int stream_index) noexcept {
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i]->stream_index == stream_index)
            return i;
    }
    return tracks.size();
}

// Moves one stream's demux read position forward to cover this packet. Uses
// pts, falling back to dts; a packet with neither leaves the position
// untouched (ShouldDemuxMorePackets then simply never paces, which is the safe
// direction -- the queue limits still bound memory). Monotonic by max() so a
// small container reordering cannot walk the position backwards.
void AdvanceDemuxPosition(int64_t& position_us, const AVPacket* pkt, AVRational tb, int64_t origin_us) noexcept {
    const int64_t raw = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
    if (raw == AV_NOPTS_VALUE)
        return;
    const int64_t pts_us = SourceRelativeTimeUs(av_rescale_q(raw, tb, AVRational{1, AV_TIME_BASE}), origin_us);
    if (position_us == kUnknownDemuxPositionUs || pts_us > position_us)
        position_us = pts_us;
}

} // namespace

std::optional<DecodedVideoFrame> EditPlayerEngine::DecodeFrameAt(int64_t target_us) {
    if (!impl_->IsOpen() || impl_->video_stream_idx < 0)
        return std::nullopt;

    target_us = std::max<int64_t>(target_us, 0);

    AVFormatContext* fmt_ctx = impl_->fmt.ctx;
    AVCodecContext* vctx = impl_->video_codec.ctx;
    AVStream* vst = fmt_ctx->streams[impl_->video_stream_idx];

    impl_->cursor_valid = false;
    impl_->cursor_frame.reset();
    impl_->cursor_lookahead.reset();
    // av_seek_frame()'s timestamp unit is the target stream's own time_base
    // when a specific stream index is passed (only the stream_index == -1
    // form takes AV_TIME_BASE), so rescale from microseconds first -- same as
    // mp4_remuxer.cpp's trim-start seek. AVSEEK_FLAG_BACKWARD snaps to the
    // keyframe at or before the target so decode-forward starts clean.
    const int64_t seek_ts = av_rescale_q(SourceTimestampUs(target_us, SourceOriginUs(fmt_ctx)),
                                         AVRational{1, AV_TIME_BASE}, vst->time_base);
    const int seek_ret = av_seek_frame(fmt_ctx, impl_->video_stream_idx, seek_ts, AVSEEK_FLAG_BACKWARD);
    if (seek_ret < 0) {
        LogWarn((std::string("av_seek_frame failed: ") + av_err2str(seek_ret)).c_str());
        return std::nullopt;
    }
    // Discard any decoder-buffered frames from before the seek.
    avcodec_flush_buffers(vctx);

    return DecodeForwardToTarget(fmt_ctx, vctx, impl_->video_stream_idx, target_us, impl_->matrix, impl_->range,
                                 impl_->pq_converter.get());
}

std::optional<RawDecodedVideoFrame> EditPlayerEngine::DecodeFrameAtRaw(int64_t target_us) {
    auto frame = ReadFrameAtRaw(target_us, VideoReadMode::RandomAccess);
    if (impl_->cursor_lookahead)
        return impl_->cursor_lookahead;
    return frame;
}

std::optional<RawDecodedVideoFrame> EditPlayerEngine::ReadFrameAtRaw(int64_t target_us, VideoReadMode mode,
                                                                     uint64_t generation,
                                                                     const std::atomic<bool>* cancelled) {
    ScopedMediaCancellation cancellation(impl_->io_cancelled, cancelled);
    if (cancelled && cancelled->load(std::memory_order_relaxed))
        return std::nullopt;
    if (!impl_->IsOpen() || impl_->video_stream_idx < 0)
        return std::nullopt;
    target_us = std::max<int64_t>(target_us, 0);
    auto* format = impl_->fmt.ctx;
    auto* codec = impl_->video_codec.ctx;
    if (mode == VideoReadMode::RandomAccess || !impl_->cursor_valid || generation != impl_->cursor_generation ||
        target_us < impl_->cursor_target_us) {
        impl_->cursor_valid = false;
        impl_->cursor_frame.reset();
        impl_->cursor_lookahead.reset();
        impl_->cursor_draining = false;
        const auto time_base = format->streams[impl_->video_stream_idx]->time_base;
        const auto timestamp =
            av_rescale_q(SourceTimestampUs(target_us, SourceOriginUs(format)), AVRational{1, AV_TIME_BASE}, time_base);
        ++impl_->video_read_measurements.seeks;
        if (av_seek_frame(format, impl_->video_stream_idx, timestamp, AVSEEK_FLAG_BACKWARD) < 0)
            return std::nullopt;
        avcodec_flush_buffers(codec);
        ++impl_->video_read_measurements.flushes;
        impl_->cursor_valid = true;
        impl_->cursor_generation = generation;
    }
    impl_->cursor_target_us = target_us;
    // A stored timestamp stands for any instant within half a tick of it.
    // Matroska commonly rounds to whole milliseconds, so the frame due at
    // 66.667 ms is stamped 67 ms; selecting strictly at or before the request
    // would show the previous frame instead and drop this one, every third
    // frame of a 30 fps source.
    // A request is itself rounded to whole microseconds, hence at least one.
    const int64_t half_tick_us = std::max<int64_t>(
        av_rescale_q(1, format->streams[impl_->video_stream_idx]->time_base, AVRational{1, AV_TIME_BASE}) / 2, 1);
    const int64_t select_us = target_us + half_tick_us;
    if (impl_->cursor_frame && (impl_->cursor_frame->pts_us == select_us ||
                                (impl_->cursor_lookahead && impl_->cursor_lookahead->pts_us > select_us))) {
        ++impl_->video_read_measurements.held;
        ++impl_->video_read_measurements.selected;
        return impl_->cursor_frame;
    }
    if (impl_->cursor_lookahead) {
        impl_->cursor_frame = std::move(impl_->cursor_lookahead);
        impl_->cursor_lookahead.reset();
        if (impl_->cursor_frame->pts_us == select_us) {
            ++impl_->video_read_measurements.selected;
            return impl_->cursor_frame;
        }
    }
    auto frame = ReadForwardToTargetRaw(format, codec, impl_->video_stream_idx, select_us, impl_->matrix, impl_->range,
                                        impl_->pq_converter.get(), impl_->cursor_draining, impl_->cursor_frame,
                                        impl_->cursor_lookahead, impl_->video_read_measurements, cancelled);
    if (frame)
        ++impl_->video_read_measurements.selected;
    else
        impl_->cursor_valid = false;
    return frame;
}
const VideoReadMeasurements& EditPlayerEngine::VideoReadStats() const noexcept {
    return impl_->video_read_measurements;
}

std::vector<AudioTrackDescription> EditPlayerEngine::AudioTracks() const {
    return impl_->audio_track_descriptions;
}

AudioMetadataReadResult EditPlayerEngine::AudioMetadata() const {
    std::lock_guard lock(impl_->playback_mutex);
    return impl_->audio_metadata;
}
bool EditPlayerEngine::SetMonitorSelection(MonitorSelectionSnapshot selection, std::string& error,
                                           const std::atomic<bool>* cancelled) {
    std::lock_guard lock(impl_->playback_mutex);
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        error = "Audio selection cancelled.";
        return false;
    }
    const auto provisional = CachedMediaAudioMetadata(impl_->source_access, false);
    auto verified = CachedMediaAudioMetadata(impl_->source_access, true);
    if (const auto current = verified ? verified : provisional)
        impl_->audio_metadata = *current;
    const auto adopted = CachedAdoptedMediaAudioMetadata(impl_->source_access);
    const bool uses_adoption = adopted && ((selection.unverified_asset_binding &&
                                            adopted->unverified_asset_binding == selection.unverified_asset_binding) ||
                                           (adopted->manifest && selection.media_id == adopted->selection_media_id &&
                                            ValidateMonitorSelection(selection, *adopted->manifest).succeeded()));
    if (uses_adoption)
        impl_->audio_metadata = *adopted;
    if (!impl_->audio_metadata.manifest && selection.unverified_asset_binding && impl_->source_access) {
        const auto& frozen = *selection.unverified_asset_binding;
        const auto& current = impl_->audio_metadata.unverified_asset_binding;
        const std::array<uint8_t, 32> pending{};
        if (current && current->sha256 == pending && frozen.sha256 != pending &&
            SameUnverifiedAsset(frozen, *current, true)) {
            // An external confirmed binding carries an expected digest, not proof
            // that a newly admitted lease has those bytes.
            const auto proof = VerifyMediaIdentity(impl_->source_access, frozen.sha256, {}, {}, cancelled);
            if (!proof.succeeded()) {
                error = proof.error()->context;
                return false;
            }
            impl_->audio_metadata = ReadAudioMediaMetadata(impl_->source_path, impl_->fmt.ctx, {}, {}, cancelled,
                                                           impl_->source_access, true);
            verified = CachedMediaAudioMetadata(impl_->source_access, true);
        }
    }
    if (!impl_->audio_metadata.manifest && selection.unverified_asset_binding) {
        const auto& current = impl_->audio_metadata.unverified_asset_binding;
        const auto& frozen = *selection.unverified_asset_binding;
        if (!selection.overlap_confirmed) {
            error = "Legacy audio tracks require explicit monitoring confirmation.";
            return false;
        }
        const auto canonical = [&](const auto& binding) {
            return binding && ((provisional && provisional->unverified_asset_binding == binding) ||
                               (verified && verified->unverified_asset_binding == binding) ||
                               (adopted && adopted->unverified_asset_binding == binding));
        };
        // Pending digests carry authority only through registry-owned bindings on this live lease.
        const bool same_live_lease =
            impl_->source_access && canonical(selection.unverified_asset_binding) && canonical(current);
        if (!current || !SameUnverifiedAsset(frozen, *current, same_live_lease)) {
            error = "Confirmed legacy selection does not match the current media bytes and track layout.";
            return false;
        }
        for (auto& track : selection.tracks) {
            const auto found = std::find_if(frozen.actual_tracks.begin(), frozen.actual_tracks.end(),
                                            [&](const auto& value) { return value.track_id == track.track_id; });
            if (found == frozen.actual_tracks.end()) {
                error = "Confirmed legacy selection references a missing bound track.";
                return false;
            }
            const auto index = static_cast<size_t>(found - frozen.actual_tracks.begin());
            track.track_id = current->actual_tracks[index].track_id;
        }
        selection.media_id = impl_->audio_metadata.selection_media_id;
        selection.unverified_asset_binding = current;
    }
    const auto& applied = impl_->monitor_selection;
    const bool identical =
        selection.media_id == applied.media_id && selection.revision == applied.revision &&
        selection.overlap_confirmed == applied.overlap_confirmed && selection.tracks.size() == applied.tracks.size() &&
        std::equal(selection.tracks.begin(), selection.tracks.end(), applied.tracks.begin(),
                   [](const auto& a, const auto& b) {
                       return a.track_id == b.track_id && a.gain_linear == b.gain_linear && a.muted == b.muted;
                   });
    if (impl_->IsOpen() && !impl_->playback_running.load() && selection.revision.value != 0 && identical) {
        impl_->audio_metadata.monitoring_confirmation_required = false;
        error.clear();
        return true;
    }
    if (!impl_->IsOpen() || impl_->playback_running.load() ||
        selection.media_id != impl_->audio_metadata.selection_media_id || selection.revision.value == 0 ||
        selection.revision.value <= impl_->monitor_selection.revision.value ||
        selection.tracks.size() > kMaxAudioTracks) {
        error = "Monitoring selection has a stale identity or revision, or playback is active.";
        return false;
    }
    if (impl_->audio_metadata.manifest) {
        if (!ValidateMonitorSelection(selection, *impl_->audio_metadata.manifest).succeeded()) {
            error = "Monitoring selection has invalid tracks, gains, or unconfirmed overlap.";
            return false;
        }
    } else if (!selection.overlap_confirmed) {
        error = "Legacy audio tracks require explicit monitoring confirmation.";
        return false;
    }
    std::set<AudioTrackId> ids;
    for (const auto& track : selection.tracks) {
        const bool known =
            std::any_of(impl_->audio_metadata.actual_tracks.begin(), impl_->audio_metadata.actual_tracks.end(),
                        [&](const auto& actual) { return actual.track_id == track.track_id; });
        if (!known || !ids.insert(track.track_id).second || !std::isfinite(track.gain_linear) ||
            track.gain_linear < 0 || track.gain_linear > GainDbToLinear(kMaxGainDb, false)) {
            error = "Monitoring selection has invalid tracks or gains.";
            return false;
        }
    }
    if (uses_adoption) {
        for (size_t index = 0; index < impl_->audio_track_descriptions.size() && index < adopted->actual_tracks.size();
             ++index) {
            auto& description = impl_->audio_track_descriptions[index];
            const auto& actual = adopted->actual_tracks[index];
            description.track_id = actual.track_id;
            description.role = actual.role;
            description.name = actual.name;
        }
    }
    impl_->monitor_selection = std::move(selection);
    impl_->audio_metadata.monitoring_confirmation_required = false;
    error.clear();
    return true;
}

bool EditPlayerEngine::DecodeAudioRange(int64_t first_sample, uint32_t frame_count, std::vector<float>& output,
                                        std::string& error, const std::atomic<bool>* cancelled) {
    ScopedMediaCancellation cancellation(impl_->io_cancelled, cancelled);
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
        error = "Source audio read cancelled.";
        return false;
    }
    return impl_->DecodeAudioRange(std::nullopt, first_sample, frame_count, output, error);
}

bool EditPlayerEngine::Impl::DecodeAudioRange(std::optional<uint32_t> stream, int64_t first_sample,
                                              uint32_t frame_count, std::vector<float>& output, std::string& error,
                                              bool continuous) {
    std::lock_guard lock(playback_mutex);
    audio_decoded_coverage.clear();
    if (!IsOpen() || first_sample < 0 || frame_count > 48000 ||
        first_sample > (std::numeric_limits<int64_t>::max)() - frame_count) {
        error = "Invalid timeline audio range.";
        return false;
    }
    output.assign(static_cast<size_t>(frame_count) * 2, 0);
    if (playback_running.load() || (!stream && audio_metadata.monitoring_confirmation_required)) {
        error = "Audio monitoring requires a confirmed selection and stopped playback.";
        return false;
    }
    if (stream &&
        (*stream >= fmt.ctx->nb_streams || fmt.ctx->streams[*stream]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)) {
        error = "The selected container stream is not audio.";
        return false;
    }
    std::vector<AudioTrack*> selected_tracks;
    std::vector<float> selected_gains;
    for (const auto& description : audio_track_descriptions) {
        if (stream && static_cast<uint32_t>(description.stream_index) != *stream)
            continue;
        const float gain = stream ? 1.0f : MonitorGain(description.stream_index);
        if (gain == 0)
            continue;
        const auto found = std::find_if(audio_tracks.begin(), audio_tracks.end(), [&](const auto& track) {
            return track->stream_index == description.stream_index;
        });
        if (found == audio_tracks.end()) {
            error = "A selected audio track has no usable decoder.";
            return false;
        }
        if (stream &&
            ((*found)->codec.ctx->ch_layout.nb_channels < 1 || (*found)->codec.ctx->ch_layout.nb_channels > 2)) {
            error = "The selected audio layout has no qualified stereo result matrix.";
            return false;
        }
        selected_tracks.push_back(found->get());
        selected_gains.push_back(gain);
    }
    if (selected_tracks.empty() || frame_count == 0)
        return true;
    const bool reuse =
        continuous && audio_cursor_valid && audio_cursor_stream == stream && audio_cursor_next == first_sample;
    const bool from_open = continuous && !audio_cursor_started && first_sample == 0;
    audio_cursor_started = true;
    audio_cursor_valid = false;
    if (!reuse) {
        audio_tails.clear();
        audio_cursor_eof = false;
        audio_cursor_next_decoded.reset();
    }
    auto* format = fmt.ctx;
    const int64_t start_us = av_rescale_q(first_sample, AVRational{1, 48000}, AVRational{1, AV_TIME_BASE});
    const int64_t origin_us = SourceOriginUs(format);
    const int64_t origin_sample = av_rescale_q(origin_us, AVRational{1, AV_TIME_BASE}, AVRational{1, 48000});
    // Decode preroll through the normal resampler so range boundaries do not
    // reset its filter at an audible sample.
    const int64_t preroll_us = std::max<int64_t>(0, start_us - 1'000'000);
    if (origin_us > 0 && preroll_us > (std::numeric_limits<int64_t>::max)() - origin_us) {
        error = "The source audio timestamp is outside the supported range.";
        return false;
    }
    const int64_t seek_us = preroll_us + origin_us;
    if (!reuse) {
        cursor_valid = false;
        cursor_frame.reset();
        cursor_lookahead.reset();
        // Opening retains priming packets before media time zero. Seeking to zero
        // can discard them and lose the AAC overlap/filter state for the first frame.
        if (!from_open) {
            ++audio_seeks;
            if (av_seek_frame(format, -1, seek_us, AVSEEK_FLAG_BACKWARD) < 0) {
                error = "Cannot seek source audio.";
                return false;
            }
        }
        for (auto* track : selected_tracks) {
            avcodec_flush_buffers(track->codec.ctx);
            if (continuous)
                track->codec.ctx->pkt_timebase = track->time_base;
            if (track->resampler.ctx)
                swr_free(&track->resampler.ctx);
            track->resampler.ctx = BuildPlaybackResampler(track->codec.ctx, stream.has_value());
            if (!track->resampler.ctx) {
                error = "Cannot resample source audio to stereo Float32.";
                return false;
            }
        }
    }
    PacketGuard packet(av_packet_alloc());
    FrameGuard frame(av_frame_alloc());
    if (!packet.pkt || !frame.frame) {
        error = "Cannot allocate audio decode buffers.";
        return false;
    }
    const int64_t last_sample = first_sample + frame_count;
    std::vector<int64_t> through(selected_tracks.size(), reuse ? audio_cursor_through : 0);
    std::vector<std::optional<int64_t>> next_sample(selected_tracks.size());
    if (reuse)
        next_sample[0] = audio_cursor_next_decoded;
    std::vector<AudioSampleSpan> coverage;
    const auto mix = [&](size_t index, int64_t at, std::span<const float> samples) {
        const auto frames = static_cast<int64_t>(samples.size() / 2);
        const auto first = std::max(first_sample, at);
        const auto end = std::min(last_sample, at + frames);
        if (first < end)
            coverage.push_back({first, end});
        for (int64_t sample = first; sample < end; ++sample)
            for (size_t channel = 0; channel < 2; ++channel)
                output[static_cast<size_t>(sample - first_sample) * 2 + channel] +=
                    samples[static_cast<size_t>(sample - at) * 2 + channel] * selected_gains[index];
    };
    for (auto it = audio_tails.begin(); it != audio_tails.end();) {
        mix(0, it->at, it->samples);
        if (it->at + static_cast<int64_t>(it->samples.size() / 2) <= last_sample)
            it = audio_tails.erase(it);
        else
            ++it;
    }
    const auto retain_tail = [&](int64_t at, std::span<const float> samples) {
        if (!continuous || at + static_cast<int64_t>(samples.size() / 2) <= last_sample)
            return true;
        const auto skip = static_cast<size_t>(std::max<int64_t>(0, last_sample - at));
        const auto tail = samples.subspan(skip * 2);
        size_t retained = tail.size();
        for (const auto& block : audio_tails)
            retained += block.samples.size();
        if (retained > 480000 * 2) {
            error = "Source audio lookahead exceeds its bounded block size.";
            return false;
        }
        audio_tails.push_back({at + static_cast<int64_t>(skip), std::vector<float>(tail.begin(), tail.end())});
        return true;
    };
    std::vector<float> scratch;
    const auto pump = [&](size_t index) {
        auto& track = *selected_tracks[index];
        for (;;) {
            const int received = avcodec_receive_frame(track.codec.ctx, frame.frame);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
                return true;
            if (received < 0) {
                error = "Source audio decode failed: " + std::string(av_err2str(received));
                return false;
            }
            ++audio_decoded_frames;
            const int64_t timestamp = frame.frame->best_effort_timestamp;
            if (timestamp == AV_NOPTS_VALUE && !next_sample[index]) {
                error = "Source audio has no initial timestamp.";
                return false;
            }
            int64_t at = timestamp == AV_NOPTS_VALUE ? *next_sample[index]
                                                     : av_rescale_q(timestamp, track.time_base, AVRational{1, 48000}) -
                                                           swr_get_delay(track.resampler.ctx, 48000) - origin_sample;
            // Container ticks can be coarser than one sample. Preserve contiguous
            // decoded sample counts instead of duplicating/dropping their rounded edges.
            const int64_t tolerance = av_rescale_q_rnd(1, track.time_base, AVRational{1, 48000}, AV_ROUND_UP);
            if (next_sample[index] && std::abs(at - *next_sample[index]) <= tolerance)
                at = *next_sample[index];
            const int capacity = swr_get_out_samples(track.resampler.ctx, frame.frame->nb_samples);
            if (capacity < 0 || capacity > 480000) {
                error = "Source audio resampler exceeded its bounded block size.";
                return false;
            }
            scratch.resize(static_cast<size_t>(capacity) * 2);
            auto* destination = reinterpret_cast<uint8_t*>(scratch.data());
            const int produced = swr_convert(track.resampler.ctx, &destination, capacity, frame.frame->extended_data,
                                             frame.frame->nb_samples);
            av_frame_unref(frame.frame);
            if (produced < 0) {
                error = "Source audio resampling failed.";
                return false;
            }
            const std::span<const float> normalized(scratch.data(), static_cast<size_t>(produced) * 2);
            mix(index, at, normalized);
            if (!retain_tail(at, normalized))
                return false;
            through[index] = std::max(through[index], at + produced);
            next_sample[index] = at + produced;
        }
    };
    while (
        !(continuous && audio_cursor_eof) &&
        !std::all_of(through.begin(), through.end(), [last_sample](int64_t value) { return value >= last_sample; })) {
        const int read = av_read_frame(format, packet.pkt);
        if (read < 0) {
            if (read != AVERROR_EOF) {
                error = "Source audio read failed: " + std::string(av_err2str(read));
                return false;
            }
            for (size_t i = 0; i < selected_tracks.size(); ++i) {
                avcodec_send_packet(selected_tracks[i]->codec.ctx, nullptr);
                if (!pump(i))
                    return false;
                auto* resampler = selected_tracks[i]->resampler.ctx;
                const int capacity = swr_get_out_samples(resampler, 0);
                if (capacity < 0 || capacity > 480000) {
                    error = "Source audio resampler flush exceeds its bounded block size.";
                    return false;
                }
                scratch.resize(static_cast<size_t>(capacity) * 2);
                auto* destination = reinterpret_cast<uint8_t*>(scratch.data());
                const int produced = swr_convert(resampler, &destination, capacity, nullptr, 0);
                if (produced < 0) {
                    error = "Source audio resampler flush failed.";
                    return false;
                }
                const int64_t at = next_sample[i].value_or(0);
                const std::span<const float> normalized(scratch.data(), static_cast<size_t>(produced) * 2);
                mix(i, at, normalized);
                if (!retain_tail(at, normalized))
                    return false;
                through[i] = std::max(through[i], at + produced);
                next_sample[i] = at + produced;
            }
            audio_cursor_eof = continuous;
            break;
        }
        for (size_t i = 0; i < selected_tracks.size(); ++i)
            if (selected_tracks[i]->stream_index == packet.pkt->stream_index) {
                const int sent = avcodec_send_packet(selected_tracks[i]->codec.ctx, packet.pkt);
                if (sent < 0 || !pump(i)) {
                    if (error.empty())
                        error = "Cannot submit source audio packet.";
                    return false;
                }
                break;
            }
        av_packet_unref(packet.pkt);
    }
    if (continuous) {
        audio_cursor_valid = true;
        audio_cursor_stream = stream;
        audio_cursor_next = last_sample;
        audio_cursor_through = through[0];
        audio_cursor_next_decoded = next_sample[0];
    }
    std::sort(coverage.begin(), coverage.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::size_t merged = 0;
    for (const auto span : coverage) {
        if (merged > 0 && span.first <= coverage[merged - 1].end)
            coverage[merged - 1].end = std::max(coverage[merged - 1].end, span.end);
        else
            coverage[merged++] = span;
    }
    coverage.resize(merged);
    audio_decoded_coverage = std::move(coverage);
    return true;
}

void EditPlayerEngine::StartPlaybackDecode(int64_t start_us, VideoFrameCallback on_video, AudioBlockCallback on_audio,
                                           std::function<int64_t()> current_media_time_us,
                                           PlaybackAudioMode audio_mode) {
    // Same demux/video/audio thread topology, same clock-gated skip decision,
    // same PacketQueue backpressure as DecodeFrameAtRaw's single-frame
    // sibling above -- docs/architecture/edit-and-export.md.
    // The video thread calls WrapRawDecodedFrame (ref-counts the decoder's
    // own buffer) rather than ConvertToDecodedFrame (a fresh BGRA allocation
    // + CPU colour conversion): this is the editor player's own continuous
    // playback, feeding EditFrameGpuConverter downstream, so paying for a CPU
    // conversion here would be pure waste. ConvertToDecodedFrame/
    // DecodeFrameAt stay in this file for the timeline thumbnail strip
    // (TimelineThumbnailSource), which has no GPU converter of its own and
    // genuinely needs ready-to-paint BGRA.
    std::lock_guard<std::mutex> lock(impl_->playback_mutex);
    if (!impl_->IsOpen() || impl_->playback_running.load())
        return;
    if (audio_mode == PlaybackAudioMode::MonitorSelection && impl_->audio_metadata.monitoring_confirmation_required) {
        LogWarn("audio monitoring requires explicit track confirmation");
        return;
    }

    // A previous run's threads may have finished on their own (EOF) without
    // StopPlaybackDecode() ever being called. Assigning over a still-joinable
    // std::thread calls std::terminate, so reap them first (they have already
    // run to completion -- playback_running is false).
    impl_->JoinPlaybackThreads();

    Impl* const impl = impl_.get();
    AVFormatContext* const fmt_ctx = impl->fmt.ctx;
    AVCodecContext* const vctx = impl->video_codec.ctx;
    AVStream* const vst = fmt_ctx->streams[impl->video_stream_idx];
    const int64_t origin_us = SourceOriginUs(fmt_ctx);
    start_us = std::max<int64_t>(start_us, 0);

    // Everything from here to the thread spawns runs on the CALLER's thread,
    // while no playback thread exists: the seek touches the format context,
    // the flushes touch every codec context and each track's resampler is
    // rebuilt -- all of which are handed to exactly one thread each below and
    // never touched from here again.
    //
    // A failed seek is logged and playback continues from the demuxer's
    // current position rather than producing nothing (same convention as
    // DecodeFrameAt).
    const int64_t seek_ts =
        av_rescale_q(SourceTimestampUs(start_us, origin_us), AVRational{1, AV_TIME_BASE}, vst->time_base);
    impl->cursor_valid = false;
    impl->cursor_frame.reset();
    impl->cursor_lookahead.reset();
    const int seek_ret = av_seek_frame(fmt_ctx, impl->video_stream_idx, seek_ts, AVSEEK_FLAG_BACKWARD);
    if (seek_ret < 0)
        LogWarn((std::string("playback av_seek_frame failed: ") + av_err2str(seek_ret)).c_str());
    avcodec_flush_buffers(vctx);
    // A previous run may have left the overload valve raised.
    vctx->skip_frame = AVDISCARD_DEFAULT;

    // Every track gets its own resampler, rebuilt per run. A track whose
    // resampler cannot be built is simply left out of this run: it must not
    // take the tracks that DID build down with it, and the mix must not wait
    // for a track that will never submit anything.
    std::vector<AudioTrack*> playing_tracks;
    std::vector<float> playing_gains;
    std::vector<QueuedStream> audio_streams;
    for (const auto& track : impl->audio_tracks) {
        if (audio_mode == PlaybackAudioMode::Disabled)
            break;
        const float gain = impl->MonitorGain(track->stream_index);
        if (gain == 0)
            continue;
        avcodec_flush_buffers(track->codec.ctx);
        if (track->resampler.ctx != nullptr)
            swr_free(&track->resampler.ctx);
        track->resampler.ctx = BuildPlaybackResampler(track->codec.ctx);
        if (track->resampler.ctx == nullptr)
            continue;
        playing_tracks.push_back(track.get());
        playing_gains.push_back(gain);
        audio_streams.push_back(QueuedStream{track->stream_index, track->time_base});
    }
    const bool audio_enabled = !playing_tracks.empty();
    // Publish it before any thread starts, so a caller reading it right after
    // StartPlaybackDecode() returns sees this run's real answer.
    impl->playback_audio_active.store(audio_enabled);
    if (audio_mode == PlaybackAudioMode::MonitorSelection) {
        if (!impl->audio_tracks.empty() && !audio_enabled)
            LogWarn("no playback audio resampler could be built -- this run is video-only");
        else if (playing_tracks.size() < impl->audio_tracks.size())
            LogWarn("an audio track has no usable playback resampler -- it stays silent this run");
    }

    const AVRational vtb = vst->time_base;
    const int video_idx = impl->video_stream_idx;

    impl->video_packets = std::make_unique<PacketQueue>(std::vector<QueuedStream>{QueuedStream{video_idx, vtb}});
    if (audio_enabled) {
        // One queue for every audio track: they arrive in a single interleaved
        // read and are summed by a single consumer, so splitting them would
        // only add queues the peering rule would then have to arbitrate
        // between.
        impl->audio_packets = std::make_unique<PacketQueue>(std::move(audio_streams));
        // Peered both ways: the "may I go past the soft limit?" rule is
        // symmetric, neither side is privileged.
        impl->video_packets->SetPeer(impl->audio_packets.get());
        impl->audio_packets->SetPeer(impl->video_packets.get());
    }

    impl->playback_cancel.store(false);
    impl->playback_running.store(true);
    impl->threads_alive.store(audio_enabled ? 3 : 2);

    // ---- Demux thread: owns the AVFormatContext ----
    std::vector<int> audio_indices;
    audio_indices.reserve(playing_tracks.size());
    for (const AudioTrack* track : playing_tracks)
        audio_indices.push_back(track->stream_index);
    impl->demux_thread = std::thread([impl, fmt_ctx, origin_us, video_idx, audio_indices = std::move(audio_indices),
                                      media_clock_us = current_media_time_us]() {
        PacketGuard pkt(av_packet_alloc());
        // Largest presentation timestamp handed on so far, video vs. audio.
        // Only the streams actually played contribute; a stray stream's
        // timestamps must not move the read position. All audio tracks share
        // one position because they share one queue and one consumer -- and
        // because the muxer emits every track in one global timestamp order,
        // so the leading audio track is the read position for all of them.
        int64_t video_pos_us = kUnknownDemuxPositionUs;
        int64_t audio_pos_us = kUnknownDemuxPositionUs;
        const bool has_audio = !audio_indices.empty();
        if (pkt.pkt != nullptr) {
            while (!impl->playback_cancel.load()) {
                const int64_t demuxed_through_us =
                    has_audio ? DemuxedThroughUs(video_pos_us, audio_pos_us) : video_pos_us;
                // Read-ahead gate. Bounded by the playback position, NOT by
                // queue occupancy: an occupancy bound makes the demuxer's
                // forward progress follow whichever consumer drains slowest,
                // which stamps that consumer's cadence onto the other
                // stream's delivery -- precisely the coupling this topology
                // exists to remove. With no clock (throughput probes,
                // video-only sessions) this never waits.
                for (;;) {
                    if (impl->playback_cancel.load())
                        break;
                    const int64_t clock_us = media_clock_us ? media_clock_us() : -1;
                    if (ShouldDemuxMorePackets(demuxed_through_us, clock_us, kDemuxReadAheadUs))
                        break;
                    const auto wait_us = std::chrono::microseconds(
                        DemuxReadAheadWaitUs(demuxed_through_us, clock_us, kDemuxReadAheadUs,
                                             kDemuxReadAheadWaitFloorUs, kDemuxReadAheadWaitCapUs));
                    std::unique_lock<std::mutex> lock(impl->demux_wait_mutex);
                    impl->demux_wait_cv.wait_for(lock, wait_us, [impl] { return impl->playback_cancel.load(); });
                }
                if (impl->playback_cancel.load())
                    break;

                const int read_ret = av_read_frame(fmt_ctx, pkt.pkt);
                if (read_ret < 0)
                    break; // EOF or a terminal read error: both decoders drain below
                // Push moves the payload (refcounted hand-off, no copy); it
                // only fails when the queue is aborted or its decode thread
                // has already left, in which case the packet is dropped.
                if (pkt.pkt->stream_index == video_idx) {
                    AdvanceDemuxPosition(video_pos_us, pkt.pkt, fmt_ctx->streams[video_idx]->time_base, origin_us);
                    if (!impl->video_packets->Push(pkt.pkt))
                        av_packet_unref(pkt.pkt);
                } else if (std::find(audio_indices.begin(), audio_indices.end(), pkt.pkt->stream_index) !=
                           audio_indices.end()) {
                    AdvanceDemuxPosition(audio_pos_us, pkt.pkt, fmt_ctx->streams[pkt.pkt->stream_index]->time_base,
                                         origin_us);
                    if (!impl->audio_packets->Push(pkt.pkt))
                        av_packet_unref(pkt.pkt);
                } else {
                    av_packet_unref(pkt.pkt); // a stream we don't play (or audio with no usable resampler)
                }
            }
        }
        impl->video_packets->SignalEndOfStream();
        if (has_audio)
            impl->audio_packets->SignalEndOfStream();
        impl->NotePlaybackThreadFinished();
    });

    // ---- Video thread: owns the video AVCodecContext ----
    impl->video_thread = std::thread([impl, vctx, vtb, origin_us, start_us, on_video = std::move(on_video),
                                      media_clock_us = std::move(current_media_time_us)]() {
        PacketGuard pkt(av_packet_alloc());
        FrameGuard frame(av_frame_alloc());
        int late_streak = 0;
        bool skipping_nonref = false;
        bool decode_failed = false;
        // A decoded frame here is wrapped (WrapRawDecodedFrame) rather than
        // converted into a fresh multi-megabyte allocation -- the per-frame
        // cost is an AVFrame struct plus a shared_ptr control block, a few
        // hundred bytes. The try/catch stays regardless: an allocation
        // failure escaping a thread body is a hard std::terminate, with none
        // of the diagnostics a normal failure path produces -- so it ends
        // THIS run's video instead, leaving teardown and the caller's own
        // error handling intact.
        try {
            if (pkt.pkt != nullptr && frame.frame != nullptr) {
                bool draining = false;
                while (!impl->playback_cancel.load()) {
                    if (!draining) {
                        const PopResult pop = impl->video_packets->Pop(pkt.pkt);
                        if (pop == PopResult::Aborted)
                            break;
                        if (pop == PopResult::EndOfStream) {
                            draining = true;
                            avcodec_send_packet(vctx, nullptr); // enter drain mode
                        } else {
                            // EAGAIN cannot happen: the receive loop below always
                            // empties the decoder before the next send (same
                            // contract note as DecodeForwardToTarget). A real send
                            // error (e.g. a corrupt packet) is logged and skipped.
                            const int send_ret = avcodec_send_packet(vctx, pkt.pkt);
                            av_packet_unref(pkt.pkt);
                            if (send_ret < 0)
                                LogWarn((std::string("playback avcodec_send_packet (video) failed: ") +
                                         av_err2str(send_ret))
                                            .c_str());
                        }
                    }

                    while (!impl->playback_cancel.load()) {
                        const int recv_ret = avcodec_receive_frame(vctx, frame.frame);
                        if (recv_ret < 0)
                            break; // EAGAIN (need more input) or EOF (fully drained)

                        const int64_t pts_us =
                            SourceRelativeFrameTimeUs(frame.frame->best_effort_timestamp, vtb, origin_us);
                        // A negative clock reading means "nobody is presenting
                        // this" -- then nothing is known to be late and nothing is
                        // discarded (see ShouldConvertDecodedFrame).
                        const int64_t now_us = media_clock_us ? media_clock_us() : -1;
                        const bool worth_converting = ShouldConvertDecodedFrame(pts_us, now_us);
                        // The frames between the keyframe the demuxer seeked to
                        // and start_us are behind the clock by construction, not
                        // because this thread is struggling. Counting them would
                        // raise the overload valve at the start of every play,
                        // which could then drop real frames just after start_us.
                        const bool preroll = pts_us < start_us;
                        if (worth_converting)
                            late_streak = 0;
                        else if (!preroll) {
                            ++late_streak;
                            RecordPerformanceEvent(PerformanceStage::DecodeDropped);
                        }

                        if (worth_converting && !NormalizeHwFrame(frame.frame)) {
                            decode_failed = true;
                            break;
                        }
                        if (worth_converting && IsConvertibleFrame(frame.frame)) {
                            // Wrap and release the decoder's frame BEFORE
                            // handing the result on: on_video is allowed to
                            // block on the consumer's bounded queue, and
                            // holding a decoder frame reference across that
                            // would pin a buffer the decoder wants back.
                            // WrapRawDecodedFrame's own av_frame_ref already
                            // gives the result its own reference on the
                            // buffer, so unref-ing frame.frame right after
                            // does not invalidate it.
                            const bool is_pq =
                                impl->pq_converter != nullptr && frame.frame->format == AV_PIX_FMT_YUV420P10LE;
                            std::optional<RawDecodedVideoFrame> out =
                                WrapRawDecodedFrame(frame.frame, pts_us, impl->matrix, impl->range, is_pq);
                            av_frame_unref(frame.frame);
                            // nullopt only on an av_frame_alloc/av_frame_ref
                            // OOM failure inside WrapRawDecodedFrame -- this
                            // frame is silently dropped rather than crashing,
                            // same as an unconvertible frame just above.
                            if (out.has_value())
                                on_video(std::move(*out));
                        } else {
                            av_frame_unref(frame.frame);
                        }

                        // Overload valve: while the thread is genuinely behind,
                        // let the decoder itself drop the frames nothing else
                        // references, and take that back the moment it catches up.
                        if (!skipping_nonref && late_streak >= kLateFramesBeforeSkippingNonRef) {
                            vctx->skip_frame = AVDISCARD_NONREF;
                            skipping_nonref = true;
                        } else if (skipping_nonref && late_streak == 0) {
                            vctx->skip_frame = AVDISCARD_DEFAULT;
                            skipping_nonref = false;
                        }
                    }

                    if (draining || decode_failed)
                        break; // EOF reached and the decoder fully drained above
                }
            }
        } catch (const std::bad_alloc&) {
            LogWarn("playback video decode ran out of memory -- ending this run's video");
        }
        if (skipping_nonref)
            vctx->skip_frame = AVDISCARD_DEFAULT;
        impl->video_packets->CloseForConsumer(); // never strand the demuxer in a Push
        impl->NotePlaybackThreadFinished();
    });

    // ---- Audio thread: owns every track's AVCodecContext and resampler ----
    if (audio_enabled) {
        impl->audio_thread = std::thread([impl, tracks = std::move(playing_tracks), gains = std::move(playing_gains),
                                          origin_us, start_us, on_audio = std::move(on_audio)]() {
            PacketGuard pkt(av_packet_alloc());
            FrameGuard frame(av_frame_alloc());
            // Sums the tracks by timestamp before anything reaches the
            // renderer. See EditAudioMixer for why that is an alignment
            // and a limiter rather than "add the two newest blocks".
            EditAudioMixer mixer;
            mixer.Reset(tracks.size(), start_us);
            // Resample scratch, reused across frames: this thread converts
            // one block per packet per track and the mix copies out of it
            // immediately, so a fresh allocation per frame would be pure
            // churn. Only ever grown, never shrunk.
            std::vector<float> pcm;

            // Hands over everything the mix is complete through. on_audio
            // may block on a full WASAPI ring -- that is the pacing, and
            // it holds up only this thread.
            const auto deliver = [&](bool end_of_stream) {
                for (;;) {
                    std::optional<EditAudioMixer::MixedBlock> mixed =
                        end_of_stream ? mixer.TakeRemainder() : mixer.Take();
                    if (!mixed.has_value())
                        break;
                    DecodedAudioBlock block;
                    block.pts_us = mixed->pts_us;
                    block.frame_count = mixed->frame_count;
                    block.interleaved_stereo =
                        std::make_shared<const std::vector<float>>(std::move(mixed->interleaved_stereo));
                    on_audio(std::move(block));
                }
            };

            // Pulls every frame one track's decoder currently has ready,
            // resamples it to the output format and submits it to the mix.
            const auto pump_track = [&](size_t index) {
                AudioTrack* const track = tracks[index];
                while (!impl->playback_cancel.load()) {
                    if (avcodec_receive_frame(track->codec.ctx, frame.frame) < 0)
                        break; // EAGAIN (need more input) or EOF (fully drained)
                    // Capture the pts BEFORE av_frame_unref resets the frame.
                    const int64_t pts_us =
                        SourceRelativeFrameTimeUs(frame.frame->best_effort_timestamp, track->time_base, origin_us);
                    const int64_t max_out_frames = swr_get_out_samples(track->resampler.ctx, frame.frame->nb_samples);
                    if (max_out_frames <= 0) {
                        av_frame_unref(frame.frame);
                        continue;
                    }
                    const size_t capacity = static_cast<size_t>(max_out_frames) * kPlaybackOutChannels;
                    if (pcm.size() < capacity)
                        pcm.resize(capacity);
                    uint8_t* out_ptr = reinterpret_cast<uint8_t*>(pcm.data());
                    const int produced = swr_convert(track->resampler.ctx, &out_ptr, static_cast<int>(max_out_frames),
                                                     frame.frame->extended_data, frame.frame->nb_samples);
                    av_frame_unref(frame.frame);
                    if (produced <= 0)
                        continue;
                    for (size_t sample = 0; sample < static_cast<size_t>(produced) * kPlaybackOutChannels; ++sample)
                        pcm[sample] *= gains[index];

                    // Trim the preroll the seek forced on us. av_seek_frame
                    // positioned on the keyframe at or before start_us, so
                    // the first blocks out of the decoder are older than the
                    // caller asked for. The video side discards its
                    // equivalent frames (ShouldConvertDecodedFrame); if audio
                    // did not, the ring would start at the keyframe while the
                    // clock is seeded to start_us and video would lead audio
                    // by that gap for the entire run.
                    const auto frames_out = static_cast<size_t>(produced);
                    const size_t preroll =
                        AudioPrerollFramesToDrop(pts_us, frames_out, kPlaybackOutSampleRate, start_us);
                    if (preroll >= frames_out)
                        continue; // entirely before the start: nothing to hand over
                    const int64_t block_pts_us = pts_us + static_cast<int64_t>(preroll) * 1'000'000 /
                                                              static_cast<int64_t>(kPlaybackOutSampleRate);
                    mixer.Submit(index, block_pts_us, pcm.data() + preroll * kPlaybackOutChannels, frames_out - preroll,
                                 av_rescale_q_rnd(1, track->time_base,
                                                  AVRational{1, static_cast<int>(kPlaybackOutSampleRate)},
                                                  AV_ROUND_UP));
                }
            };

            // Same reasoning as the video thread: an allocation failure here
            // must end this run's audio, not the process.
            try {
                if (pkt.pkt != nullptr && frame.frame != nullptr) {
                    while (!impl->playback_cancel.load()) {
                        const PopResult pop = impl->audio_packets->Pop(pkt.pkt);
                        if (pop == PopResult::Aborted)
                            break;
                        if (pop == PopResult::EndOfStream) {
                            // One shared queue means one end-of-stream for
                            // all tracks: drain every decoder, then hand
                            // over what is left in the mix regardless of
                            // which track reached how far -- nothing more
                            // can arrive to complete it.
                            for (size_t i = 0; i < tracks.size(); ++i) {
                                avcodec_send_packet(tracks[i]->codec.ctx, nullptr);
                                pump_track(i);
                            }
                            deliver(/*end_of_stream=*/true);
                            break;
                        }

                        const size_t index = TrackIndexForStream(tracks, pkt.pkt->stream_index);
                        if (index >= tracks.size()) {
                            av_packet_unref(pkt.pkt); // not a track of this run
                            continue;
                        }
                        const int send_ret = avcodec_send_packet(tracks[index]->codec.ctx, pkt.pkt);
                        av_packet_unref(pkt.pkt);
                        if (send_ret < 0)
                            LogWarn(
                                (std::string("playback avcodec_send_packet (audio) failed: ") + av_err2str(send_ret))
                                    .c_str());
                        pump_track(index);
                        deliver(/*end_of_stream=*/false);
                    }
                }
            } catch (const std::bad_alloc&) {
                LogWarn("playback audio decode ran out of memory -- ending this run's audio");
            }
            if (mixer.LateFramesDropped() > 0)
                LogWarn("playback audio mix discarded late sample frames -- a track lagged past the lookbehind");
            impl->audio_packets->CloseForConsumer();
            impl->NotePlaybackThreadFinished();
        });
    }
}

void EditPlayerEngine::StopPlaybackDecode() {
    // Same mutex as StartPlaybackDecode -- Impl's playback_mutex contract is
    // that start/stop are safe against concurrent calls. No playback thread
    // ever takes this mutex, so joining under it cannot deadlock.
    std::lock_guard<std::mutex> lock(impl_->playback_mutex);
    impl_->playback_cancel.store(true);
    impl_->JoinPlaybackThreads();
    impl_->playback_running.store(false);
    impl_->playback_audio_active.store(false); // no run, no audio -- next Start decides afresh
}

} // namespace exosnap::engine
