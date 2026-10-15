#include <capability/translatable.h>
#include <exosnap/engine/edit_player_engine.h>
#include <exosnap/engine/edit_timeline.h>
#include <exosnap/engine/edit_timeline_compositor.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/edit_timeline_render.h>
#include <exosnap/engine/edit_types.h>
#include <exosnap/engine/output_geometry.h>

#include "brickwall_limiter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace exosnap::engine {

EditRenderPlan ClassifyEditExport(const TimelineRenderSnapshot& snapshot,
                                  const TimelineExportAssessment& copy_assessment, bool to_mp4) {
    EditRenderPlan plan;
    if (snapshot.timeline.crossfades.empty() && copy_assessment.eligibility == TimelineExportEligibility::StreamCopy) {
        plan.path = EditExportPath::StreamCopy;
        return plan;
    }
    if (to_mp4) {
        plan.reason =
            EXOSNAP_TRANSLATABLE("EditRender", "Render export currently supports Matroska with PCM audio. Choose MKV.");
        return plan;
    }
    if (snapshot.timeline.duration_us <= 0) {
        plan.reason = EXOSNAP_TRANSLATABLE("EditRender", "The timeline is empty.");
        return plan;
    }
    for (const auto& clip : snapshot.timeline.clips) {
        const auto source = std::find_if(snapshot.sources.begin(), snapshot.sources.end(),
                                         [&clip](const auto& s) { return s.id == clip.asset; });
        if (source == snapshot.sources.end() || !source->metadata.error.empty() || clip.source_in_us < 0 ||
            clip.source_out_us <= clip.source_in_us || clip.source_out_us > source->metadata.duration_us ||
            clip.start_us < 0) {
            plan.reason = EXOSNAP_TRANSLATABLE(
                "EditRender", "A timeline source is unavailable or its clip exceeds the source interval.");
            return plan;
        }
        if (clip.audio) {
            const auto linked = std::find_if(
                snapshot.timeline.clips.begin(), snapshot.timeline.clips.end(), [&clip](const auto& video) {
                    return !video.audio && video.group && video.group == clip.group && video.asset == clip.asset &&
                           video.start_us == clip.start_us && video.source_in_us == clip.source_in_us &&
                           video.source_out_us == clip.source_out_us;
                });
            if (linked == snapshot.timeline.clips.end()) {
                plan.reason =
                    EXOSNAP_TRANSLATABLE("EditRender", "Independent audio edits are not supported by render export.");
                return plan;
            }
            continue;
        }
        const auto& metadata = source->metadata;
        if (!metadata.render_color_supported) {
            plan.reason = metadata.render_color_reason;
            return plan;
        }
        if (metadata.width <= 0 || metadata.height <= 0 ||
            !IsEncoderAlignedSize({static_cast<uint32_t>(metadata.width), static_cast<uint32_t>(metadata.height)})) {
            plan.reason = EXOSNAP_TRANSLATABLE(
                "EditRender", "Match source rendering needs even video dimensions and an exact positive frame rate.");
            return plan;
        }
        if (!plan.output.IsValid()) {
            plan.output = {static_cast<uint32_t>(metadata.width), static_cast<uint32_t>(metadata.height)};
            plan.fps_num = metadata.fps_num;
            plan.fps_den = metadata.fps_den;
        }
        for (const int64_t time : {clip.start_us, clip.end_us() - 1}) {
            const auto evaluation = EvaluateTimeline(snapshot.timeline, time);
            if (!evaluation.error.empty()) {
                plan.reason = evaluation.error;
                return plan;
            }
        }
    }
    if (!IsEncoderAlignedSize(plan.output) || plan.fps_num <= 0 || plan.fps_den <= 0) {
        plan.reason = EXOSNAP_TRANSLATABLE(
            "EditRender", "Match source rendering needs even video dimensions and an exact positive frame rate.");
        return plan;
    }
    plan.path = EditExportPath::Render;
    return plan;
}

struct EditTimelineReader::Impl {
    struct Decoder {
        uint64_t clip = 0;
        std::unique_ptr<EditPlayerEngine> engine;
        // Unset only for a clip without a stream reference over a source with
        // several audio streams; that legacy path mixes the confirmed selection.
        std::optional<uint32_t> stream;
    };
    static constexpr size_t kMaxAudioDecoders = 8;
    TimelineRenderSnapshot snapshot;
    EditRenderPlan plan;
    std::array<Decoder, 2> video_decoders;
    std::vector<Decoder> audio_decoders;
    size_t peak_decoders = 0;
    VideoReadMeasurements retired_video_measurements;

