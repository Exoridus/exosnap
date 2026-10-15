#include <exosnap/engine/edit_timeline.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/edit_timeline_render.h>
#include <exosnap/engine/mp4_remuxer.h>
#include <exosnap/engine/packet_types.h>

#include "audio_media_manifest_adapter.h"
#include "edit_mix_encoder.h"
#include "matroska_packet_timestamps.h"
#include "matroska_stream_writer.h"
#include "media_identity_sidecar.h"
#include "render_audio_manifest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

namespace exosnap::engine {
namespace {

constexpr AVRational kNanoseconds{1, 1'000'000'000};

std::string Utf8Path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

struct InputFile {
    AVFormatContext* context = nullptr;
    std::shared_ptr<void> lease;
    ~InputFile() {
        avformat_close_input(&context);
    }
    bool Open(const std::filesystem::path& path) {
        auto acquired = AcquireMediaReadLease(path);
        if (!acquired.succeeded())
            return false;
        lease = std::move(*acquired.value());
        return avformat_open_input(&context, Utf8Path(path).c_str(), nullptr, nullptr) >= 0 &&
               avformat_find_stream_info(context, nullptr) >= 0;
    }
    [[nodiscard]] int VideoStream() const {
        return av_find_best_stream(context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    }
};

// Source timestamps in nanoseconds, preferring the exact ExoSnap mapping that a
// recording stores beside rounded Matroska block times.
bool SourceTimestampsNs(AVPacket& packet, const AVStream& stream, int64_t& pts_ns, int64_t& dts_ns) {
    size_t additional_size = 0;
    const auto* additional = av_packet_get_side_data(&packet, AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL, &additional_size);
    if (additional && additional_size >= 8) {
        uint64_t type = 0;
        for (size_t i = 0; i < 8; ++i)
            type = (type << 8) | additional[i];
        if (type == kPacketTimestampMappingType) {
            const auto exact = DecodeMatroskaPacketTimestamps({additional + 8, additional_size - 8});
            if (!exact)
                return false;
            pts_ns = static_cast<int64_t>(exact->pts_ns);
            dts_ns = exact->dts_ns;
            return true;
        }
    }
    if (packet.pts == AV_NOPTS_VALUE || packet.dts == AV_NOPTS_VALUE)
        return false;
    pts_ns = av_rescale_q(packet.pts, stream.time_base, kNanoseconds);
    dts_ns = av_rescale_q(packet.dts, stream.time_base, kNanoseconds);
    return true;
}

} // namespace

RemuxResult ExportEditTimelineWithCopiedVideo(const TimelineRenderSnapshot& snapshot,
                                              const std::vector<TimelineExportClip>& video_recipe,
                                              const std::filesystem::path& output, bool render_audio, bool aac_audio,
                                              RemuxProgressCallback progress) {
    std::error_code path_error;
    if (output.empty() || std::filesystem::exists(output, path_error) || path_error)
        return RemuxResult::Fail(-1, "Timeline export requires a new staging output path.");
    if (!progress)
        progress = RemuxNoopCallback();
    std::string error;
    const auto run = [&]() -> bool {
        const auto assessment = AssessTimelineExport(video_recipe, false);
        if (assessment.eligibility != TimelineExportEligibility::StreamCopy) {
            error = assessment.reason;
            return false;
        }
        int64_t covered = 0;
        for (const auto& clip : video_recipe) {
            const auto metadata = ProbeEditMedia(clip.source);
            if (clip.timeline_start_us != covered || clip.source_in_us != 0 ||
                std::abs(clip.source_out_us - metadata.duration_us) > 1000) {
                error = "Copied video must use complete sources that cover the timeline from its start.";
                return false;
            }
            covered += clip.source_out_us - clip.source_in_us;
        }
        if (std::abs(covered - snapshot.timeline.duration_us) > 1000) {
            error = "Copied video must cover the whole timeline.";
            return false;
        }
        std::optional<RenderAudioPreflight> preflight;
        if (render_audio) {
            auto prepared = PrepareRenderAudioManifest(snapshot);
            if (!prepared.succeeded()) {
                error = prepared.error()->context;
                return false;
            }
            preflight = std::move(*prepared.value());
        }
        const bool audio = preflight && !preflight->manifest.tracks.empty();
        InputFile first;
        if (!first.Open(video_recipe.front().source) || first.VideoStream() < 0) {
            error = "Cannot open the copied video source.";
            return false;
        }
        MatroskaStreamConfig config;
        std::vector<uint64_t> track_numbers;
        if (!ConfigureMatroskaStreamCopy(first.context, Utf8Path(output), config, track_numbers)) {
            error = "The source video track cannot be copied into Matroska.";
            return false;
        }
        config.audio_tracks = {};
        config.audio_track_count = audio ? 1 : 0;
        EditMixEncoder encoder;
        if (audio) {
            if (!encoder.Init(aac_audio, error))
                return false;
            encoder.Configure(config, preflight->manifest);
            config.audio_manifest = preflight->manifest;
            config.audio_tracks[0].name = preflight->manifest.tracks[0].container.name;
        } else {
            config.audio_manifest.reset();
        }
        const auto duration_ns = static_cast<uint64_t>(snapshot.timeline.duration_us) * 1000ULL;
        config.timeline_duration_ns = duration_ns;
        MatroskaStreamWriter writer;
        if (!writer.Open(config)) {
            error = writer.error();
            return false;
        }
        EditTimelineReader reader;
        EditRenderPlan reader_plan;
        reader_plan.path = EditExportPath::Render;
        if (audio && !reader.Open(snapshot, reader_plan, error))
            return false;
        const uint64_t audio_count = (duration_ns * 48000 + 500'000'000ULL) / 1'000'000'000ULL;
        uint64_t audio_position = 0;
        uint64_t accumulated = 0;
        std::vector<float> samples;
        std::vector<EncodedAudioPacket> audio_packets;
        const auto push_audio = [&](uint64_t through) {
            through = std::min(through, audio_count);
            while (audio && audio_position < through) {
                const auto count = static_cast<uint32_t>(std::min<uint64_t>(4800, through - audio_position));
                if (!reader.AudioAt(static_cast<int64_t>(audio_position), count, samples, error))
                    return false;
                encoder.encoder->FeedFloat32(samples.data(), samples.size(), audio_position * 1'000'000'000ULL / 48000,
                                             accumulated, 48000, 2, audio_packets);
                for (auto& packet : audio_packets)
                    if (!writer.Push({packet.pts_ns, 2, true, std::move(packet.bytes), {}})) {
                        error = writer.error();
                        return false;
                    }
                audio_packets.clear();
                audio_position += count;
            }
            return true;
        };
        auto free_packet = [](AVPacket* packet) { av_packet_free(&packet); };
        std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(), free_packet);
        if (!packet) {
            error = "Cannot allocate an export packet.";
            return false;
        }
        int64_t last_dts = (std::numeric_limits<int64_t>::min)();
        for (const auto& clip : video_recipe) {
            InputFile source;
            if (!source.Open(clip.source)) {
                error = "Cannot open a copied video source.";
                return false;
            }
            const int video = source.VideoStream();
            if (video < 0) {
                error = "A copied video source has no video stream.";
                return false;
            }
            const int64_t origin_us = source.context->start_time == AV_NOPTS_VALUE ? 0 : source.context->start_time;
            const int64_t offset_ns = (clip.timeline_start_us - origin_us) * 1000;
            int status = 0;
            while ((status = av_read_frame(source.context, packet.get())) >= 0) {
                if (packet->stream_index != video) {
                    av_packet_unref(packet.get());
                    continue;
                }
                int64_t pts_ns = 0;
                int64_t dts_ns = 0;
                if (!SourceTimestampsNs(*packet, *source.context->streams[video], pts_ns, dts_ns)) {
                    error = "A source video packet has no usable timestamps.";
                    return false;
                }
                pts_ns += offset_ns;
                dts_ns += offset_ns;
                if (pts_ns < 0 || dts_ns > pts_ns || dts_ns <= last_dts) {
                    error = "Copied video timestamps are not monotonic across sources.";
                    return false;
                }
                last_dts = dts_ns;
                if (!push_audio(static_cast<uint64_t>(std::max<int64_t>(0, dts_ns)) * 48000 / 1'000'000'000ULL))
                    return false;
                MuxPacket mux;
                mux.track_num = 1;
                mux.pts_ns = static_cast<uint64_t>(pts_ns);
                mux.dts_ns = dts_ns;
                mux.is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                mux.bytes.assign(packet->data, packet->data + packet->size);
                av_packet_unref(packet.get());
                if (!writer.Push(std::move(mux))) {
                    error = writer.error();
                    return false;
                }
                if (!progress(std::clamp(static_cast<float>(pts_ns) / static_cast<float>(duration_ns), 0.0f, 0.99f))) {
                    error = "Export cancelled.";
                    return false;
                }
            }
            if (status != AVERROR_EOF) {
                error = "Cannot read a copied video source.";
                return false;
            }
        }
        if (!push_audio(audio_count))
            return false;
        if (audio) {
            encoder.encoder->Flush(audio_packets);
            for (auto& packet_out : audio_packets)
                if (!writer.Push({packet_out.pts_ns, 2, true, std::move(packet_out.bytes), {}})) {
                    error = writer.error();
                    return false;
                }
            encoder.encoder->Shutdown();
        }
        if (!writer.Finalize()) {
            error = writer.error();
            return false;
        }
        return progress(1);
    };
    if (!run()) {
        std::error_code ignored;
        std::filesystem::remove(output, ignored);
        return RemuxResult::Fail(-1, error.empty() ? "Export cancelled." : error);
    }
    return RemuxResult::Ok();
}

} // namespace exosnap::engine
