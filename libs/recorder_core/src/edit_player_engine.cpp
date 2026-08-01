#include "recorder_core/edit_player_engine.h"

#include "edit_playback_pacing.h"
#include "recorder_core/logging/logging.h"
#include "yuv_to_bgra.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
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

namespace recorder_core {

namespace {

constexpr const char* kLogComponent = "edit_player_engine";

void LogWarn(const char* msg) {
    logging::log(logging::LogLevel::Warn, kLogComponent, msg);
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

// Maps a container's own CICP color tags to recorder_core's color-metadata
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

// ---- Bounded packet queue (demux thread -> one decode thread) -------------
//
// How much media each queue may hold before the demuxer has to wait. One
// second is the starting value from the design: comfortably longer than any
// hitch this topology exists to absorb, so a video-side stall leaves audio
// roughly a second of already-demuxed packets to keep going on.
//
// Bounded by buffered DURATION rather than packet count on purpose -- a second
// of Opus is ~50 tiny packets and a second of 1440p60 video is 60 much larger
// ones, so any single count would either starve one stream or over-buffer the
// other.
constexpr int64_t kPacketQueueCapacityUs = 1'000'000;

// Memory backstop for containers that declare no packet durations at all
// (pkt->duration == 0), where the duration bound above can never trip. Large
// enough that it is never the binding limit for real material.
constexpr size_t kPacketQueueMaxPackets = 1024;

enum class PopResult {
    Packet,      // a packet was moved into the caller's AVPacket
    EndOfStream, // the demuxer finished and the queue is drained -- go drain the decoder
    Aborted,     // shutdown: stop immediately
};

// Single-producer / single-consumer queue of refcounted packets. Packets are
// handed over with av_packet_move_ref, never copied.
//
// EVERY wait predicate below includes `aborted_`, and Abort() notifies both
// condition variables: a thread still parked in here when its join runs would
// be a hang, so waking it is not optional (see the design's shutdown section).
class PacketQueue {
  public:
    explicit PacketQueue(AVRational time_base) noexcept : time_base_(time_base) {
    }
    ~PacketQueue() {
        std::lock_guard<std::mutex> lock(mutex_);
        ClearLocked();
    }

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    // Producer side. Takes ownership of *src's payload on success (src is left
    // blank, as if unreferenced). Returns false when the queue is aborted or
    // its consumer has already left -- the caller then drops the packet.
    bool Push(AVPacket* src) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] {
            return aborted_ || consumer_gone_ ||
                   (buffered_us_ < kPacketQueueCapacityUs && packets_.size() < kPacketQueueMaxPackets);
        });
        if (aborted_ || consumer_gone_)
            return false;
        AVPacket* owned = av_packet_alloc();
        if (owned == nullptr)
            return false;
        av_packet_move_ref(owned, src);
        buffered_us_ += DurationUs(owned);
        packets_.push_back(owned);
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
        av_packet_move_ref(dst, front);
        av_packet_free(&front);
        lock.unlock();
        not_full_.notify_one();
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
    }

    // Consumer side, on the way out. A decode thread that ends early (decode
    // error, cancel) must not strand the demuxer inside a Push nothing will
    // ever drain again.
    void CloseForConsumer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            consumer_gone_ = true;
            ClearLocked();
        }
        not_full_.notify_all();
    }

  private:
    [[nodiscard]] int64_t DurationUs(const AVPacket* pkt) const noexcept {
        if (pkt->duration <= 0)
            return 0; // unknown: the packet-count backstop bounds this case
        return av_rescale_q(pkt->duration, time_base_, AVRational{1, AV_TIME_BASE});
    }

    void ClearLocked() noexcept {
        for (AVPacket* pkt : packets_)
            av_packet_free(&pkt);
        packets_.clear();
        buffered_us_ = 0;
    }

    AVRational time_base_{1, AV_TIME_BASE};
    std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<AVPacket*> packets_;
    int64_t buffered_us_ = 0;
    bool end_of_stream_ = false;
    bool aborted_ = false;
    bool consumer_gone_ = false;
};

