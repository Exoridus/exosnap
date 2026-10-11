#include "exosnap/engine/edit_timeline_export.h"
#include <capability/translatable.h>
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/background_io.h>

#include "audio_media_manifest_adapter.h"
#include "budgeted_media_avio.h"
#include "exosnap/engine/mp4_remuxer.h"
#include "matroska_packet_timestamps.h"
#include "matroska_stream_writer.h"
#include "media_identity_sidecar.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <system_error>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
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
#include <libavutil/macros.h>
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
    std::shared_ptr<void> media_read_lease;
    AudioMetadataReadResult metadata;
    BackgroundIoAdmission metered_admission;
    std::unique_ptr<BudgetedMediaAvio> budgeted_input;
    bool admission_denied = false;
    AVFormatContext* context = nullptr;
    ~Input() {
        avformat_close_input(&context);
    }
    Input() = default;
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    int Open(const std::filesystem::path& source, const std::function<bool()>& continue_io = {},
             const BackgroundIoAdmission& before_io = {}, const std::atomic<bool>* cancelled = nullptr) {
        auto lease = AcquireMediaReadLease(source);
        if (!lease.succeeded())
            return AVERROR(EACCES);
        media_read_lease = std::move(*lease.value());
        if (before_io || cancelled) {
            metered_admission = [&, cancelled](uint64_t bytes, const std::atomic<bool>*) {
                if ((!cancelled || !cancelled->load(std::memory_order_relaxed)) &&
                    (!before_io || before_io(bytes, cancelled)))
                    return true;
                admission_denied = true;
                return false;
            };
            budgeted_input = BudgetedMediaAvio::Open(source, false, &metered_admission, cancelled, &admission_denied);
            if (!budgeted_input)
                return AVERROR(EIO);
            context = avformat_alloc_context();
            if (!context)
                return AVERROR(ENOMEM);
            context->pb = budgeted_input->context();
            context->flags |= AVFMT_FLAG_CUSTOM_IO;
        }
        int result =
            avformat_open_input(&context, budgeted_input ? nullptr : Utf8Path(source).c_str(), nullptr, nullptr);
        if (result < 0 || (result = avformat_find_stream_info(context, nullptr)) < 0)
            return result;
        if (admission_denied)
            return AVERROR_EXIT;
        metadata = ReadAudioMediaMetadata(source, context, continue_io, metered_admission, cancelled);
        if (admission_denied || (cancelled && cancelled->load(std::memory_order_relaxed)))
            return AVERROR_EXIT;
        return result;
    }
};

