#include <capability/translatable.h>
#include <exosnap/engine/edit_player_engine.h>
#include <exosnap/engine/edit_timeline.h>
#include <exosnap/engine/edit_timeline_compositor.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/edit_timeline_render.h>
#include <exosnap/engine/output_geometry.h>

#include "brickwall_limiter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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
    if (snapshot.timeline.crossfades.empty()) {
        plan.reason = EXOSNAP_TRANSLATABLE(
            "EditRender",
            "This recipe is not stream-copy compatible. Render export currently requires a Crossfade timeline.");
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
    };
    TimelineRenderSnapshot snapshot;
    EditRenderPlan plan;
    std::array<Decoder, 2> decoders;
    size_t peak_decoders = 0;
    BrickwallLimiter limiter{BrickwallLimiter::Config{}};

    bool Prepare(const std::vector<TimelineContribution>& contributions, std::string& error) {
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
            *slot = {};
            slot->engine = std::make_unique<EditPlayerEngine>();
            if (!slot->engine->Open(source->path, error)) {
                slot->engine.reset();
                return false;
            }
            slot->clip = contribution.clip;
        }
        peak_decoders =
            std::max(peak_decoders, static_cast<size_t>(std::count_if(decoders.begin(), decoders.end(),
                                                                      [](const auto& d) { return bool(d.engine); })));
        return true;
    }
    EditPlayerEngine* Find(uint64_t clip) {
        for (auto& decoder : decoders)
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

bool EditTimelineReader::VideoAt(int64_t time_us, TimelineVideoFrame& output, std::string& error) {
    const auto evaluation = EvaluateTimeline(impl_->snapshot.timeline, time_us);
    if (!evaluation.error.empty()) {
        error = evaluation.error;
        return false;
    }
    if (!impl_->Prepare(evaluation.video, error))
        return false;
    output = {time_us, impl_->plan.output, {}};
    for (const auto& contribution : evaluation.video) {
        auto frame = impl_->Find(contribution.clip)->DecodeFrameAtRaw(contribution.source_us);
        if (!frame) {
            error = "Cannot decode a timeline video frame.";
            return false;
        }
        output.layers.push_back({std::move(*frame), contribution.weight});
    }
    return true;
}

bool EditTimelineReader::AudioAt(int64_t first_sample, uint32_t count, std::vector<float>& output, std::string& error) {
    if (first_sample < 0 || count > 48000) {
        error = "Invalid timeline audio block.";
        return false;
    }
    output.assign(static_cast<size_t>(count) * 2, 0);
    uint32_t offset = 0;
    while (offset < count) {
        const int64_t sample = first_sample + offset;
        const int64_t time = sample * 1'000'000 / 48000;
        const auto evaluation = EvaluateTimeline(impl_->snapshot.timeline, time);
        if (!evaluation.error.empty() || !impl_->Prepare(evaluation.video, error)) {
            if (!evaluation.error.empty())
                error = evaluation.error;
            return false;
        }
        uint32_t length = count - offset;
        for (const auto& clip : impl_->snapshot.timeline.clips)
            for (const int64_t boundary : {clip.start_us, clip.end_us()}) {
                const int64_t boundary_sample = (boundary * 48000 + 999'999) / 1'000'000;
                if (boundary_sample > sample)
                    length = static_cast<uint32_t>(std::min<int64_t>(length, boundary_sample - sample));
            }
        std::vector<float> source;
        for (const auto& contribution : evaluation.audio) {
            const auto audio_clip =
                std::find_if(impl_->snapshot.timeline.clips.begin(), impl_->snapshot.timeline.clips.end(),
                             [&contribution](const auto& c) { return c.id == contribution.clip; });
            const auto video_clip =
                std::find_if(impl_->snapshot.timeline.clips.begin(), impl_->snapshot.timeline.clips.end(),
                             [&audio_clip](const auto& c) { return !c.audio && c.group == audio_clip->group; });
            auto* decoder = video_clip == impl_->snapshot.timeline.clips.end() ? nullptr : impl_->Find(video_clip->id);
            if (!decoder) {
                error = "Timeline audio is not linked to an active video source.";
                return false;
            }
            const int64_t source_sample = (contribution.source_us * 48000 + 500'000) / 1'000'000;
            if (!decoder->DecodeAudioRange(source_sample, length, source, error))
                return false;
            for (uint32_t frame = 0; frame < length; ++frame) {
                const auto weights = EvaluateTimeline(impl_->snapshot.timeline, (sample + frame) * 1'000'000 / 48000);
                const auto weight =
                    std::find_if(weights.audio.begin(), weights.audio.end(),
                                 [&contribution](const auto& c) { return c.clip == contribution.clip; });
                if (weight == weights.audio.end())
                    continue;
                for (size_t channel = 0; channel < 2; ++channel)
                    output[static_cast<size_t>(offset + frame) * 2 + channel] +=
                        source[static_cast<size_t>(frame) * 2 + channel] * static_cast<float>(weight->weight);
            }
        }
        offset += length;
    }
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