// How many consecutive already-late frames the video thread tolerates before
// it asks the decoder itself to cut work (AVDISCARD_NONREF). Short enough to
// react within a few frames, long enough that a single hitch does not trip it.
constexpr int kLateFramesBeforeSkippingNonRef = 4;

} // namespace

struct EditPlayerEngine::Impl {
    InputCtxGuard fmt;
    CodecCtxGuard video_codec;
    CodecCtxGuard audio_codec;
    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    ColorRange range = ColorRange::Limited;
    MatrixCoefficients matrix = MatrixCoefficients::Bt709;
    AVRational frame_rate{0, 1}; // the opened clip's own rate; {0,1} when unknown

    // Playback runs on three threads with strictly separate ownership (see
    // docs/superpowers/specs/2026-08-01-edit-player-decoupled-decode-design.md):
    // demux owns `fmt`, video owns `video_codec`, audio owns `audio_codec` and
    // `resampler`. No context is touched by two threads, which keeps the
    // engine's single-writer contract intact instead of covering it with locks.
    // Everything below is set up (seek, flushes, resampler build) on the
    // caller's thread inside StartPlaybackDecode, while no thread exists.
    std::thread demux_thread;
    std::thread video_thread;
    std::thread audio_thread;
    std::unique_ptr<PacketQueue> video_packets;
    std::unique_ptr<PacketQueue> audio_packets;
    std::atomic<int> threads_alive{0};
    std::atomic<bool> playback_running{false};
    std::atomic<bool> playback_cancel{false};
    std::mutex playback_mutex; // guards start/stop against concurrent calls
    // Playback audio resampler (decoder-native -> 48 kHz stereo float32).
    // Owned by the audio thread while it runs; only ever freed/rebuilt after
    // that thread has been joined (StartPlaybackDecode/Close), never
    // concurrently with it.
    SwrCtxGuard resampler;

    [[nodiscard]] bool IsOpen() const noexcept {
        return fmt.ctx != nullptr;
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

bool EditPlayerEngine::Open(const std::filesystem::path& path, std::string& out_error) {
    Close(); // tear down any previous session first

    const std::string path_str = path.string();

    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, path_str.c_str(), nullptr, nullptr);
    if (ret < 0) {
        out_error = std::string("avformat_open_input failed: ") + av_err2str(ret);
        return false;
    }
    impl_->fmt.ctx = fmt_ctx;

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        out_error = std::string("avformat_find_stream_info failed: ") + av_err2str(ret);
        Close();
        return false;
    }

    // Locate the best video and audio streams (av_find_best_stream picks the
    // most likely primary stream of each type -- correct for our own
    // single-video-track, up-to-N-audio-track recordings).
    const int video_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audio_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

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
        // avcodec defaults thread_count to 1, i.e. no decoder threading at all
        // -- measured on a 2560x1440@60 recording, the whole playback path then
        // peaks below realtime. 0 lets libavcodec pick a count from the host's
        // core count; frame threading adds a few frames of decode latency,
        // which is irrelevant for a player paced by an audio clock anyway.
        vctx->thread_count = 0;
        vctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        vctx_ready = avcodec_open2(vctx, vcodec, nullptr) >= 0;
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

    if (audio_idx >= 0) {
        AVStream* ast = fmt_ctx->streams[audio_idx];
        const AVCodec* acodec = avcodec_find_decoder(ast->codecpar->codec_id);
        if (acodec != nullptr) {
            AVCodecContext* actx = avcodec_alloc_context3(acodec);
            if (actx != nullptr && avcodec_parameters_to_context(actx, ast->codecpar) >= 0 &&
                avcodec_open2(actx, acodec, nullptr) >= 0) {
                impl_->audio_codec.ctx = actx;
                impl_->audio_stream_idx = audio_idx;
            } else {
                if (actx)
                    avcodec_free_context(&actx);
                LogWarn("failed to open the audio decoder -- continuing video-only");
            }
        } else {
            LogWarn("no decoder available for the audio codec -- continuing video-only");
        }
    }

    return true;
}

