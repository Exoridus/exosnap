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

#include <cerrno>
#include <cstdint>
#include <ratio>
#include <system_error>
#include <utility>
extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/macros.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
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

#include "exosnap/engine/color_metadata.h"
#include "exosnap/engine/logging/logging.h"
#include "exosnap/engine/mp4_remuxer.h"
#include "matroska_packet_timestamps.h"
#include "matroska_stream_writer.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace exosnap::engine {

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

static bool HasExactPacketTimestamps(const AVPacket* packet) {
    size_t size = 0;
    const auto* data = av_packet_get_side_data(packet, AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, &size);
    if (!data || size < 8)
        return false;
    uint64_t type = 0;
    for (size_t i = 0; i < 8; ++i)
        type = (type << 8) | data[i];
    return type == kPacketTimestampMappingType;
}

static bool ConfigureExactTimestampRewrite(const AVFormatContext* input, const std::string& output,
                                           MatroskaStreamConfig& config, std::vector<uint64_t>& track_numbers) {
    config.output_path = output;
    config.path_pre_reserved = std::filesystem::exists(output);
    track_numbers.resize(input->nb_streams);
    bool video_found = false;
    AVCodecID audio_codec = AV_CODEC_ID_NONE;
    for (unsigned i = 0; i < input->nb_streams; ++i) {
        const auto* stream = input->streams[i];
        const auto* par = stream->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && !video_found) {
            switch (par->codec_id) {
            case AV_CODEC_ID_H264:
                config.video_codec_id = "V_MPEG4/ISO/AVC";
                break;
            case AV_CODEC_ID_HEVC:
                config.video_codec_id = "V_MPEGH/ISO/HEVC";
                break;
            case AV_CODEC_ID_AV1:
                config.video_codec_id = "V_AV1";
                break;
            default:
                return false;
            }
            video_found = true;
            track_numbers[i] = 1;
            config.encode_width = static_cast<uint32_t>(par->width);
            config.encode_height = static_cast<uint32_t>(par->height);
            if (par->extradata_size > 0)
                config.video_codec_private.assign(par->extradata, par->extradata + par->extradata_size);
            if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
                config.frame_rate_num = static_cast<uint32_t>(stream->avg_frame_rate.num);
                config.frame_rate_den = static_cast<uint32_t>(stream->avg_frame_rate.den);
            }
            config.color.primaries = static_cast<ColorPrimaries>(par->color_primaries);
            config.color.transfer = static_cast<TransferCharacteristics>(par->color_trc);
            config.color.matrix = static_cast<MatrixCoefficients>(par->color_space);
            config.color.range = par->color_range == AVCOL_RANGE_JPEG   ? ColorRange::Full
                                 : par->color_range == AVCOL_RANGE_MPEG ? ColorRange::Limited
                                                                        : ColorRange::Unspecified;
            if (const auto* descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(par->format))) {
                config.color.bits_per_channel = static_cast<uint32_t>(descriptor->comp[0].depth);
                config.chroma_420 = descriptor->log2_chroma_w == 1 && descriptor->log2_chroma_h == 1;
            }
            config.color.hdr = par->color_trc == AVCOL_TRC_SMPTE2084 || par->color_trc == AVCOL_TRC_ARIB_STD_B67;
            if (const auto* side = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data,
                                                           AV_PKT_DATA_CONTENT_LIGHT_LEVEL)) {
                if (side->size < sizeof(AVContentLightMetadata))
                    return false;
                const auto* light = reinterpret_cast<const AVContentLightMetadata*>(side->data);
                config.color.max_content_light_level = light->MaxCLL;
                config.color.max_frame_average_light_level = light->MaxFALL;
            }
            if (const auto* side = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data,
                                                           AV_PKT_DATA_MASTERING_DISPLAY_METADATA)) {
                if (side->size < sizeof(AVMasteringDisplayMetadata))
                    return false;
                const auto* mastering = reinterpret_cast<const AVMasteringDisplayMetadata*>(side->data);
                if (mastering->has_primaries && mastering->has_luminance) {
                    config.color.has_mastering_display = true;
                    config.color.mastering_display_primary_r_x =
                        static_cast<float>(av_q2d(mastering->display_primaries[0][0]));
                    config.color.mastering_display_primary_r_y =
                        static_cast<float>(av_q2d(mastering->display_primaries[0][1]));
                    config.color.mastering_display_primary_g_x =
                        static_cast<float>(av_q2d(mastering->display_primaries[1][0]));
                    config.color.mastering_display_primary_g_y =
                        static_cast<float>(av_q2d(mastering->display_primaries[1][1]));
                    config.color.mastering_display_primary_b_x =
                        static_cast<float>(av_q2d(mastering->display_primaries[2][0]));
                    config.color.mastering_display_primary_b_y =
                        static_cast<float>(av_q2d(mastering->display_primaries[2][1]));
                    config.color.mastering_display_white_point_x =
                        static_cast<float>(av_q2d(mastering->white_point[0]));
                    config.color.mastering_display_white_point_y =
                        static_cast<float>(av_q2d(mastering->white_point[1]));
                    config.color.mastering_display_max_luminance = static_cast<float>(av_q2d(mastering->max_luminance));
                    config.color.mastering_display_min_luminance = static_cast<float>(av_q2d(mastering->min_luminance));
                }
            }
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && config.audio_track_count < config.audio_tracks.size()) {
            if (audio_codec != AV_CODEC_ID_NONE &&
                (audio_codec != par->codec_id || config.audio_sample_rate != static_cast<uint32_t>(par->sample_rate) ||
                 config.audio_channels != static_cast<uint32_t>(par->ch_layout.nb_channels)))
                return false;
            audio_codec = par->codec_id;
            switch (audio_codec) {
            case AV_CODEC_ID_AAC:
                config.audio_codec = StreamAudioCodec::Aac;
                break;
            case AV_CODEC_ID_OPUS:
                config.audio_codec = StreamAudioCodec::Opus;
                break;
            case AV_CODEC_ID_FLAC:
                config.audio_codec = StreamAudioCodec::Flac;
                break;
            case AV_CODEC_ID_PCM_S16LE:
            case AV_CODEC_ID_PCM_S24LE:
            case AV_CODEC_ID_PCM_S32LE:
            case AV_CODEC_ID_PCM_F32LE:
                config.audio_codec = StreamAudioCodec::Pcm;
                break;
            default:
                return false;
            }
            config.audio_sample_rate = static_cast<uint32_t>(par->sample_rate);
            config.audio_channels = static_cast<uint32_t>(par->ch_layout.nb_channels);
            config.audio_bit_depth = static_cast<uint32_t>(par->bits_per_raw_sample > 0 ? par->bits_per_raw_sample
                                                                                        : par->bits_per_coded_sample);
            config.audio_float = audio_codec == AV_CODEC_ID_PCM_F32LE;
            if (audio_codec == AV_CODEC_ID_OPUS)
                config.opus_frame_samples = static_cast<uint32_t>((std::max)(par->frame_size, 0));
            auto& track = config.audio_tracks[config.audio_track_count];
            track_numbers[i] = 2 + config.audio_track_count++;
            if (par->extradata_size > 0) {
                if (audio_codec == AV_CODEC_ID_FLAC && par->extradata_size == 34)
                    track.codec_private = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
                track.codec_private.insert(track.codec_private.end(), par->extradata,
                                           par->extradata + par->extradata_size);
            }
            track.codec_delay_samples = static_cast<uint32_t>((std::max)(par->initial_padding, 0));
            if (const auto* title = av_dict_get(stream->metadata, "title", nullptr, 0))
                track.name = title->value;
        } else
            return false;
    }
    return video_found;
}

