// mp4_remuxer.cpp — MKV → progressive MP4 stream-copy remux engine
//
// Implementation notes:
//   - Uses libavformat for demux (input) and mux (output).
//   - movflags=+faststart causes libavformat to perform a two-pass write: first
//     pass writes a temporary file, second pass moves moov before mdat.
//   - No decoder/encoder is opened; only codec parameters are copied.
//   - Progress is estimated from video PTS vs the container duration.  If the
//     input has no duration metadata (e.g. truncated MKV), progress stays at 0.
//   - On cooperative cancel the partial output file is deleted.
//   - av_err2str() uses a C99 compound literal and cannot be called from C++
//     under MSVC.  We override the macro with a thread_local buffer wrapper.
//
// Note: _CRT_SECURE_NO_WARNINGS is propagated from the FFmpeg::mux IMPORTED
// target's INTERFACE_COMPILE_DEFINITIONS — do not redefine it here.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
}

// MSVC + C++: override av_err2str to avoid C99 compound literal
static inline const char* av_err2str_cpp(int errnum) noexcept {
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}
#ifdef av_err2str
#undef av_err2str
#endif
#define av_err2str(e) av_err2str_cpp(e)

#include "recorder_core/logging/logging.h"
#include "recorder_core/mp4_remuxer.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recorder_core {

namespace {

constexpr const char* kLogComponent = "mp4_remuxer";

// Convenience wrappers: log a string without fields.
static void LogInfo(const char* msg) {
    logging::log(logging::LogLevel::Info, kLogComponent, msg);
}
static void LogWarn(const char* msg) {
    logging::log(logging::LogLevel::Warn, kLogComponent, msg);
}
static void LogError(const char* msg) {
    logging::log(logging::LogLevel::Error, kLogComponent, msg);
}

// RAII wrapper for AVFormatContext* (input)
struct InputCtxGuard {
    AVFormatContext* ctx = nullptr;
    ~InputCtxGuard() {
        if (ctx)
            avformat_close_input(&ctx);
    }
    InputCtxGuard(const InputCtxGuard&) = delete;
    InputCtxGuard& operator=(const InputCtxGuard&) = delete;
    InputCtxGuard() = default;
};

// RAII wrapper for AVFormatContext* (output) + optional avio
struct OutputCtxGuard {
    AVFormatContext* ctx = nullptr;
    bool avio_opened = false;