void EditPlayerEngine::Close() {
    StopPlaybackDecode();
    // Safe only after the join above: the playback thread owns the resampler
    // while it runs.
    if (impl_->resampler.ctx)
        swr_free(&impl_->resampler.ctx);
    if (impl_->video_codec.ctx)
        avcodec_free_context(&impl_->video_codec.ctx);
    if (impl_->audio_codec.ctx)
        avcodec_free_context(&impl_->audio_codec.ctx);
    if (impl_->fmt.ctx)
        avformat_close_input(&impl_->fmt.ctx);
    impl_->video_stream_idx = -1;
    impl_->audio_stream_idx = -1;
    impl_->frame_rate = AVRational{0, 1};
}

double EditPlayerEngine::VideoFrameRate() const noexcept {
    if (!impl_->IsOpen() || impl_->frame_rate.num <= 0 || impl_->frame_rate.den <= 0)
        return 0.0;
    return av_q2d(impl_->frame_rate);
}

bool EditPlayerEngine::HasVideoStream() const noexcept {
    return impl_->IsOpen() && impl_->video_stream_idx >= 0;
}

bool EditPlayerEngine::HasAudioStream() const noexcept {
    return impl_->IsOpen() && impl_->audio_stream_idx >= 0;
}

namespace {

// True when the decoder emitted a frame layout this engine can convert
// (fully-planar 4:2:0, 8- or 10-bit -- what our h264/hevc/av1 software
// decoders produce for the product's own recordings).
bool IsConvertibleFrame(const AVFrame* frame) noexcept {
    return frame->width > 0 && frame->height > 0 &&
           (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUV420P10LE);
}

// Converts one held decoder frame to a ready-to-paint DecodedVideoFrame.
DecodedVideoFrame ConvertToDecodedFrame(const AVFrame* frame, int64_t pts_us, MatrixCoefficients matrix,
                                        ColorRange range) {
    FullPlanarYuv420Frame src;
    src.y_plane = frame->data[0];
    src.y_stride_bytes = static_cast<uint32_t>(frame->linesize[0]);
    src.u_plane = frame->data[1];
    src.u_stride_bytes = static_cast<uint32_t>(frame->linesize[1]);
    src.v_plane = frame->data[2];
    src.v_stride_bytes = static_cast<uint32_t>(frame->linesize[2]);
    src.width = static_cast<uint32_t>(frame->width);
    src.height = static_cast<uint32_t>(frame->height);
    src.bits_per_sample = (frame->format == AV_PIX_FMT_YUV420P10LE) ? 10u : 8u;

    YuvToBgraParams params;
    params.matrix = matrix;
    params.range = range;

    DecodedVideoFrame out;
    out.pts_us = pts_us;
    out.width = src.width;
    out.height = src.height;
    out.stride_bytes = src.width * 4u;
    // new[] default-initializes (no zero fill); the conversion below writes
    // every byte anyway, so zeroing first would be a pure ~15 MB memset per
    // frame at 1440p -- measured at 2.2 ms/frame, on a 16.7 ms budget.
    const size_t bgra_bytes = static_cast<size_t>(out.stride_bytes) * out.height;
    std::shared_ptr<uint8_t[]> bgra(new uint8_t[bgra_bytes]);
    ConvertFullPlanarYuv420ToBgra(src, params, bgra.get(), out.stride_bytes);
    out.bgra = std::move(bgra);
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
                                                       MatrixCoefficients matrix, ColorRange range) {
    PacketGuard pkt(av_packet_alloc());
    FrameGuard frame(av_frame_alloc());
    FrameGuard held(av_frame_alloc());
    if (pkt.pkt == nullptr || frame.frame == nullptr || held.frame == nullptr)
        return std::nullopt;

    bool have_candidate = false;
    int64_t candidate_pts_us = 0;
    const AVRational tb = fmt_ctx->streams[video_stream_idx]->time_base;

    const auto finish = [&]() -> std::optional<DecodedVideoFrame> {
        if (!have_candidate)
            return std::nullopt;
        return ConvertToDecodedFrame(held.frame, candidate_pts_us, matrix, range);
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

            // best_effort_timestamp already falls back from a missing pts to
            // the interpolated dts-based estimate; a frame with neither is
            // treated as t=0 rather than fed to av_rescale_q as
            // AV_NOPTS_VALUE (which would rescale into garbage).
            const int64_t raw_pts =
                (frame.frame->best_effort_timestamp != AV_NOPTS_VALUE) ? frame.frame->best_effort_timestamp : 0;
            const int64_t pts_us = av_rescale_q(raw_pts, tb, AVRational{1, AV_TIME_BASE});

            if (IsConvertibleFrame(frame.frame)) {
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

// ---- Continuous playback decode helpers ----

// Fixed playback audio output format (see DecodedAudioBlock's contract):
// 48 kHz stereo interleaved float32, the product's own internal mix-bus
// format.
constexpr uint32_t kPlaybackOutSampleRate = 48000;
constexpr uint32_t kPlaybackOutChannels = 2;

// Builds the playback audio resampler (decoder-native format -> the fixed
// output format above), following the same swr_alloc_set_opts2 + swr_init
// convention as OutputFormatAudioSrc::BuildSwrContext. Returns nullptr on
// failure; playback then continues video-only for the session.
SwrContext* BuildPlaybackResampler(AVCodecContext* actx) {
    AVChannelLayout out_layout{};
    av_channel_layout_default(&out_layout, static_cast<int>(kPlaybackOutChannels));
    SwrContext* swr = nullptr;
    const int ret = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, static_cast<int>(kPlaybackOutSampleRate),
                                        &actx->ch_layout, actx->sample_fmt, actx->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&out_layout);
    if (ret < 0 || swr == nullptr) {
        LogWarn("swr_alloc_set_opts2 failed -- continuing video-only for this playback session");
        return nullptr;
    }
    if (swr_init(swr) < 0) {
        swr_free(&swr);
        LogWarn("swr_init failed -- continuing video-only for this playback session");
        return nullptr;
    }
    return swr;
}

// Microsecond pts for one decoded frame, with the same missing-timestamp
// fallback DecodeForwardToTarget applies (best_effort_timestamp, else t=0 --
// never AV_NOPTS_VALUE into av_rescale_q).
int64_t FramePtsUs(const AVFrame* frame, AVRational tb) noexcept {
    const int64_t raw = (frame->best_effort_timestamp != AV_NOPTS_VALUE) ? frame->best_effort_timestamp : 0;
    return av_rescale_q(raw, tb, AVRational{1, AV_TIME_BASE});
}

} // namespace

std::optional<DecodedVideoFrame> EditPlayerEngine::DecodeFrameAt(int64_t target_us) {
    if (!impl_->IsOpen() || impl_->video_stream_idx < 0)
        return std::nullopt;

    target_us = std::max<int64_t>(target_us, 0);

    AVFormatContext* fmt_ctx = impl_->fmt.ctx;
    AVCodecContext* vctx = impl_->video_codec.ctx;
    AVStream* vst = fmt_ctx->streams[impl_->video_stream_idx];

    // av_seek_frame()'s timestamp unit is the target stream's own time_base
    // when a specific stream index is passed (only the stream_index == -1
    // form takes AV_TIME_BASE), so rescale from microseconds first -- same as
    // mp4_remuxer.cpp's trim-start seek. AVSEEK_FLAG_BACKWARD snaps to the
    // keyframe at or before the target so decode-forward starts clean.
    const int64_t seek_ts = av_rescale_q(target_us, AVRational{1, AV_TIME_BASE}, vst->time_base);
    const int seek_ret = av_seek_frame(fmt_ctx, impl_->video_stream_idx, seek_ts, AVSEEK_FLAG_BACKWARD);
    if (seek_ret < 0) {
        LogWarn((std::string("av_seek_frame failed: ") + av_err2str(seek_ret)).c_str());
        return std::nullopt;
    }
    // Discard any decoder-buffered frames from before the seek.
    avcodec_flush_buffers(vctx);

    return DecodeForwardToTarget(fmt_ctx, vctx, impl_->video_stream_idx, target_us, impl_->matrix, impl_->range);
}

void EditPlayerEngine::StartPlaybackDecode(int64_t start_us, VideoFrameCallback on_video, AudioBlockCallback on_audio,
                                           std::function<int64_t()> current_media_time_us) {
    std::lock_guard<std::mutex> lock(impl_->playback_mutex);
    if (!impl_->IsOpen() || impl_->playback_running.load())
        return;

    // A previous run's threads may have finished on their own (EOF) without
    // StopPlaybackDecode() ever being called. Assigning over a still-joinable
    // std::thread calls std::terminate, so reap them first (they have already
    // run to completion -- playback_running is false).
    impl_->JoinPlaybackThreads();

    Impl* const impl = impl_.get();
    AVFormatContext* const fmt_ctx = impl->fmt.ctx;
    AVCodecContext* const vctx = impl->video_codec.ctx;
    AVCodecContext* const actx = impl->audio_codec.ctx;
    AVStream* const vst = fmt_ctx->streams[impl->video_stream_idx];
    start_us = std::max<int64_t>(start_us, 0);

    // Everything from here to the thread spawns runs on the CALLER's thread,
    // while no playback thread exists: the seek touches the format context,
    // the flushes touch both codec contexts and the resampler is rebuilt --
    // all of which are handed to exactly one thread each below and never
    // touched from here again.
    //
    // A failed seek is logged and playback continues from the demuxer's
    // current position rather than producing nothing (same convention as
    // DecodeFrameAt).
    const int64_t seek_ts = av_rescale_q(start_us, AVRational{1, AV_TIME_BASE}, vst->time_base);
    const int seek_ret = av_seek_frame(fmt_ctx, impl->video_stream_idx, seek_ts, AVSEEK_FLAG_BACKWARD);
    if (seek_ret < 0)
        LogWarn((std::string("playback av_seek_frame failed: ") + av_err2str(seek_ret)).c_str());
    avcodec_flush_buffers(vctx);
    // A previous run may have left the overload valve raised.
    vctx->skip_frame = AVDISCARD_DEFAULT;
    if (actx != nullptr)
        avcodec_flush_buffers(actx);

    if (impl->resampler.ctx != nullptr)
        swr_free(&impl->resampler.ctx);
    if (actx != nullptr)
        impl->resampler.ctx = BuildPlaybackResampler(actx);
    const bool audio_enabled = (actx != nullptr && impl->resampler.ctx != nullptr);

    const AVRational vtb = vst->time_base;
    const AVRational atb =
        audio_enabled ? fmt_ctx->streams[impl->audio_stream_idx]->time_base : AVRational{1, AV_TIME_BASE};

    impl->video_packets = std::make_unique<PacketQueue>(vtb);
    if (audio_enabled)
        impl->audio_packets = std::make_unique<PacketQueue>(atb);

    impl->playback_cancel.store(false);
    impl->playback_running.store(true);
    impl->threads_alive.store(audio_enabled ? 3 : 2);

    // ---- Demux thread: owns the AVFormatContext ----
    const int video_idx = impl->video_stream_idx;
    const int audio_idx = audio_enabled ? impl->audio_stream_idx : -1;
    impl->demux_thread = std::thread([impl, fmt_ctx, video_idx, audio_idx]() {
        PacketGuard pkt(av_packet_alloc());
        if (pkt.pkt != nullptr) {
            while (!impl->playback_cancel.load()) {
                const int read_ret = av_read_frame(fmt_ctx, pkt.pkt);
                if (read_ret < 0)
                    break; // EOF or a terminal read error: both decoders drain below
                // Push moves the payload (refcounted hand-off, no copy); it
                // only fails when the queue is aborted or its decode thread
                // has already left, in which case the packet is dropped.
                if (pkt.pkt->stream_index == video_idx) {
                    if (!impl->video_packets->Push(pkt.pkt))
                        av_packet_unref(pkt.pkt);
                } else if (audio_idx >= 0 && pkt.pkt->stream_index == audio_idx) {
                    if (!impl->audio_packets->Push(pkt.pkt))
                        av_packet_unref(pkt.pkt);
                } else {
                    av_packet_unref(pkt.pkt); // a stream we don't play (or audio with no usable resampler)
                }
            }
        }
        impl->video_packets->SignalEndOfStream();
        if (audio_idx >= 0)
            impl->audio_packets->SignalEndOfStream();
        impl->NotePlaybackThreadFinished();
    });

    // ---- Video thread: owns the video AVCodecContext ----
    impl->video_thread = std::thread([impl, vctx, vtb, start_us, on_video = std::move(on_video),
                                      media_clock_us = std::move(current_media_time_us)]() {
        PacketGuard pkt(av_packet_alloc());
        FrameGuard frame(av_frame_alloc());
        int late_streak = 0;
        bool skipping_nonref = false;
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
                            LogWarn(
                                (std::string("playback avcodec_send_packet (video) failed: ") + av_err2str(send_ret))
                                    .c_str());
                    }
                }

                while (!impl->playback_cancel.load()) {
                    const int recv_ret = avcodec_receive_frame(vctx, frame.frame);
                    if (recv_ret < 0)
                        break; // EAGAIN (need more input) or EOF (fully drained)

                    const int64_t pts_us = FramePtsUs(frame.frame, vtb);
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
                    else if (!preroll)
                        ++late_streak;

                    if (worth_converting && IsConvertibleFrame(frame.frame)) {
                        // Convert and release the decoder's frame BEFORE
                        // handing the result on: on_video is allowed to block
                        // on the consumer's bounded queue, and holding a
                        // decoder frame reference across that would pin a
                        // buffer the decoder wants back.
                        DecodedVideoFrame out = ConvertToDecodedFrame(frame.frame, pts_us, impl->matrix, impl->range);
                        av_frame_unref(frame.frame);
                        on_video(std::move(out));
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

                if (draining)
                    break; // EOF reached and the decoder fully drained above
            }
        }
        if (skipping_nonref)
            vctx->skip_frame = AVDISCARD_DEFAULT;
        impl->video_packets->CloseForConsumer(); // never strand the demuxer in a Push
        impl->NotePlaybackThreadFinished();
    });