// Generic internal stream-copy remux. Both RemuxToProgressiveMp4 and
// RemuxToMkv delegate here. opts.format_name must not be nullptr.
// tr is optional: TrimRange{} (both kNoTimestamp) means no trim.
static RemuxResult RemuxStreamCopy(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                                   RemuxProgressCallback progress_cb, const RemuxOptions& opts, TrimRange tr,
                                   const RemuxIoFaults* faults) {
    const auto remux_start = std::chrono::steady_clock::now();
    double write_ms = 0.0, peak_write_ms = 0.0;
    int64_t output_bytes = 0;
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
    auto free_packet = [](AVPacket* packet) { av_packet_free(&packet); };
    std::vector<std::unique_ptr<AVPacket, decltype(free_packet)>> prefetched;
    size_t prefetch_index = 0;
    int prefetch_error = 0;
    bool exact_mkv_rewrite = false;
    if (std::string_view(opts.format_name) == "matroska") {
        // Keep the bounded prefix instead of seeking back. Crash-truncated files
        // can have readable clusters but no index supporting a backward seek.
        size_t prefix_bytes = 0;
        for (unsigned n = 0; n < 64 && prefix_bytes < 64 * 1024 * 1024; ++n) {
            std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
            if (!packet)
                return RemuxResult::Fail(AVERROR(ENOMEM), "av_packet_alloc failed");
            const int read_result = av_read_frame(in_ctx, packet.get());
            if (read_result < 0) {
                prefetch_error = read_result;
                break;
            }
            const int stream = packet->stream_index;
            const bool video = stream >= 0 && static_cast<unsigned>(stream) < in_ctx->nb_streams &&
                               in_ctx->streams[stream]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
            if (video)
                exact_mkv_rewrite = HasExactPacketTimestamps(packet.get());
            prefix_bytes += static_cast<size_t>(packet->size);
            prefetched.push_back(std::move(packet));
            if (video)
                break;
        }
    }
    std::unique_ptr<MatroskaStreamWriter> exact_writer;
    MatroskaStreamConfig rewrite_config;
    std::vector<uint64_t> rewrite_track_numbers;
    if (exact_mkv_rewrite) {
        if (!ConfigureExactTimestampRewrite(in_ctx, out_str, rewrite_config, rewrite_track_numbers))
            return RemuxResult::Fail(AVERROR(ENOSYS),
                                     "Unsupported track configuration for exact-timestamp MKV rewrite");
        if (faults && faults->fail_output_close)
            rewrite_config.fail_io = [](OutputIoOperation operation) { return operation == OutputIoOperation::Close; };
        exact_writer = std::make_unique<MatroskaStreamWriter>();
        if (!exact_writer->Open(rewrite_config))
            return RemuxResult::Fail(AVERROR(EIO), exact_writer->error());
    }

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
        if (exact_writer)
            out_st->time_base = in_st->time_base;

        // HEVC-in-MP4: request the 'hvc1' sample-entry FourCC. With codec_tag=0
        // libavformat's mov muxer defaults to 'hev1', which permits in-band
        // parameter sets but is refused by QuickTime / Apple devices and several
        // NLEs. 'hvc1' carries the parameter sets out-of-band in the hvcC box —
        // exactly how the transient MKV already stores them (hvcC codec-private),
        // so a plain stream-copy yields a conformant Apple-compatible file
        // MKV output ignores codec_tag (it maps tracks by
        // CodecID string), so this is gated to the MP4 muxer only.
        if (out_is_mp4 && out_st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            out_st->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            out_st->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
        }

        // Color description — the MP4 output must carry the source's colour
        // identity, not a hardcoded SDR one. avcodec_parameters_copy copies the
        // color_primaries / color_trc / color_space / color_range CICP fields
        // AND the coded_side_data array from the input AVCodecParameters. So a
        // source MKV written with KaxVideoColour tags — SDR BT.709 or
        // HDR10 BT.2020/PQ — round-trips verbatim: the mov muxer emits the colr
        // (nclx) box from the CICP fields and, for HDR sources that carry
        // KaxVideoColourMasterMeta, the mdcv (mastering-display) box from the
        // copied AV_PKT_DATA_MASTERING_DISPLAY_METADATA side data. Content-light-
        // level is only emitted when the source carries it: an HDR10 recording
        // measures MaxCLL/MaxFALL per frame and writes them into the source MKV,
        // so the clli box follows from the copied side data. A source without them
        // writes no clli box, which is correct -- an empty one would be a
        // conformance defect.
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
    if (!exact_writer && !(out_ctx->oformat->flags & AVFMT_NOFILE)) {
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

    if (!exact_writer) {
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
        } else {
            prefetched.clear();
            prefetch_error = 0;
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
    // true = past the start boundary. Only a video keyframe can lock it, so an
    // input with no video stream would never lock it at all: every packet would be
    // skipped, the trailer would be written over nothing, and an empty file would
    // be reported as a successful trim.
    bool trim_start_locked = !tr.HasStart() || video_stream_idx < 0;
    bool video_reorders = video_stream_idx >= 0 && in_ctx->streams[video_stream_idx]->codecpar->video_delay > 0;
    std::optional<uint64_t> rewrite_epoch_ns;
    int64_t video_end_us = AV_NOPTS_VALUE;
    std::vector<bool> audio_end_reached(in_ctx->nb_streams, true);
    for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
        if (in_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            audio_end_reached[i] = false;
    }

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

        int ret;
        if (prefetch_index < prefetched.size()) {
            av_packet_move_ref(pkt, prefetched[prefetch_index++].get());
            ret = 0;
        } else
            ret = prefetch_error < 0 ? prefetch_error : av_read_frame(in_ctx, pkt);
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
        std::optional<MatroskaPacketTimestamps> exact_timestamps;
        size_t additional_size = 0;
        const uint8_t* additional =
            av_packet_get_side_data(pkt, AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, &additional_size);
        if (si == video_stream_idx && additional && additional_size >= 8) {
            uint64_t type = 0;
            for (size_t i = 0; i < 8; ++i)
                type = (type << 8) | additional[i];
            if (type == kPacketTimestampMappingType) {
                exact_timestamps = DecodeMatroskaPacketTimestamps({additional + 8, additional_size - 8});
                if (!exact_timestamps)
                    return RemuxResult::Fail(AVERROR_INVALIDDATA, "Invalid ExoSnap packet timestamp metadata");
                if (std::string_view(opts.format_name) == "matroska" && !exact_writer)
                    return RemuxResult::Fail(AVERROR_INVALIDDATA, "Exact video timestamps exceed the MKV prefix limit");
                pkt->pts =
                    av_rescale_q(static_cast<int64_t>(exact_timestamps->pts_ns), {1, 1000000000}, in_st->time_base);
                pkt->dts = av_rescale_q(exact_timestamps->dts_ns, {1, 1000000000}, in_st->time_base);
            }
        }

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
        if (si == video_stream_idx && pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE && pkt->dts < pkt->pts)
            video_reorders = true;
        if (video_end_us == AV_NOPTS_VALUE && tr.HasEnd() && si == video_stream_idx && pkt->pts != AV_NOPTS_VALUE) {
            const int64_t pts_us = av_rescale_q(pkt->pts, in_st->time_base, {1, AV_TIME_BASE});
            // A reference picture can precede B-frames whose PTS is still below
            // the requested end. Keep the dependency-complete GOP until the next
            // random-access picture instead of cutting on its future PTS.
            if (pts_us >= tr.end_us && (!video_reorders || (pkt->flags & AV_PKT_FLAG_KEY))) {
                av_packet_unref(pkt);
                if (!video_reorders || std::all_of(audio_end_reached.begin(), audio_end_reached.end(),
                                                   [](bool reached) { return reached; }))
                    break;
                video_end_us = pts_us;
                continue;
            }
        }

        if (video_end_us != AV_NOPTS_VALUE) {
            // The next IDR can arrive at its decode time before audio reaches
            // the completed GOP's presentation end. Drain every audio track to
            // that boundary while keeping all later video out of the export.
            if (si == video_stream_idx || in_st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO || audio_end_reached[si]) {
                av_packet_unref(pkt);
                continue;
            }
            if (pkt->pts != AV_NOPTS_VALUE &&
                av_rescale_q(pkt->pts, in_st->time_base, {1, AV_TIME_BASE}) >= video_end_us) {
                audio_end_reached[si] = true;
                av_packet_unref(pkt);
                if (std::all_of(audio_end_reached.begin(), audio_end_reached.end(),
                                [](bool reached) { return reached; }))
                    break;
                continue;
            }
        }

        // Rescale timestamps from the input stream's time base to the output
        // stream's time base.
        av_packet_rescale_ts(pkt, in_st->time_base, out_st->time_base);
        if (exact_timestamps) {
            pkt->pts = av_rescale_q(static_cast<int64_t>(exact_timestamps->pts_ns), {1, 1000000000}, out_st->time_base);
            pkt->dts = av_rescale_q(exact_timestamps->dts_ns, {1, 1000000000}, out_st->time_base);
        }
        pkt->pos = -1; // invalidate byte position (invalid in the new file)

        // Capture PTS for progress before the write consumes the packet.
        const int64_t pkt_pts = pkt->pts;
        const AVRational out_tb = out_st->time_base;

        const auto write_start = std::chrono::steady_clock::now();
        if (exact_writer) {
            MuxPacket packet;
            packet.track_num = rewrite_track_numbers[static_cast<size_t>(si)];
            packet.is_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            packet.bytes.assign(pkt->data, pkt->data + pkt->size);
            if (packet.track_num == 1) {
                if (!exact_timestamps)
                    return RemuxResult::Fail(AVERROR_INVALIDDATA, "Missing exact video timestamps in MKV rewrite");
                if (!rewrite_epoch_ns)
                    rewrite_epoch_ns = tr.HasStart() ? exact_timestamps->pts_ns : 0;
                const auto epoch = static_cast<int64_t>(*rewrite_epoch_ns);
                if (exact_timestamps->pts_ns < *rewrite_epoch_ns ||
                    exact_timestamps->dts_ns < (std::numeric_limits<int64_t>::min)() + epoch + 1)
                    return RemuxResult::Fail(AVERROR_INVALIDDATA, "Video timestamp underflow in MKV rewrite");
                packet.pts_ns = exact_timestamps->pts_ns - *rewrite_epoch_ns;
                packet.dts_ns = exact_timestamps->dts_ns - epoch;
            } else {
                // Matroska demux subtracts CodecDelay. The writer expects block
                // timestamps before that subtraction and writes CodecDelay again.
                const auto delay =
                    rewrite_config.audio_tracks[static_cast<size_t>(packet.track_num - 2)].codec_delay_samples;
                const int64_t delay_ticks =
                    av_rescale_q(delay, {1, static_cast<int>(rewrite_config.audio_sample_rate)}, out_st->time_base);
                const int64_t pts_ns = av_rescale_q(pkt->pts + delay_ticks, out_st->time_base, {1, 1000000000});
                if (pts_ns < 0 || pkt->pts == AV_NOPTS_VALUE)
                    return RemuxResult::Fail(AVERROR_INVALIDDATA, "Invalid audio timestamp in MKV rewrite");
                const auto epoch = rewrite_epoch_ns.value_or(0);
                if (static_cast<uint64_t>(pts_ns) < epoch) {
                    av_packet_unref(pkt);
                    continue;
                }
                packet.pts_ns = static_cast<uint64_t>(pts_ns) - epoch;
            }
            ret = exact_writer->Push(std::move(packet)) ? 0 : AVERROR(EIO);
        } else
            ret = av_interleaved_write_frame(out_ctx, pkt);
        const double elapsed_write_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - write_start).count();
        write_ms += elapsed_write_ms;
        peak_write_ms = std::max(peak_write_ms, elapsed_write_ms);
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
        exact_writer.reset();
        // Close handles before deleting the partial file.
        if (out_guard.avio_opened && !(out_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&out_ctx->pb);
            out_guard.avio_opened = false;
        }
        avformat_free_context(out_ctx);
        out_guard.ctx = nullptr;

        // The caller owns this path -- every call site hands in a staging file and
        // publishes it itself -- so removing it here is a courtesy, not the
        // transaction. It is still reported: a partial file left on disk because it
        // could not be deleted is something the caller's own cleanup has to see.
        std::error_code ec;
        std::filesystem::remove(output_path, ec);
        if (ec) {
            logging::LogField fields[] = {{"output", out_str}, {"error", ec.message()}};
            logging::log(logging::LogLevel::Warn, kLogComponent,
                         "Remux cancelled, but the partial output could not be removed",
                         std::span<const logging::LogField>(fields, std::size(fields)));
        } else {
            LogInfo("Remux cancelled by caller — partial output removed");
        }
        return RemuxResult::Fail(AVERROR(ECANCELED), "Remux cancelled by caller");
    }

    if (exact_writer) {
        if (!exact_writer->Finalize())
            return RemuxResult::Fail(AVERROR(EIO), exact_writer->error());
        output_bytes = static_cast<int64_t>(exact_writer->bytes_written());
    } else {
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

    if (out_guard.avio_opened && !(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_flush(out_ctx->pb);
        const int stream_error = out_ctx->pb->error;
        output_bytes = out_ctx->pb->bytes_written;
        int close_error = avio_closep(&out_ctx->pb);
        if (faults && faults->fail_output_close && close_error >= 0)
            close_error = AVERROR(EIO);
        out_guard.avio_opened = false;
        const int error = stream_error < 0 ? stream_error : close_error;
        if (error < 0)
            return RemuxResult::Fail(error, std::string("Output flush/close failed: ") + av_err2str(error));
    }

    // Signal 100% progress.
    if (progress_cb)
        progress_cb(1.0f);

    // error_code overload: this is the only throwing call on the success path of a
    // function the caller joins synchronously on the GUI thread, and an output that
    // briefly becomes unreadable (a share, a scanner) must not turn a completed
    // remux into an uncaught exception. The size is only ever logged.
    std::error_code size_ec;
    const auto out_size = std::filesystem::file_size(output_path, size_ec);
    {
        logging::LogField fields[] = {
            {"output_bytes", size_ec ? std::string("unknown") : std::to_string(out_size)},
            {"logical_read_bytes", in_ctx->pb ? std::to_string(in_ctx->pb->bytes_read) : "unavailable"},
            {"logical_write_bytes", std::to_string(output_bytes)},
            {"packet_write_total_ms", std::to_string(write_ms)},
            {"packet_write_peak_ms", std::to_string(peak_write_ms)},
            {"remux_ms",
             std::to_string(
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - remux_start).count())}};
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
                                  RemuxProgressCallback progress_cb, TrimRange tr, const RemuxIoFaults* faults) {
    // movflags=+faststart: two-pass write that moves moov before mdat.
    static const char* const kMp4Opts[] = {"movflags", "+faststart", nullptr};
    RemuxOptions opts;
    opts.format_name = "mp4";
    opts.extra_opts = kMp4Opts;
    return RemuxStreamCopy(input_path, output_path, std::move(progress_cb), opts, tr, faults);
}

RemuxResult RemuxToMkv(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                       RemuxProgressCallback progress_cb) {
    return RemuxToMkv(input_path, output_path, std::move(progress_cb), TrimRange{});
}

RemuxResult RemuxToMkv(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                       RemuxProgressCallback progress_cb, TrimRange tr, const RemuxIoFaults* faults) {
    // The matroska muxer writes Cues, SeekHead, and Duration at trailer time.
    // No extra options needed: libavformat defaults are correct for recovery MKV.
    RemuxOptions opts;
    opts.format_name = "matroska";
    opts.extra_opts = nullptr;
    return RemuxStreamCopy(input_path, output_path, std::move(progress_cb), opts, tr, faults);
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

} // namespace exosnap::engine