bool SemanticStreamMap(const Input& source, const Input& reference, std::vector<unsigned>& mapping) {
    if (source.context->nb_streams != reference.context->nb_streams)
        return false;
    mapping.resize(source.context->nb_streams);
    const auto& source_binding = source.metadata.unverified_asset_binding;
    const auto& reference_binding = reference.metadata.unverified_asset_binding;
    if (source_binding && reference_binding && source_binding->file_identity == reference_binding->file_identity &&
        source_binding->file_size == reference_binding->file_size &&
        source_binding->sha256 == reference_binding->sha256) {
        // Ordinals are unambiguous only within the same leased immutable asset.
        for (unsigned index = 0; index < source.context->nb_streams; ++index) {
            if (source.context->streams[index]->codecpar->codec_type !=
                reference.context->streams[index]->codecpar->codec_type)
                return false;
            mapping[index] = index;
        }
        return true;
    }
    std::vector<unsigned> reference_audio;
    std::vector<unsigned> reference_video;
    for (unsigned i = 0; i < reference.context->nb_streams; ++i) {
        const auto type = reference.context->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_AUDIO)
            reference_audio.push_back(i);
        else if (type == AVMEDIA_TYPE_VIDEO)
            reference_video.push_back(i);
        else
            return false;
    }
    if (!reference_audio.empty() && (source.metadata.status != AudioMetadataStatus::Verified ||
                                     reference.metadata.status != AudioMetadataStatus::Verified ||
                                     !source.metadata.manifest || !reference.metadata.manifest))
        return false;
    size_t audio = 0;
    size_t video = 0;
    std::vector<bool> used(mapping.size(), false);
    for (unsigned i = 0; i < source.context->nb_streams; ++i) {
        const auto type = source.context->streams[i]->codecpar->codec_type;
        unsigned destination = 0;
        if (type == AVMEDIA_TYPE_AUDIO) {
            if (!source.metadata.manifest || !reference.metadata.manifest)
                return false;
            const auto& source_tracks = source.metadata.manifest->tracks;
            const auto found = std::find_if(source_tracks.begin(), source_tracks.end(), [audio](const auto& track) {
                return track.container.stream_index.value == audio;
            });
            if (found == source_tracks.end())
                return false;
            const auto& reference_tracks = reference.metadata.manifest->tracks;
            const auto target = std::find_if(reference_tracks.begin(), reference_tracks.end(), [&](const auto& track) {
                return track.container.track_id == found->container.track_id;
            });
            if (target == reference_tracks.end() || target->container.stream_index.value >= reference_audio.size())
                return false;
            auto normalized = *source.metadata.manifest;
            for (auto& track : normalized.tracks) {
                const auto identity =
                    std::find_if(reference_tracks.begin(), reference_tracks.end(), [&](const auto& value) {
                        return value.container.track_id == track.container.track_id;
                    });
                if (identity == reference_tracks.end())
                    return false;
                track.container.stream_index = identity->container.stream_index;
                track.container.track_uid = identity->container.track_uid;
            }
            // Compare the frozen processing, not a codec match or a positional label.
            const auto original =
                SerializeAudioTrackProvenance(*reference.metadata.manifest, target->container.track_id);
            const auto incoming = SerializeAudioTrackProvenance(normalized, found->container.track_id);
            if (!original.succeeded() || !incoming.succeeded() || *original.value() != *incoming.value())
                return false;
            destination = reference_audio[target->container.stream_index.value];
            ++audio;
        } else if (type == AVMEDIA_TYPE_VIDEO && video < reference_video.size()) {
            destination = reference_video[video++];
        } else
            return false;
        if (used[destination])
            return false;
        used[destination] = true;
        mapping[i] = destination;
    }
    return std::all_of(used.begin(), used.end(), [](bool value) { return value; });
}

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