    ~OutputCtxGuard() {
        if (ctx) {
            if (avio_opened && !(ctx->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&ctx->pb);
            }
            avformat_free_context(ctx);
        }
    }
    OutputCtxGuard(const OutputCtxGuard&) = delete;
    OutputCtxGuard& operator=(const OutputCtxGuard&) = delete;
    OutputCtxGuard() = default;
};

// RAII wrapper for AVPacket*
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

// RAII wrapper for AVDictionary*
struct DictGuard {
    AVDictionary* dict = nullptr;
    ~DictGuard() {
        av_dict_free(&dict);
    }
    DictGuard(const DictGuard&) = delete;
    DictGuard& operator=(const DictGuard&) = delete;
    DictGuard() = default;
};

// Options for the generic stream-copy remux function.
struct RemuxOptions {
    const char* format_name = nullptr; // libavformat short name (e.g. "mp4", "matroska")
    // Extra AVDictionary key/value pairs written before avformat_write_header.
    // Pairs are (key, value); list must be even-length and null-terminated by a nullptr pair.
    const char* const* extra_opts = nullptr; // may be nullptr
};

// Generic internal stream-copy remux. Both RemuxToProgressiveMp4 and
// RemuxToMkv delegate here. opts.format_name must not be nullptr.
// tr is optional: TrimRange{} (both kNoTimestamp) means no trim.
static RemuxResult RemuxStreamCopy(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                                   RemuxProgressCallback progress_cb, const RemuxOptions& opts, TrimRange tr = {}) {
    const std::string in_str = input_path.string();
    const std::string out_str = output_path.string();

    {
        logging::LogField fields[] = {{"input", in_str}, {"output", out_str}, {"format", opts.format_name}};
        logging::log(logging::LogLevel::Info, kLogComponent, "Remux start",
                     std::span<const logging::LogField>(fields, std::size(fields)));
    }

    // -----------------------------------------------------------------------
    // 1. Open input
    // -----------------------------------------------------------------------
    InputCtxGuard in_guard;
    {
        int ret = avformat_open_input(&in_guard.ctx, in_str.c_str(), nullptr, nullptr);
        if (ret < 0) {
            std::string msg = std::string("avformat_open_input failed: ") + av_err2str(ret);
            LogError(msg.c_str());
            return RemuxResult::Fail(ret, std::move(msg));
        }
    }

    {
        int ret = avformat_find_stream_info(in_guard.ctx, nullptr);
        if (ret < 0) {
            std::string msg = std::string("avformat_find_stream_info failed: ") + av_err2str(ret);
            LogError(msg.c_str());
            return RemuxResult::Fail(ret, std::move(msg));
        }
    }

    AVFormatContext* const in_ctx = in_guard.ctx;

    // Extract input duration for progress estimation.
    // AV_NOPTS_VALUE means the container did not report it (truncated MKV, etc.).
    const double input_duration_sec = (in_ctx->duration != AV_NOPTS_VALUE && in_ctx->duration > 0)
                                          ? static_cast<double>(in_ctx->duration) / AV_TIME_BASE
                                          : 0.0;

    {
        logging::LogField fields[] = {{"nb_streams", std::to_string(in_ctx->nb_streams)},
                                      {"duration_s", std::to_string(input_duration_sec)}};
        logging::log(logging::LogLevel::Debug, kLogComponent, "Input opened",
                     std::span<const logging::LogField>(fields, std::size(fields)));
    }

    // -----------------------------------------------------------------------
    // 2. Create output context
    // -----------------------------------------------------------------------
    OutputCtxGuard out_guard;
    {
        int ret = avformat_alloc_output_context2(&out_guard.ctx, nullptr, opts.format_name, out_str.c_str());
        if (ret < 0 || !out_guard.ctx) {
            int err = (ret < 0) ? ret : AVERROR(ENOMEM);
            std::string msg = std::string("avformat_alloc_output_context2 failed: ") + av_err2str(err);
            LogError(msg.c_str());
            return RemuxResult::Fail(err, std::move(msg));
        }
    }

    AVFormatContext* const out_ctx = out_guard.ctx;

    // 'hvc1'-vs-'hev1' codec-tag selection (below) applies only to the ISOBMFF
    // (MP4) muxer; the Matroska muxer maps tracks by CodecID and ignores it.
    const bool out_is_mp4 = opts.format_name != nullptr && std::string_view(opts.format_name) == "mp4";

    // -----------------------------------------------------------------------
    // 3. Map all input streams to output streams (stream-copy)
    // -----------------------------------------------------------------------
    for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
        AVStream* const in_st = in_ctx->streams[i];
        AVStream* out_st = avformat_new_stream(out_ctx, nullptr);
        if (!out_st) {
            LogError("avformat_new_stream failed (out of memory)");
            return RemuxResult::Fail(AVERROR(ENOMEM), "avformat_new_stream failed (out of memory)");
        }

        int ret = avcodec_parameters_copy(out_st->codecpar, in_st->codecpar);
        if (ret < 0) {
            std::string msg = std::string("avcodec_parameters_copy failed: ") + av_err2str(ret);
            LogError(msg.c_str());
            return RemuxResult::Fail(ret, std::move(msg));
        }

        // Clear codec_tag so the output muxer picks the correct FourCC for its
        // own container (MP4 and MKV use different tag spaces).
        out_st->codecpar->codec_tag = 0;

        // HEVC-in-MP4: request the 'hvc1' sample-entry FourCC. With codec_tag=0
        // libavformat's mov muxer defaults to 'hev1', which permits in-band
        // parameter sets but is refused by QuickTime / Apple devices and several
        // NLEs. 'hvc1' carries the parameter sets out-of-band in the hvcC box —
        // exactly how the transient MKV already stores them (hvcC codec-private),
        // so a plain stream-copy yields a conformant Apple-compatible file
        // (ADR 0010 / ADR 0014). MKV output ignores codec_tag (it maps tracks by
        // CodecID string), so this is gated to the MP4 muxer only.
        if (out_is_mp4 && out_st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            out_st->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            out_st->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
        }

        // Color description — the MP4 output must carry the source's colour
        // identity, not a hardcoded SDR one. avcodec_parameters_copy copies the
        // color_primaries / color_trc / color_space / color_range CICP fields
        // AND the coded_side_data array from the input AVCodecParameters. So a
        // source MKV written with KaxVideoColour tags (ADR 0032) — SDR BT.709 or
        // HDR10 BT.2020/PQ — round-trips verbatim: the mov muxer emits the colr
        // (nclx) box from the CICP fields and, for HDR sources that carry
        // KaxVideoColourMasterMeta, the mdcv (mastering-display) box from the
        // copied AV_PKT_DATA_MASTERING_DISPLAY_METADATA side data. Content-light-
        // level is only emitted when the source carries it; our recordings set no
        // MaxCLL/MaxFALL, so no clli box is written (absent is correct — an empty
        // clli would be a conformance defect).
        //
        // For older or truncated files where the demuxer returns UNSPECIFIED
        // (0 / 2), apply the SDR Rec.709 limited-range fallback so the output is
        // always explicitly tagged. The fallback is suppressed when the stream
        // carries mastering-display metadata: an HDR file with a partially
        // UNSPECIFIED CICP field must not have SDR BT.709 primaries stamped over
        // it, which would desync the colr box from the mdcv box. This is
        // defensive — no current production writer emits mastering-display
        // metadata with partial CICP (our recordings are always fully tagged);
        // it protects foreign/hand-crafted inputs and future writer changes.
        if (out_st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            const bool has_mastering_display =
                av_packet_side_data_get(out_st->codecpar->coded_side_data, out_st->codecpar->nb_coded_side_data,
                                        AV_PKT_DATA_MASTERING_DISPLAY_METADATA) != nullptr;
            if (!has_mastering_display) {
                if (out_st->codecpar->color_primaries == AVCOL_PRI_UNSPECIFIED ||
                    out_st->codecpar->color_primaries == AVCOL_PRI_RESERVED0) {
                    out_st->codecpar->color_primaries = AVCOL_PRI_BT709;
                }
                if (out_st->codecpar->color_trc == AVCOL_TRC_UNSPECIFIED ||
                    out_st->codecpar->color_trc == AVCOL_TRC_RESERVED0) {
                    out_st->codecpar->color_trc = AVCOL_TRC_BT709;
                }
                if (out_st->codecpar->color_space == AVCOL_SPC_UNSPECIFIED ||
                    out_st->codecpar->color_space == AVCOL_SPC_RESERVED) {
                    out_st->codecpar->color_space = AVCOL_SPC_BT709;
                }
                if (out_st->codecpar->color_range == AVCOL_RANGE_UNSPECIFIED) {
                    out_st->codecpar->color_range = AVCOL_RANGE_MPEG;
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // 4. Open the output file
    // -----------------------------------------------------------------------
    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&out_ctx->pb, out_str.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            std::string msg = std::string("avio_open failed: ") + av_err2str(ret);
            LogError(msg.c_str());
            return RemuxResult::Fail(ret, std::move(msg));
        }
        out_guard.avio_opened = true;
    }

    // -----------------------------------------------------------------------
    // 5. Write header (with any caller-supplied muxer options)
    // -----------------------------------------------------------------------
    DictGuard header_opts;
    if (opts.extra_opts != nullptr) {
        for (int k = 0; opts.extra_opts[k] != nullptr && opts.extra_opts[k + 1] != nullptr; k += 2) {
            av_dict_set(&header_opts.dict, opts.extra_opts[k], opts.extra_opts[k + 1], 0);
        }
    }

    {
        int ret = avformat_write_header(out_ctx, &header_opts.dict);
        if (ret < 0) {
            std::string msg = std::string("avformat_write_header failed: ") + av_err2str(ret);
            LogError(msg.c_str());
            return RemuxResult::Fail(ret, std::move(msg));
        }
    }

    // -----------------------------------------------------------------------
    // 5b. Trim: seek to keyframe at/before start_us, identify video stream.
    // -----------------------------------------------------------------------
    // Find the first video stream index — needed for seek targeting and for
    // the end-trim PTS comparison in the packet loop.
    int video_stream_idx = -1;
    for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
        if (in_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = static_cast<int>(i);
            break;
        }
    }

    if (tr.HasStart()) {
        // Seek to the keyframe at or before start_us.
        // AVSEEK_FLAG_BACKWARD ensures we snap to the nearest preceding keyframe
        // so the output starts on a clean keyframe boundary.
        //
        // av_seek_frame()'s timestamp unit depends on stream_index: when a
        // specific stream is targeted (video_stream_idx >= 0, the normal case
        // here), the timestamp must already be expressed in that stream's own
        // time_base. Only the stream_index == -1 form auto-converts from
        // AV_TIME_BASE. tr.start_us is always microseconds at AV_TIME_BASE, so
        // it must be rescaled to the video stream's time_base before seeking —
        // passing it unconverted seeks to the wrong position (typically far
        // past EOF) and silently fails, which used to be masked by a fudge
        // factor in the packet-loop keyframe check below.
        const int64_t seek_ts = (video_stream_idx >= 0) ? av_rescale_q(tr.start_us, AVRational{1, AV_TIME_BASE},
                                                                       in_ctx->streams[video_stream_idx]->time_base)
                                                        : tr.start_us;
        const int seek_ret = av_seek_frame(in_ctx, video_stream_idx, seek_ts, AVSEEK_FLAG_BACKWARD);
        if (seek_ret < 0) {
            // Non-fatal: if seek fails, continue from the beginning.
            std::string msg = std::string("av_seek_frame (trim start) failed: ") + av_err2str(seek_ret);
            LogWarn(msg.c_str());
        }
    }

    // -----------------------------------------------------------------------
    // 6. Packet loop: stream-copy all packets
    // -----------------------------------------------------------------------
    PacketGuard pkt_guard{av_packet_alloc()};
    if (!pkt_guard.pkt) {
        LogError("av_packet_alloc failed (out of memory)");
        return RemuxResult::Fail(AVERROR(ENOMEM), "av_packet_alloc failed (out of memory)");
    }
    AVPacket* const pkt = pkt_guard.pkt;

    bool cancelled = false;

    // When trimming from the start, the backward seek above (when it
    // succeeds) positions the read cursor exactly at the keyframe at or
    // before start_us — the seek target. We track whether we have locked
    // onto that keyframe yet.
    bool trim_start_locked = !tr.HasStart(); // true = past the start boundary

    // Last progress value handed to the callback. Re-sent by the unconditional
    // cancellation probe below so a cancel poll never reports a bogus position.
    float last_progress = 0.0f;

    while (true) {
        // Cancellation is tested here, once per packet, unconditionally.
        //
        // It used to live inside the progress block further down, behind three
        // extra conditions: a positive container duration, stream index == 0,
        // and a valid PTS. Any one of them false disabled cancellation for the
        // whole run -- and the duration one is routinely false, because
        // matroska_stream_writer writes KaxDuration as 0.0 and only back-patches
        // it at finalize. So exactly the inputs a cancel matters for (an
        // unfinalized master from a crash, a recovery artefact, an abrupt stop)
        // were the ones that could not be cancelled. The caller joins this work
        // synchronously on the GUI thread during window close, so the failure
        // mode was a frozen UI for the length of a whole stream copy.
        //
        // Breaking here is exactly as safe as breaking at the old site: the
        // cancelled branch below closes the AVIO handle, frees the context and
        // deletes the partial output before returning ECANCELED.
        if (progress_cb && !progress_cb(last_progress)) {
            cancelled = true;
            break;
        }

        int ret = av_read_frame(in_ctx, pkt);
        if (ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            // A non-EOF read error means the source could not be read to
            // completion (corruption, disk I/O failure, etc.) — the remaining
            // packets are lost. Finalizing here would silently hand back an
            // incomplete file tagged as a success, so this is fatal. AVERROR_EOF
            // (handled above) remains the only non-fatal termination.
            std::string msg = std::string("av_read_frame failed: ") + av_err2str(ret);
            LogError(msg.c_str());
            return RemuxResult::Fail(ret, std::move(msg));
        }

        const int si = pkt->stream_index;
        if (si < 0 || static_cast<unsigned>(si) >= in_ctx->nb_streams) {
            av_packet_unref(pkt);
            continue;
        }

        AVStream* const in_st = in_ctx->streams[si];
        AVStream* const out_st = out_ctx->streams[si];

        // ---------------------------------------------------------------
        // Trim: start boundary — skip packets until we lock onto a keyframe.
        // ---------------------------------------------------------------
        if (!trim_start_locked && si == video_stream_idx) {
            // Accept the first video keyframe encountered unconditionally. A
            // successful backward seek already positions the read cursor at
            // the keyframe at or before start_us (the seek target), so the
            // first keyframe read here IS that target — no pts comparison
            // needed. If the seek failed, we fall back to a linear scan from
            // the beginning of the file, and the first keyframe encountered
            // there is still the best available "at or before" candidate.
            // Non-keyframe video packets before that point are skipped.
            if (pkt->flags & AV_PKT_FLAG_KEY) {
                trim_start_locked = true;
            } else {
                av_packet_unref(pkt);
                continue;
            }
        } else if (!trim_start_locked) {
            // Audio/other streams before we've locked onto start keyframe: skip.
            av_packet_unref(pkt);
            continue;
        }

        // ---------------------------------------------------------------
        // Trim: end boundary — stop when the video PTS exceeds end_us.
        // ---------------------------------------------------------------
        if (tr.HasEnd() && si == video_stream_idx && pkt->pts != AV_NOPTS_VALUE) {
            const int64_t pts_us = av_rescale_q(pkt->pts, in_st->time_base, {1, AV_TIME_BASE});
            if (pts_us >= tr.end_us) {
                av_packet_unref(pkt);
                break; // Past the end boundary — stop the copy loop.
            }
        }

        // Rescale timestamps from the input stream's time base to the output
        // stream's time base.
        av_packet_rescale_ts(pkt, in_st->time_base, out_st->time_base);
        pkt->pos = -1; // invalidate byte position (invalid in the new file)

        // Capture PTS for progress before the write consumes the packet.
        const int64_t pkt_pts = pkt->pts;
        const AVRational out_tb = out_st->time_base;

        ret = av_interleaved_write_frame(out_ctx, pkt);
        // av_interleaved_write_frame takes ownership of the packet data;
        // av_packet_unref is a no-op here but keeps the guard state consistent.
        av_packet_unref(pkt);

        if (ret < 0) {
            // A write failure (e.g. disk full, I/O error) means the output is
            // incomplete and will only get worse from here — surface it as a
            // fatal error rather than silently degrading to a truncated "success".
            std::string msg = std::string("av_interleaved_write_frame failed: ") + av_err2str(ret);
            LogError(msg.c_str());
            return RemuxResult::Fail(ret, std::move(msg));
        }

        // Progress reporting. Keyed on the video stream rather than on index 0:
        // "stream 0 is typically video" was a guess, and it reports nothing at
        // all for a file whose video is not the first track. Still requires a
        // known duration -- without one there is no denominator, so the run is
        // simply progress-less. Cancellation no longer depends on any of this;
        // it is handled at the top of the loop.
        if (progress_cb && input_duration_sec > 0.0 && si == video_stream_idx && pkt_pts != AV_NOPTS_VALUE) {
            const double pts_sec = static_cast<double>(pkt_pts) * av_q2d(out_tb);
            last_progress = std::clamp(static_cast<float>(pts_sec / input_duration_sec), 0.0f, 1.0f);

            if (!progress_cb(last_progress)) {
                cancelled = true;
                break;
            }
        }
    }

    // -----------------------------------------------------------------------
    // 7. Finalize (or clean up on cancel)
    // -----------------------------------------------------------------------
    if (cancelled) {
        // Close handles before deleting the partial file.
        if (out_guard.avio_opened && !(out_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&out_ctx->pb);
            out_guard.avio_opened = false;
        }
        avformat_free_context(out_ctx);
        out_guard.ctx = nullptr;

        std::error_code ec;
        std::filesystem::remove(output_path, ec);

        LogInfo("Remux cancelled by caller — partial output removed");
        return RemuxResult::Fail(AVERROR(ECANCELED), "Remux cancelled by caller");
    }

    {
        int ret = av_write_trailer(out_ctx);
        if (ret < 0) {
            // The trailer carries the faststart moov (or, for Matroska, the Cues /
            // SeekHead / Duration) — without it the output is not the well-formed
            // file the caller is about to treat as the new source of truth. Fatal,
            // matching the write-failure handling above.
            std::string msg = std::string("av_write_trailer failed: ") + av_err2str(ret);
            LogError(msg.c_str());
            return RemuxResult::Fail(ret, std::move(msg));
        }
    }

    // Signal 100% progress.
    if (progress_cb)
        progress_cb(1.0f);

    const auto out_size = std::filesystem::file_size(output_path);
    {
        logging::LogField fields[] = {{"output_bytes", std::to_string(out_size)}};
        logging::log(logging::LogLevel::Info, kLogComponent, "Remux complete",
                     std::span<const logging::LogField>(fields, std::size(fields)));
    }

    return RemuxResult::Ok();
}

} // namespace

