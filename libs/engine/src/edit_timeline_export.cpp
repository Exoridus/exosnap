#include "exosnap/engine/edit_timeline_export.h"
#include <capability/translatable.h>

#include "exosnap/engine/mp4_remuxer.h"
#include "matroska_packet_timestamps.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <system_error>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/defs.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

namespace exosnap::engine {
namespace {

constexpr AVRational kMicroseconds{1, AV_TIME_BASE};

std::string Utf8Path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string AvError(int code) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    return message;
}

struct Input {
    AVFormatContext* context = nullptr;
    ~Input() {
        avformat_close_input(&context);
    }
    Input() = default;
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    int Open(const std::filesystem::path& source) {
        int result = avformat_open_input(&context, Utf8Path(source).c_str(), nullptr, nullptr);
        return result < 0 ? result : avformat_find_stream_info(context, nullptr);
    }
};

struct Output {
    AVFormatContext* context = nullptr;
    ~Output() {
        if (context) {
            if (context->pb)
                avio_closep(&context->pb);
            avformat_free_context(context);
        }
    }
};

struct StagingFile {
    std::filesystem::path path;
    bool complete = false;
    ~StagingFile() {
        if (!complete) {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    }
};

void ResolveDecodedColor(AVFormatContext* input, AVStream* stream) {
    auto* parameters = stream->codecpar;
    if (parameters->color_primaries != AVCOL_PRI_UNSPECIFIED && parameters->color_trc != AVCOL_TRC_UNSPECIFIED)
        return;
    const AVCodec* decoder = avcodec_find_decoder(parameters->codec_id);
    if (!decoder)
        return;
    AVCodecContext* context = avcodec_alloc_context3(decoder);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (context && packet && frame && avcodec_parameters_to_context(context, parameters) >= 0 &&
        avcodec_open2(context, decoder, nullptr) >= 0) {
        // Container tags may omit values that the bitstream explicitly carries.
        // A bounded first-frame probe resolves those facts without guessing.
        for (int count = 0; count < 512 && av_read_frame(input, packet) >= 0; ++count) {
            if (packet->stream_index == stream->index && avcodec_send_packet(context, packet) >= 0 &&
                avcodec_receive_frame(context, frame) >= 0) {
                if (parameters->color_primaries == AVCOL_PRI_UNSPECIFIED)
                    parameters->color_primaries = frame->color_primaries;
                if (parameters->color_trc == AVCOL_TRC_UNSPECIFIED)
                    parameters->color_trc = frame->color_trc;
                break;
            }
            av_packet_unref(packet);
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
}

std::vector<TimelineExportClip> Coalesce(const std::vector<TimelineExportClip>& clips) {
    std::vector<TimelineExportClip> result;
    for (const auto& clip : clips) {
        if (!result.empty() && result.back().source == clip.source &&
            result.back().source_out_us == clip.source_in_us &&
            result.back().timeline_start_us + (result.back().source_out_us - result.back().source_in_us) ==
                clip.timeline_start_us) {
            result.back().source_out_us = clip.source_out_us;
        } else {
            result.push_back(clip);
        }
    }
    return result;
}

bool SameRatio(AVRational a, AVRational b) {
    return (a.num == 0 && b.num == 0) || (a.den != 0 && b.den != 0 && av_cmp_q(a, b) == 0);
}

bool Compatible(const AVStream* a, const AVStream* b) {
    const auto* x = a->codecpar;
    const auto* y = b->codecpar;
    if (x->codec_type != y->codec_type || x->codec_id != y->codec_id || x->format != y->format ||
        x->width != y->width || x->height != y->height || x->sample_rate != y->sample_rate ||
        x->profile != y->profile || x->level != y->level || x->video_delay != y->video_delay ||
        x->color_range != y->color_range || x->color_primaries != y->color_primaries || x->color_trc != y->color_trc ||
        x->color_space != y->color_space || x->initial_padding != y->initial_padding ||
        x->trailing_padding != y->trailing_padding || x->extradata_size != y->extradata_size ||
        x->bits_per_coded_sample != y->bits_per_coded_sample || x->bits_per_raw_sample != y->bits_per_raw_sample ||
        x->block_align != y->block_align || x->frame_size != y->frame_size ||
        x->nb_coded_side_data != y->nb_coded_side_data || !SameRatio(a->avg_frame_rate, b->avg_frame_rate) ||
        av_channel_layout_compare(&x->ch_layout, &y->ch_layout) != 0 ||
        !SameRatio(x->sample_aspect_ratio, y->sample_aspect_ratio))
        return false;
    for (int i = 0; i < x->nb_coded_side_data; ++i) {
        const auto& left = x->coded_side_data[i];
        const auto& right = y->coded_side_data[i];
        if (left.type != right.type || left.size != right.size || std::memcmp(left.data, right.data, left.size) != 0)
            return false;
    }
    return x->extradata_size == 0 ||
           std::memcmp(x->extradata, y->extradata, static_cast<size_t>(x->extradata_size)) == 0;
}

TimelineExportAssessment Invalid(std::string reason) {
    return {TimelineExportEligibility::Invalid, std::move(reason)};
}

TimelineExportAssessment Render(std::string reason) {
    return {TimelineExportEligibility::RenderRequired, std::move(reason)};
}

TimelineExportAssessment ValidateRecipe(const std::vector<TimelineExportClip>& clips) {
    if (clips.empty())
        return Invalid("The timeline is empty.");
    int64_t end = 0;
    for (const auto& clip : clips) {
        if (clip.source.empty() || clip.source_in_us < 0 || clip.source_out_us <= clip.source_in_us ||
            clip.timeline_start_us < 0 ||
            clip.timeline_start_us > (std::numeric_limits<int64_t>::max)() - (clip.source_out_us - clip.source_in_us))
            return Invalid("The timeline contains an invalid clip range.");
        if (clip.timeline_start_us != end)
            return Render("Gaps and overlapping clips require render export.");
        end = clip.timeline_start_us + clip.source_out_us - clip.source_in_us;
    }
    return {TimelineExportEligibility::StreamCopy, {}};
}

} // namespace

EditMediaMetadata ProbeEditMedia(const std::filesystem::path& source) {
    EditMediaMetadata result;
    Input input;
    const int status = input.Open(source);
    if (status < 0) {
        result.error = AvError(status);
        return result;
    }
    result.duration_us = std::max<int64_t>(0, input.context->duration);
    for (unsigned i = 0; i < input.context->nb_streams; ++i) {
        auto* stream = input.context->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && result.width == 0) {
            ResolveDecodedColor(input.context, stream);
            result.width = stream->codecpar->width;
            result.height = stream->codecpar->height;
            const auto fps = stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
            result.fps = fps.den > 0 ? av_q2d(fps) : 0;
            result.fps_num = fps.num;
            result.fps_den = fps.den;
            const auto& color = *stream->codecpar;
            result.render_color_supported =
                color.format == AV_PIX_FMT_YUV420P && color.color_primaries == AVCOL_PRI_BT709 &&
                color.color_trc == AVCOL_TRC_BT709 && color.color_space == AVCOL_SPC_BT709 &&
                (color.color_range == AVCOL_RANGE_MPEG || color.color_range == AVCOL_RANGE_JPEG);
            if (!result.render_color_supported)
                result.render_color_reason =
                    std::string(EXOSNAP_TRANSLATABLE(
                        "EditRender", "Render export requires explicitly tagged 8-bit BT.709 4:2:0 SDR with known "
                                      "range. HDR and unsupported color paths remain lossless-only.")) +
                    " (pixel format=" + std::to_string(color.format) +
                    ", primaries=" + std::to_string(color.color_primaries) +
                    ", transfer=" + std::to_string(color.color_trc) + ", matrix=" + std::to_string(color.color_space) +
                    ", range=" + std::to_string(color.color_range) + ")";
        }
        result.has_audio |= stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
    }
    if (result.duration_us <= 0 || (result.width == 0 && !result.has_audio))
        result.error = "Media has no supported timed video or audio stream.";
    return result;
}

TimelineExportAssessment AssessTimelineExport(const std::vector<TimelineExportClip>& clips, bool to_mp4) {
    const auto validity = ValidateRecipe(clips);
    if (validity.eligibility != TimelineExportEligibility::StreamCopy)
        return validity;
    const auto recipe = Coalesce(clips);
    const auto* format = av_guess_format(to_mp4 ? "mp4" : "matroska", nullptr, nullptr);
    if (!format)
        return Invalid("The requested container is unavailable.");
    std::unique_ptr<Input> reference;
    for (const auto& clip : recipe) {
        auto input = std::make_unique<Input>();
        const int status = input->Open(clip.source);
        if (status < 0)
            return Invalid("Cannot open source media: " + AvError(status));
        const auto duration = input->context->duration;
        if (duration <= 0 || clip.source_out_us - duration > 1000)
            return Invalid("A clip extends beyond the available source duration.");
        if (recipe.size() > 1 && (clip.source_in_us != 0 || std::abs(clip.source_out_us - duration) > 1000))
            return Render("Multiple trimmed sources require render export. Complete compatible sources can be copied.");
        if (input->context->nb_streams == 0)
            return Invalid("The source has no streams.");
        if (reference && reference->context->nb_streams != input->context->nb_streams)
            return Render("Sources with different stream layouts require render export.");
        for (unsigned i = 0; i < input->context->nb_streams; ++i) {
            const auto* stream = input->context->streams[i];
            const auto* codec = stream->codecpar;
            if (codec->codec_type != AVMEDIA_TYPE_VIDEO && codec->codec_type != AVMEDIA_TYPE_AUDIO)
                return Render("Only video and audio streams can be copied from a timeline.");
            if (avformat_query_codec(format, codec->codec_id, FF_COMPLIANCE_NORMAL) <= 0)
                return Render("The chosen container cannot copy a source codec.");
            if (reference && !Compatible(reference->context->streams[i], stream))
                return Render("Sources with different codecs or media parameters require render export.");
            if (recipe.size() > 1 && codec->codec_type == AVMEDIA_TYPE_AUDIO && codec->initial_padding != 0)
                return Render("Concatenating independently primed audio streams requires render export.");
        }
        if (recipe.size() > 1) {
            auto free_packet = [](AVPacket* packet) { av_packet_free(&packet); };
            std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
            if (!packet)
                return Invalid("Cannot allocate an input packet.");
            std::vector<bool> found(input->context->nb_streams, false);
            int status_read = 0;
            while ((status_read = av_read_frame(input->context, packet.get())) >= 0) {
                const auto index = static_cast<unsigned>(packet->stream_index);
                if (!found[index] && input->context->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                    !(packet->flags & AV_PKT_FLAG_KEY))
                    return Render("Every source must begin at a video keyframe for lossless concatenation.");
                found[index] = true;
                av_packet_unref(packet.get());
                if (std::all_of(found.begin(), found.end(), [](bool value) { return value; }))
                    break;
            }
            if (status_read < 0)
                return Invalid("A source is missing packets for one or more streams.");
        }
        if (!reference)
            reference = std::move(input);
    }
    return {TimelineExportEligibility::StreamCopy, recipe.size() == 1
                                                       ? "Stream copy uses keyframe-accurate trim boundaries."
                                                       : "Compatible complete sources."};
}

RemuxResult ExportTimelineStreamCopy(const std::vector<TimelineExportClip>& clips, const std::filesystem::path& output,
                                     bool to_mp4, RemuxProgressCallback progress_cb) {
    std::error_code path_error;
    if (output.empty() || std::filesystem::exists(output, path_error) || path_error)
        return RemuxResult::Fail(AVERROR(EEXIST), "Timeline export requires a new staging output path.");
    const auto assessment = AssessTimelineExport(clips, to_mp4);
    if (assessment.eligibility != TimelineExportEligibility::StreamCopy)
        return RemuxResult::Fail(AVERROR(EINVAL), assessment.reason);
    if (!progress_cb)
        progress_cb = RemuxNoopCallback();
    if (!progress_cb(0))
        return RemuxResult::Fail(AVERROR_EXIT, "Export cancelled.");
    const auto recipe = Coalesce(clips);
    StagingFile staging{output};
    if (recipe.size() == 1) {
        const auto& clip = recipe.front();
        TrimRange trim{clip.source_in_us, clip.source_out_us};
        const auto result = to_mp4 ? RemuxToProgressiveMp4(clip.source, output, progress_cb, trim)
                                   : RemuxToMkv(clip.source, output, progress_cb, trim);
        staging.complete = result.success;
        return result;
    }
    Output destination;
    int status = avformat_alloc_output_context2(&destination.context, nullptr, to_mp4 ? "mp4" : "matroska",
                                                Utf8Path(output).c_str());
    if (status < 0 || !destination.context)
        return RemuxResult::Fail(status, "Cannot allocate export container.");
    destination.context->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    Input first;
    if ((status = first.Open(recipe.front().source)) < 0)
        return RemuxResult::Fail(status, AvError(status));
    for (unsigned i = 0; i < first.context->nb_streams; ++i) {
        auto* stream = avformat_new_stream(destination.context, nullptr);
        if (!stream)
            return RemuxResult::Fail(AVERROR(ENOMEM), "Cannot allocate output stream.");
        if ((status = avcodec_parameters_copy(stream->codecpar, first.context->streams[i]->codecpar)) < 0)
            return RemuxResult::Fail(status, AvError(status));
        stream->codecpar->codec_tag = 0;
        stream->time_base = first.context->streams[i]->time_base;
        stream->avg_frame_rate = first.context->streams[i]->avg_frame_rate;
        stream->disposition = first.context->streams[i]->disposition;
        av_dict_copy(&stream->metadata, first.context->streams[i]->metadata, 0);
    }
    if ((status = avio_open(&destination.context->pb, Utf8Path(output).c_str(), AVIO_FLAG_WRITE)) < 0)
        return RemuxResult::Fail(status, AvError(status));
    AVDictionary* options = nullptr;
    if (to_mp4)
        av_dict_set(&options, "movflags", "+faststart", 0);
    status = avformat_write_header(destination.context, &options);
    av_dict_free(&options);
    if (status < 0)
        return RemuxResult::Fail(status, AvError(status));
    auto free_packet = [](AVPacket* packet) { av_packet_free(&packet); };
    std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
    if (!packet)
        return RemuxResult::Fail(AVERROR(ENOMEM), "Cannot allocate export packet.");
    std::vector<int64_t> last_dts(first.context->nb_streams, AV_NOPTS_VALUE);
    const int64_t total = recipe.back().timeline_start_us + recipe.back().source_out_us;
    float progress = 0;
    for (const auto& clip : recipe) {
        Input source;
        if ((status = source.Open(clip.source)) < 0)
            return RemuxResult::Fail(status, AvError(status));
        const int64_t origin = source.context->start_time == AV_NOPTS_VALUE ? 0 : source.context->start_time;
        while ((status = av_read_frame(source.context, packet.get())) >= 0) {
            const auto index = static_cast<unsigned>(packet->stream_index);
            auto* input_stream = source.context->streams[index];
            auto* output_stream = destination.context->streams[index];
            size_t additional_size = 0;
            const auto* additional =
                av_packet_get_side_data(packet.get(), AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, &additional_size);
            if (additional && additional_size >= 8) {
                uint64_t type = 0;
                for (size_t i = 0; i < 8; ++i)
                    type = (type << 8) | additional[i];
                if (type == kPacketTimestampMappingType) {
                    const auto exact = DecodeMatroskaPacketTimestamps({additional + 8, additional_size - 8});
                    if (!exact)
                        return RemuxResult::Fail(AVERROR_INVALIDDATA, "Invalid ExoSnap timestamp metadata.");
                    packet->pts =
                        av_rescale_q(static_cast<int64_t>(exact->pts_ns), {1, 1000000000}, input_stream->time_base);
                    packet->dts = av_rescale_q(exact->dts_ns, {1, 1000000000}, input_stream->time_base);
                    // These source timestamps must not override the new timeline timestamps on a later remux.
                    av_packet_side_data_remove(packet->side_data, &packet->side_data_elems,
                                               AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL);
                }
            }
            if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE)
                return RemuxResult::Fail(AVERROR_INVALIDDATA, "A source packet has no usable timestamps.");
            av_packet_rescale_ts(packet.get(), input_stream->time_base, output_stream->time_base);
            const auto offset = av_rescale_q(clip.timeline_start_us - origin, kMicroseconds, output_stream->time_base);
            packet->pts += offset;
            packet->dts += offset;
            if (last_dts[index] != AV_NOPTS_VALUE && packet->dts <= last_dts[index])
                return RemuxResult::Fail(AVERROR_INVALIDDATA,
                                         "Source boundary has overlapping decode timestamps; render required.");
            last_dts[index] = packet->dts;
            packet->pos = -1;
            const auto position = av_rescale_q(packet->pts, output_stream->time_base, kMicroseconds);
            progress =
                std::max(progress, std::clamp(static_cast<float>(position) / static_cast<float>(total), 0.0f, 0.99f));
            if ((status = av_interleaved_write_frame(destination.context, packet.get())) < 0)
                return RemuxResult::Fail(status, AvError(status));
            if (!progress_cb(progress))
                return RemuxResult::Fail(AVERROR_EXIT, "Export cancelled.");
        }
        if (status != AVERROR_EOF)
            return RemuxResult::Fail(status, AvError(status));
    }
    if ((status = av_write_trailer(destination.context)) < 0)
        return RemuxResult::Fail(status, AvError(status));
    if ((status = avio_closep(&destination.context->pb)) < 0)
        return RemuxResult::Fail(status, AvError(status));
    staging.complete = true;
    progress_cb(1);
    return RemuxResult::Ok();
}

} // namespace exosnap::engine