EditMediaMetadata ProbeEditMedia(const std::filesystem::path& source, const std::function<bool()>& continue_io,
                                 const BackgroundIoAdmission& before_io, const std::atomic<bool>* cancelled) {
    EditMediaMetadata result;
    Input input;
    const int status = input.Open(source, continue_io, before_io, cancelled);
    if (status < 0) {
        result.error = AvError(status);
        return result;
    }
    result.audio_metadata = input.metadata;
    result.duration_us = std::max<int64_t>(0, input.context->duration);
    for (unsigned i = 0; i < input.context->nb_streams; ++i) {
        auto* stream = input.context->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && result.width == 0) {
            ResolveDecodedColor(input.context, stream);
            result.video_codec = avcodec_get_name(stream->codecpar->codec_id);
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
    if (result.error.empty()) {
        const auto identity = HashFinalMedia(source, continue_io, input.metered_admission, cancelled);
        if (!identity.succeeded())
            result.error = identity.error()->context;
        else {
            result.file_identity = identity.value()->file_identity;
            result.file_size = identity.value()->file_size;
            result.content_sha256 = identity.value()->sha256;
            result.content_identity_valid = true;
        }
    }
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
        if (reference) {
            for (const auto type : {AVMEDIA_TYPE_AUDIO, AVMEDIA_TYPE_VIDEO}) {
                const auto count = [type](const AVFormatContext* context) {
                    unsigned total = 0;
                    for (unsigned index = 0; index < context->nb_streams; ++index)
                        total += context->streams[index]->codecpar->codec_type == type;
                    return total;
                };
                if (count(reference->context) != count(input->context))
                    return Render("Sources with different stream layouts require render export.");
            }
        }
        std::vector<unsigned> stream_mapping;
        if (reference && !SemanticStreamMap(*input, *reference, stream_mapping))
            return Invalid("Audio track identity or processing differs. Explicit semantic mapping is required.");
        for (unsigned i = 0; i < input->context->nb_streams; ++i) {
            const auto* stream = input->context->streams[i];
            const auto* codec = stream->codecpar;
            if (codec->codec_type != AVMEDIA_TYPE_VIDEO && codec->codec_type != AVMEDIA_TYPE_AUDIO)
                return Render("Only video and audio streams can be copied from a timeline.");
            if (avformat_query_codec(format, codec->codec_id, FF_COMPLIANCE_NORMAL) <= 0)
                return Render("The chosen container cannot copy a source codec.");
            if (reference && !Compatible(reference->context->streams[stream_mapping[i]], stream))
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
    MediaIdentityReferenceCopyGuard reference_copies;
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
        if (to_mp4 && stream->codecpar->codec_id == AV_CODEC_ID_HEVC)
            stream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
        stream->time_base = first.context->streams[i]->time_base;
        stream->avg_frame_rate = first.context->streams[i]->avg_frame_rate;
        stream->disposition = first.context->streams[i]->disposition;
        av_dict_copy(&stream->metadata, first.context->streams[i]->metadata, 0);
    }
    std::optional<AudioManifest1> output_manifest;
    MediaIdentitySidecarReferences output_references;
    if (first.metadata.manifest) {
        output_manifest = DeriveAudioManifest(*first.metadata.manifest);
        output_manifest->derived_from_media_ids.clear();
        for (const auto& clip : recipe) {
            Input source;
            if ((status = source.Open(clip.source)) < 0)
                return RemuxResult::Fail(status, AvError(status));
            if (!source.metadata.manifest)
                return RemuxResult::Fail(AVERROR(EINVAL), "Audio metadata changed before export");
            const auto id = source.metadata.manifest->media_id;
            const auto copied =
                CopyMediaIdentityReferenceFiles(clip.source, output, source.metadata.references, &reference_copies);
            if (!copied.succeeded())
                return RemuxResult::Fail(AVERROR(EIO), copied.error()->context);
            for (const auto& name : source.metadata.references.marker_refs)
                if (std::find(output_references.marker_refs.begin(), output_references.marker_refs.end(), name) ==
                    output_references.marker_refs.end())
                    output_references.marker_refs.push_back(name);
            if (source.metadata.references.source_ranges_json != "[]") {
                if (output_references.source_ranges_json != "[]" &&
                    output_references.source_ranges_json != source.metadata.references.source_ranges_json)
                    return RemuxResult::Fail(AVERROR(EINVAL),
                                             "Conflicting source-range associations require an explicit export plan");
                output_references.source_ranges_json = source.metadata.references.source_ranges_json;
            }
            if (std::find(output_manifest->derived_from_media_ids.begin(),
                          output_manifest->derived_from_media_ids.end(),
                          id) == output_manifest->derived_from_media_ids.end())
                output_manifest->derived_from_media_ids.push_back(id);
        }
        if (!to_mp4)
            for (auto& track : output_manifest->tracks)
                track.container.track_uid = 2 + track.container.stream_index.value;
        if (!WriteAudioManifestMetadata(destination.context, *output_manifest).succeeded())
            return RemuxResult::Fail(AVERROR(EINVAL), "Cannot write concatenated audio metadata");
    }
    std::unique_ptr<MatroskaStreamWriter> writer;
    std::vector<uint64_t> track_numbers;
    if (!to_mp4 && output_manifest) {
        MatroskaStreamConfig config;
        if (!ConfigureMatroskaStreamCopy(first.context, Utf8Path(output), config, track_numbers))
            return RemuxResult::Fail(AVERROR(ENOSYS),
                                     "Unsupported track configuration for semantic MKV concatenation.");
        config.audio_manifest = *output_manifest;
        for (const auto& track : output_manifest->tracks)
            config.audio_tracks[track.container.stream_index.value].name = track.container.name;
        writer = std::make_unique<MatroskaStreamWriter>();
        if (!writer->Open(config))
            return RemuxResult::Fail(AVERROR(EIO), writer->error());
    } else if ((status = avio_open(&destination.context->pb, Utf8Path(output).c_str(), AVIO_FLAG_WRITE)) < 0)
        return RemuxResult::Fail(status, AvError(status));
    AVDictionary* options = nullptr;
    if (to_mp4)
        av_dict_set(&options, "movflags", "+faststart", 0);
    status = writer ? 0 : avformat_write_header(destination.context, &options);
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
        std::vector<unsigned> mapping;
        if (!SemanticStreamMap(source, first, mapping))
            return RemuxResult::Fail(AVERROR(EINVAL), "Audio track identity changed before export");
        const int64_t origin = source.context->start_time == AV_NOPTS_VALUE ? 0 : source.context->start_time;
        while ((status = av_read_frame(source.context, packet.get())) >= 0) {
            const auto index = static_cast<unsigned>(packet->stream_index);
            auto* input_stream = source.context->streams[index];
            const auto destination_index = mapping[index];
            auto* output_stream = destination.context->streams[destination_index];
            std::optional<MatroskaPacketTimestamps> exact_timestamps;
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
                    exact_timestamps = exact;
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
            int64_t pts_ns = exact_timestamps ? static_cast<int64_t>(exact_timestamps->pts_ns)
                                              : av_rescale_q(packet->pts, input_stream->time_base, {1, 1000000000});
            int64_t dts_ns = exact_timestamps ? exact_timestamps->dts_ns
                                              : av_rescale_q(packet->dts, input_stream->time_base, {1, 1000000000});
            av_packet_rescale_ts(packet.get(), input_stream->time_base, output_stream->time_base);
            const auto offset = av_rescale_q(clip.timeline_start_us - origin, kMicroseconds, output_stream->time_base);
            packet->pts += offset;
            packet->dts += offset;
            if (last_dts[destination_index] != AV_NOPTS_VALUE && packet->dts <= last_dts[destination_index])
                return RemuxResult::Fail(AVERROR_INVALIDDATA,
                                         "Source boundary has overlapping decode timestamps; render required.");
            last_dts[destination_index] = packet->dts;
            packet->stream_index = static_cast<int>(destination_index);
            packet->pos = -1;
            const auto position = av_rescale_q(packet->pts, output_stream->time_base, kMicroseconds);
            progress =
                std::max(progress, std::clamp(static_cast<float>(position) / static_cast<float>(total), 0.0f, 0.99f));
            if (writer) {
                const auto offset_ns = av_rescale_q(clip.timeline_start_us - origin, kMicroseconds, {1, 1000000000});
                const auto shifted = [offset_ns](int64_t value) -> std::optional<int64_t> {
                    if ((offset_ns > 0 && value > (std::numeric_limits<int64_t>::max)() - offset_ns) ||
                        (offset_ns < 0 && value < (std::numeric_limits<int64_t>::min)() - offset_ns))
                        return std::nullopt;
                    return value + offset_ns;
                };
                const auto pts = shifted(pts_ns);
                const auto dts = shifted(dts_ns);
                if (!pts || !dts || *pts < 0 || *dts == AV_NOPTS_VALUE || *dts > *pts)
                    return RemuxResult::Fail(AVERROR_INVALIDDATA, "Invalid concatenated MKV timestamps.");
                MuxPacket mux;
                mux.track_num = track_numbers[destination_index];
                mux.pts_ns = static_cast<uint64_t>(*pts);
                mux.dts_ns = *dts;
                mux.is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                mux.bytes.assign(packet->data, packet->data + packet->size);
                if (!writer->Push(std::move(mux)))
                    return RemuxResult::Fail(AVERROR(EIO), writer->error());
                av_packet_unref(packet.get());
            } else if ((status = av_interleaved_write_frame(destination.context, packet.get())) < 0)
                return RemuxResult::Fail(status, AvError(status));
            if (!progress_cb(progress))
                return RemuxResult::Fail(AVERROR_EXIT, "Export cancelled.");
        }
        if (status != AVERROR_EOF)
            return RemuxResult::Fail(status, AvError(status));
    }
    if (writer) {
        if (!writer->Finalize())
            return RemuxResult::Fail(AVERROR(EIO), writer->error());
    } else {
        if ((status = av_write_trailer(destination.context)) < 0)
            return RemuxResult::Fail(status, AvError(status));
        if ((status = avio_closep(&destination.context->pb)) < 0)
            return RemuxResult::Fail(status, AvError(status));
    }
    if (output_manifest &&
        (to_mp4 || !output_references.marker_refs.empty() || output_references.source_ranges_json != "[]") &&
        !WriteMediaIdentitySidecar(
             output, *output_manifest, [&] { return progress_cb(0.99f); }, output_references)
             .succeeded())
        return RemuxResult::Fail(AVERROR(EIO), "MetadataIncomplete: cannot publish the final media identity sidecar");
    if (!progress_cb(1))
        return RemuxResult::Fail(AVERROR_EXIT, "Export cancelled.");
    staging.complete = true;
    auto result = RemuxResult::Ok();
    result.created_metadata_companions = reference_copies.Commit();
    return result;
}

} // namespace exosnap::engine