RemuxResult RemuxToProgressiveMp4(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                                  RemuxProgressCallback progress_cb) {
    return RemuxToProgressiveMp4(input_path, output_path, std::move(progress_cb), TrimRange{});
}

RemuxResult RemuxToProgressiveMp4(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                                  RemuxProgressCallback progress_cb, TrimRange tr) {
    // movflags=+faststart: two-pass write that moves moov before mdat.
    static const char* const kMp4Opts[] = {"movflags", "+faststart", nullptr};
    RemuxOptions opts;
    opts.format_name = "mp4";
    opts.extra_opts = kMp4Opts;
    return RemuxStreamCopy(input_path, output_path, std::move(progress_cb), opts, tr);
}

RemuxResult RemuxToMkv(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                       RemuxProgressCallback progress_cb) {
    return RemuxToMkv(input_path, output_path, std::move(progress_cb), TrimRange{});
}

RemuxResult RemuxToMkv(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                       RemuxProgressCallback progress_cb, TrimRange tr) {
    // The matroska muxer writes Cues, SeekHead, and Duration at trailer time.
    // No extra options needed: libavformat defaults are correct for recovery MKV.
    RemuxOptions opts;
    opts.format_name = "matroska";
    opts.extra_opts = nullptr;
    return RemuxStreamCopy(input_path, output_path, std::move(progress_cb), opts, tr);
}