    void Retire(Decoder& decoder, bool audio) {
        if (decoder.engine && !audio) {
            const auto stats = decoder.engine->VideoReadStats();
            retired_video_measurements.seeks += stats.seeks;
            retired_video_measurements.flushes += stats.flushes;
            retired_video_measurements.decoded += stats.decoded;
            retired_video_measurements.selected += stats.selected;
            retired_video_measurements.held += stats.held;
        }
        decoder = {};
    }
    BrickwallLimiter limiter{BrickwallLimiter::Config{}};

    [[nodiscard]] size_t OpenDecoders() const {
        size_t open = 0;
        for (const auto& decoder : video_decoders)
            open += decoder.engine ? 1 : 0;
        for (const auto& decoder : audio_decoders)
            open += decoder.engine ? 1 : 0;
        return open;
    }

    bool Prepare(const std::vector<TimelineContribution>& contributions, std::string& error) {
        auto& decoders = video_decoders;
        std::set<uint64_t> clips;
        for (const auto& contribution : contributions)
            clips.insert(contribution.clip);
        if (clips.size() > decoders.size()) {
            error = "More than two simultaneous timeline sources are unsupported.";
            return false;
        }
        for (const auto& contribution : contributions) {
            if (Find(contribution.clip))
                continue;
            const auto source = std::find_if(snapshot.sources.begin(), snapshot.sources.end(),
                                             [&contribution](const auto& s) { return s.id == contribution.asset; });
            if (source == snapshot.sources.end()) {
                error = "Timeline source is missing.";
                return false;
            }
            auto slot = std::find_if(decoders.begin(), decoders.end(), [](const auto& d) { return !d.engine; });
            if (slot == decoders.end())
                slot = std::find_if(decoders.begin(), decoders.end(),
                                    [&clips](const auto& d) { return !clips.contains(d.clip); });
            Retire(*slot, false);
            slot->engine = std::make_unique<EditPlayerEngine>();
            if (!slot->engine->Open(source->path, error)) {
                slot->engine.reset();
                return false;
            }
            if (source->monitor_selection && !slot->engine->SetMonitorSelection(*source->monitor_selection, error)) {
                slot->engine.reset();
                return false;
            }
            slot->clip = contribution.clip;
        }
        peak_decoders = std::max(peak_decoders, OpenDecoders());
        return true;
    }

    Decoder* AudioDecoder(const AudioIntervalContribution& contribution, int64_t at_sample, std::string& error) {
        for (auto& decoder : audio_decoders)
            if (decoder.clip == contribution.clip && decoder.engine)
                return &decoder;
        std::erase_if(audio_decoders, [&](const auto& decoder) {
            const auto clip = std::find_if(snapshot.timeline.clips.begin(), snapshot.timeline.clips.end(),
                                           [&](const auto& value) { return value.id == decoder.clip; });
            return clip == snapshot.timeline.clips.end() || AudioSampleIndex(clip->end_us()) <= at_sample;
        });
        const auto source = std::find_if(snapshot.sources.begin(), snapshot.sources.end(),
                                         [&contribution](const auto& s) { return s.id == contribution.asset; });
        if (source == snapshot.sources.end()) {
            error = "Timeline source is missing.";
            return nullptr;
        }
        std::optional<uint32_t> stream;
        if (contribution.stream) {
            if (contribution.stream->kind != TimelineStreamKind::Audio) {
                error = "The selected container stream is not audio.";
                return nullptr;
            }
            stream = contribution.stream->container_index;
        } else {
            size_t audio_streams = 0;
            for (const auto& descriptor : source->metadata.streams)
                if (descriptor.kind == TimelineStreamKind::Audio) {
                    ++audio_streams;
                    stream = descriptor.container_index;
                }
            if (audio_streams != 1)
                stream.reset();
        }
        if (audio_decoders.size() >= kMaxAudioDecoders) {
            error = "Too many simultaneous timeline audio sources.";
            return nullptr;
        }
        auto& decoder = audio_decoders.emplace_back();
        decoder.clip = contribution.clip;
        decoder.stream = stream;
        decoder.engine = std::make_unique<EditPlayerEngine>();
        const bool opened = stream ? decoder.engine->OpenAudioStream(source->path, *stream, error)
                                   : decoder.engine->Open(source->path, error);
        if (!opened || (!stream && source->monitor_selection &&
                        !decoder.engine->SetMonitorSelection(*source->monitor_selection, error))) {
            audio_decoders.pop_back();
            return nullptr;
        }
        peak_decoders = std::max(peak_decoders, OpenDecoders());
        return &decoder;
    }
    EditPlayerEngine* Find(uint64_t clip) {
        for (auto& decoder : video_decoders)
            if (decoder.clip == clip && decoder.engine)
                return decoder.engine.get();
        return nullptr;
    }
};

