#include "recorder_core/edit_player_engine.h"

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

    std::thread playback_thread;
    std::atomic<bool> playback_running{false};
    std::atomic<bool> playback_cancel{false};
    std::mutex playback_mutex; // guards start/stop against concurrent calls
    // Playback audio resampler (decoder-native -> 48 kHz stereo float32).
    // Built by the playback thread per session; only ever freed/rebuilt after
    // that thread has been joined (StartPlaybackDecode/Close), never
    // concurrently with it.
    SwrCtxGuard resampler;

    [[nodiscard]] bool IsOpen() const noexcept {
        return fmt.ctx != nullptr;
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

void EditPlayerEngine::StartPlaybackDecode(int64_t start_us, VideoFrameCallback on_video, AudioBlockCallback on_audio) {
    std::lock_guard<std::mutex> lock(impl_->playback_mutex);
    if (!impl_->IsOpen() || impl_->playback_running.load())
        return;

    // A previous session's thread may have finished on its own (EOF) without
    // StopPlaybackDecode() ever being called. Assigning over a still-joinable
    // std::thread calls std::terminate, so reap it first (it has already run
    // to completion -- playback_running is false).
    if (impl_->playback_thread.joinable())
        impl_->playback_thread.join();

    impl_->playback_cancel.store(false);
    impl_->playback_running.store(true);

    impl_->playback_thread = std::thread([impl = impl_.get(), start_us = std::max<int64_t>(start_us, 0),
                                          on_video = std::move(on_video), on_audio = std::move(on_audio)]() {
        AVFormatContext* fmt_ctx = impl->fmt.ctx;
        AVCodecContext* vctx = impl->video_codec.ctx;
        AVCodecContext* actx = impl->audio_codec.ctx;

        // Seek to the keyframe at or before start_us and discard any
        // decoder-buffered frames from before it -- same convention as
        // DecodeFrameAt. A failed seek is logged and playback continues from
        // the demuxer's current position rather than producing nothing.
        AVStream* vst = fmt_ctx->streams[impl->video_stream_idx];
        const int64_t seek_ts = av_rescale_q(start_us, AVRational{1, AV_TIME_BASE}, vst->time_base);
        const int seek_ret = av_seek_frame(fmt_ctx, impl->video_stream_idx, seek_ts, AVSEEK_FLAG_BACKWARD);
        if (seek_ret < 0)
            LogWarn((std::string("playback av_seek_frame failed: ") + av_err2str(seek_ret)).c_str());
        avcodec_flush_buffers(vctx);
        if (actx != nullptr)
            avcodec_flush_buffers(actx);

        // (Re)build the audio resampler for this session. Freeing a previous
        // session's context is safe here: that session's thread was joined
        // before this one was spawned.
        if (impl->resampler.ctx != nullptr)
            swr_free(&impl->resampler.ctx);
        if (actx != nullptr)
            impl->resampler.ctx = BuildPlaybackResampler(actx);
        const bool audio_enabled = (actx != nullptr && impl->resampler.ctx != nullptr);

        PacketGuard pkt(av_packet_alloc());
        FrameGuard frame(av_frame_alloc());
        if (pkt.pkt == nullptr || frame.frame == nullptr) {
            impl->playback_running.store(false);
            return;
        }

        const AVRational vtb = vst->time_base;
        const AVRational atb =
            audio_enabled ? fmt_ctx->streams[impl->audio_stream_idx]->time_base : AVRational{1, AV_TIME_BASE};

        bool draining = false;
        while (!impl->playback_cancel.load()) {
            const int read_ret = av_read_frame(fmt_ctx, pkt.pkt);
            if (read_ret < 0) {
                // EOF (or a terminal read error): enter drain mode on both
                // decoders so their buffered frames still come out below.
                draining = true;
                avcodec_send_packet(vctx, nullptr);
                if (audio_enabled)
                    avcodec_send_packet(actx, nullptr);
            } else if (pkt.pkt->stream_index == impl->video_stream_idx) {
                // EAGAIN cannot happen: the drain loops below always empty
                // both decoders before the next send (same contract note as
                // DecodeForwardToTarget). A real send error (e.g. a corrupt
                // packet) is logged and that packet skipped.
                const int send_ret = avcodec_send_packet(vctx, pkt.pkt);
                av_packet_unref(pkt.pkt);
                if (send_ret < 0)
                    LogWarn(
                        (std::string("playback avcodec_send_packet (video) failed: ") + av_err2str(send_ret)).c_str());
            } else if (audio_enabled && pkt.pkt->stream_index == impl->audio_stream_idx) {
                const int send_ret = avcodec_send_packet(actx, pkt.pkt);
                av_packet_unref(pkt.pkt);
                if (send_ret < 0)
                    LogWarn(
                        (std::string("playback avcodec_send_packet (audio) failed: ") + av_err2str(send_ret)).c_str());
            } else {
                av_packet_unref(pkt.pkt); // a stream we don't play (or audio with no usable resampler)
            }

            // Drain video frames. Every decoded frame is delivered (that is
            // the point of continuous playback), so the one BGRA conversion +
            // allocation per frame inside ConvertToDecodedFrame is the
            // structural minimum -- the buffer's ownership crosses to the
            // consumer via the DecodedVideoFrame shared_ptr, so it cannot be
            // pooled or reused here.
            while (!impl->playback_cancel.load()) {
                if (avcodec_receive_frame(vctx, frame.frame) < 0)
                    break; // EAGAIN (need more input) or EOF (fully drained)
                if (IsConvertibleFrame(frame.frame))
                    on_video(
                        ConvertToDecodedFrame(frame.frame, FramePtsUs(frame.frame, vtb), impl->matrix, impl->range));
                av_frame_unref(frame.frame);
            }

            // Drain audio frames, resampling each to 48 kHz stereo float32.
            while (audio_enabled && !impl->playback_cancel.load()) {
                if (avcodec_receive_frame(actx, frame.frame) < 0)
                    break;
                // Capture the pts BEFORE av_frame_unref resets the frame.
                const int64_t pts_us = FramePtsUs(frame.frame, atb);
                const int64_t max_out_frames = swr_get_out_samples(impl->resampler.ctx, frame.frame->nb_samples);
                if (max_out_frames <= 0) {
                    av_frame_unref(frame.frame);
                    continue;
                }
                auto pcm =
                    std::make_shared<std::vector<float>>(static_cast<size_t>(max_out_frames) * kPlaybackOutChannels);
                uint8_t* out_ptr = reinterpret_cast<uint8_t*>(pcm->data());
                const int produced = swr_convert(impl->resampler.ctx, &out_ptr, static_cast<int>(max_out_frames),
                                                 frame.frame->extended_data, frame.frame->nb_samples);
                av_frame_unref(frame.frame);
                if (produced <= 0)
                    continue;
                pcm->resize(static_cast<size_t>(produced) * kPlaybackOutChannels);
                DecodedAudioBlock block;
                block.pts_us = pts_us;
                block.frame_count = static_cast<uint32_t>(produced);
                block.interleaved_stereo = std::move(pcm);
                on_audio(std::move(block));
            }

            if (draining)
                break; // EOF reached and both decoders fully drained above
        }

        impl->playback_running.store(false);
    });
}

void EditPlayerEngine::StopPlaybackDecode() {
    // Same mutex as StartPlaybackDecode -- Impl's playback_mutex contract is
    // that start/stop are safe against concurrent calls. The playback thread
    // itself never takes this mutex, so joining under it cannot deadlock.
    std::lock_guard<std::mutex> lock(impl_->playback_mutex);
    impl_->playback_cancel.store(true);
    if (impl_->playback_thread.joinable())
        impl_->playback_thread.join();
    impl_->playback_running.store(false);
}

} // namespace recorder_core