std::vector<int64_t> ExtractKeyframeTimestamps(const std::filesystem::path& input_path) {
    // Strategy: open the file, call avformat_find_stream_info (which reads the Matroska
    // Cues track and populates the per-stream index), then read index entries for the
    // video stream that are flagged AVINDEX_KEYFRAME.
    //
    // We use index entries (from Cues) rather than per-packet AV_PKT_FLAG_KEY because
    // the H.264 / AV1 bitstream parsers can override the packet keyframe flag based on
    // payload content.  Index entries come from the Cues track written by
    // MatroskaStreamWriter and are reliable regardless of payload content.
    //
    // Log noise from codec probing of test fixtures (e.g. "Invalid NAL unit size") is
    // suppressed during avformat_find_stream_info.

    const std::string in_str = input_path.string();

    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, in_str.c_str(), nullptr, nullptr) < 0)
        return {};
    struct CtxGuard {
        AVFormatContext* c;
        ~CtxGuard() {
            if (c)
                avformat_close_input(&c);
        }
    } g{ctx};

    // Suppress codec-probe noise (fake H.264/AV1 stubs generate "Invalid NAL" etc.)
    const int saved_log = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);
    avformat_find_stream_info(ctx, nullptr); // initialises codec params

    // FFmpeg's Matroska demuxer loads Cues lazily on the first seek.  Trigger that
    // load now so avformat_index_get_entries_count returns the full keyframe list.
    avformat_seek_file(ctx, -1, INT64_MIN, 0, INT64_MAX, 0);
    av_log_set_level(saved_log);

    // Find the first video stream.
    int vs_idx = -1;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vs_idx = static_cast<int>(i);
            break;
        }
    }
    if (vs_idx < 0)
        return {};

    AVStream* const vs = ctx->streams[vs_idx];
    const AVRational vs_tb = vs->time_base;
    const int n_idx = avformat_index_get_entries_count(vs);

    std::vector<int64_t> keyframes;
    keyframes.reserve(static_cast<size_t>(n_idx));

    for (int i = 0; i < n_idx; ++i) {
        const AVIndexEntry* ie = avformat_index_get_entry(vs, i);
        if (!ie)
            break;
        if (ie->flags & AVINDEX_KEYFRAME) {
            const int64_t ts_us = av_rescale_q(ie->timestamp, vs_tb, {1, AV_TIME_BASE});
            keyframes.push_back(ts_us);
        }
    }

    std::sort(keyframes.begin(), keyframes.end());
    return keyframes;
}

} // namespace recorder_core