EditTimelineReader::EditTimelineReader() : impl_(std::make_unique<Impl>()) {
}
EditTimelineReader::~EditTimelineReader() = default;

bool EditTimelineReader::Open(TimelineRenderSnapshot snapshot, const EditRenderPlan& plan, std::string& error) {
    if (plan.path != EditExportPath::Render) {
        error = plan.reason;
        return false;
    }
    impl_ = std::make_unique<Impl>();
    impl_->snapshot = std::move(snapshot);
    impl_->plan = plan;
    return true;
}

bool EditTimelineReader::UpdateSnapshot(TimelineRenderSnapshot snapshot, const EditRenderPlan& plan,
                                        std::string& error) {
    if (plan.path != EditExportPath::Render) {
        error = plan.reason;
        return false;
    }
    const auto same_monitor = [](const auto& before, const auto& after) {
        if (before.has_value() != after.has_value())
            return false;
        if (!before)
            return true;
        if (before->media_id != after->media_id || before->revision != after->revision ||
            before->overlap_confirmed != after->overlap_confirmed ||
            before->unverified_asset_binding != after->unverified_asset_binding ||
            before->tracks.size() != after->tracks.size())
            return false;
        for (size_t index = 0; index < before->tracks.size(); ++index) {
            const auto& a = before->tracks[index];
            const auto& b = after->tracks[index];
            if (a.track_id != b.track_id || a.gain_linear != b.gain_linear || a.muted != b.muted)
                return false;
        }
        return true;
    };
    const auto compatible = [&](const Impl::Decoder& decoder, bool audio) {
        const auto find_clip = [&](const auto& clips) {
            return std::find_if(clips.begin(), clips.end(), [&](const auto& clip) { return clip.id == decoder.clip; });
        };
        const auto before = find_clip(impl_->snapshot.timeline.clips);
        const auto after = find_clip(snapshot.timeline.clips);
        if (before == impl_->snapshot.timeline.clips.end() || after == snapshot.timeline.clips.end() ||
            before->asset != after->asset || before->stream != after->stream || before->audio != after->audio ||
            before->start_us != after->start_us || before->source_in_us != after->source_in_us ||
            before->source_out_us != after->source_out_us)
            return false;
        const auto find_source = [&](const auto& sources) {
            return std::find_if(sources.begin(), sources.end(),
                                [&](const auto& source) { return source.id == before->asset; });
        };
        const auto source_before = find_source(impl_->snapshot.sources);
        const auto source_after = find_source(snapshot.sources);
        if (source_before == impl_->snapshot.sources.end() || source_after == snapshot.sources.end())
            return false;
        const auto& a = source_before->metadata;
        const auto& b = source_after->metadata;
        return source_before->path == source_after->path && a.content_identity_valid && b.content_identity_valid &&
               a.file_identity == b.file_identity && a.file_size == b.file_size &&
               a.content_sha256 == b.content_sha256 &&
               (!audio || same_monitor(source_before->monitor_selection, source_after->monitor_selection));
    };
    for (auto& decoder : impl_->video_decoders)
        if (decoder.engine && !compatible(decoder, false))
            impl_->Retire(decoder, false);
    for (auto& decoder : impl_->audio_decoders)
        if (decoder.engine && !compatible(decoder, true))
            impl_->Retire(decoder, true);
    std::erase_if(impl_->audio_decoders, [](const auto& decoder) { return !decoder.engine; });
    impl_->snapshot = std::move(snapshot);
    impl_->plan = plan;
    return true;
}

VideoReadMeasurements EditTimelineReader::VideoReadStats() const noexcept {
    VideoReadMeasurements result = impl_->retired_video_measurements;
    for (const auto& slot : impl_->video_decoders) {
        if (!slot.engine)
            continue;
        const auto stats = slot.engine->VideoReadStats();
        result.seeks += stats.seeks;
        result.flushes += stats.flushes;
        result.decoded += stats.decoded;
        result.selected += stats.selected;
        result.held += stats.held;
    }
    return result;
}