    // ---- Audio thread: owns the audio AVCodecContext and the resampler ----
    if (audio_enabled) {
        SwrContext* const swr = impl->resampler.ctx;
        impl->audio_thread = std::thread([impl, actx, atb, swr, on_audio = std::move(on_audio)]() {
            PacketGuard pkt(av_packet_alloc());
            FrameGuard frame(av_frame_alloc());
            if (pkt.pkt != nullptr && frame.frame != nullptr) {
                bool draining = false;
                while (!impl->playback_cancel.load()) {
                    if (!draining) {
                        const PopResult pop = impl->audio_packets->Pop(pkt.pkt);
                        if (pop == PopResult::Aborted)
                            break;
                        if (pop == PopResult::EndOfStream) {
                            draining = true;
                            avcodec_send_packet(actx, nullptr);
                        } else {
                            const int send_ret = avcodec_send_packet(actx, pkt.pkt);
                            av_packet_unref(pkt.pkt);
                            if (send_ret < 0)
                                LogWarn((std::string("playback avcodec_send_packet (audio) failed: ") +
                                         av_err2str(send_ret))
                                            .c_str());
                        }
                    }

                    while (!impl->playback_cancel.load()) {
                        if (avcodec_receive_frame(actx, frame.frame) < 0)
                            break;
                        // Capture the pts BEFORE av_frame_unref resets the frame.
                        const int64_t pts_us = FramePtsUs(frame.frame, atb);
                        const int64_t max_out_frames = swr_get_out_samples(swr, frame.frame->nb_samples);
                        if (max_out_frames <= 0) {
                            av_frame_unref(frame.frame);
                            continue;
                        }
                        auto pcm = std::make_shared<std::vector<float>>(static_cast<size_t>(max_out_frames) *
                                                                        kPlaybackOutChannels);
                        uint8_t* out_ptr = reinterpret_cast<uint8_t*>(pcm->data());
                        const int produced = swr_convert(swr, &out_ptr, static_cast<int>(max_out_frames),
                                                         frame.frame->extended_data, frame.frame->nb_samples);
                        av_frame_unref(frame.frame);
                        if (produced <= 0)
                            continue;
                        pcm->resize(static_cast<size_t>(produced) * kPlaybackOutChannels);
                        DecodedAudioBlock block;
                        block.pts_us = pts_us;
                        block.frame_count = static_cast<uint32_t>(produced);
                        block.interleaved_stereo = std::move(pcm);
                        on_audio(std::move(block)); // may block on a full WASAPI ring -- only THIS thread
                    }

                    if (draining)
                        break;
                }
            }
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
}

} // namespace recorder_core