bool EditTimelineReader::VideoAt(int64_t time_us, TimelineVideoFrame& output, std::string& error) {
    return VideoAt(time_us, output, error, VideoReadMode::Sequential, 0);
}

bool EditTimelineReader::VideoAt(int64_t time_us, TimelineVideoFrame& output, std::string& error, VideoReadMode mode,
                                 uint64_t generation) {
    const auto evaluation = EvaluateTimeline(impl_->snapshot.timeline, time_us);
    if (!evaluation.error.empty()) {
        error = evaluation.error;
        return false;
    }
    if (!impl_->Prepare(evaluation.video, error))
        return false;
    output = {time_us, impl_->plan.output, {}};
    for (const auto& contribution : evaluation.video) {
        auto frame = impl_->Find(contribution.clip)->ReadFrameAtRaw(contribution.source_us, mode, generation);
        if (!frame) {
            error = "Cannot decode a timeline video frame.";
            return false;
        }
        output.layers.push_back({std::move(*frame), contribution.weight});
    }
    return true;
}

bool EditTimelineReader::AudioAt(int64_t first_sample, uint32_t count, std::vector<float>& output, std::string& error) {
    constexpr int64_t kRate = 48000;
    constexpr int64_t kMicroseconds = 1'000'000;
    if (first_sample < 0 || count > kRate ||
        first_sample > (std::numeric_limits<int64_t>::max)() / kMicroseconds - kRate) {
        error = "Invalid timeline audio block.";
        return false;
    }
    output.assign(static_cast<size_t>(count) * 2, 0);
    if (count == 0)
        return true;
    const int64_t last_sample = first_sample + count;
    const auto intervals = EvaluateAudioIntervals(impl_->snapshot.timeline, first_sample * kMicroseconds / kRate,
                                                  (last_sample * kMicroseconds + kRate - 1) / kRate);
    if (!intervals.error.empty()) {
        error = intervals.error;
        return false;
    }
    std::set<uint64_t> used;
    std::vector<float> source;
    for (const auto& interval : intervals.intervals) {
        const int64_t begin = std::max(first_sample, AudioSampleIndex(interval.in_us));
        const int64_t end = std::min(last_sample, AudioSampleIndex(interval.out_us));
        for (const auto& contribution : interval.contributions) {
            const int64_t clip_begin = AudioSampleIndex(contribution.clip_start_us);
            const int64_t clip_end =
                AudioSampleIndex(contribution.clip_start_us + (contribution.source_out_us - contribution.source_in_us));
            const int64_t from = std::max(begin, clip_begin);
            const int64_t to = std::min(end, clip_end);
            if (to <= from)
                continue;
            auto* decoder = impl_->AudioDecoder(contribution, from, error);
            if (!decoder)
                return false;
            used.insert(contribution.clip);
            const auto source_sample = AudioSourceSample(contribution, from);
            if (!source_sample) {
                error = "The source audio position is outside the supported range.";
                return false;
            }
            const auto length = static_cast<uint32_t>(to - from);
            const bool decoded =
                decoder->stream
                    ? decoder->engine->DecodeAudioStreamRange(*decoder->stream, *source_sample, length, source, error)
                    : decoder->engine->DecodeAudioRange(*source_sample, length, source, error);
            if (!decoded)
                return false;
            for (uint32_t frame = 0; frame < length; ++frame) {
                const auto gain = EvaluateAudioGain(contribution, from + frame);
                if (!gain) {
                    error = "Invalid timeline audio envelope.";
                    return false;
                }
                const auto at = static_cast<size_t>(from - first_sample + frame) * 2;
                for (size_t channel = 0; channel < 2; ++channel)
                    output[at + channel] +=
                        source[static_cast<size_t>(frame) * 2 + channel] * static_cast<float>(*gain);
            }
        }
    }
    std::erase_if(impl_->audio_decoders, [&used](const auto& decoder) { return !used.contains(decoder.clip); });
    impl_->limiter.Process(output.data(), count);
    return true;
}

void EditTimelineReader::ResetAudio() {
    impl_->limiter.Reset();
}
size_t EditTimelineReader::PeakDecoders() const {
    return impl_->peak_decoders;
}

} // namespace exosnap::engine
