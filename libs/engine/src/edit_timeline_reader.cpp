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

TimelineVideoAdmission AssessTimelineVideoAdmission(const TimelineRenderSnapshot& snapshot, int64_t in_us,
                                                    int64_t out_us) {
    TimelineVideoAdmission result;
    const auto& timeline = snapshot.timeline;
    struct Event {
        int64_t time_us = 0;
        int sources = 0;
        int64_t pixels = 0;
    };
    std::vector<Event> events;
    for (const auto& clip : timeline.clips) {
        if (clip.audio || !clip.enabled || clip.end_us() <= in_us || clip.start_us >= out_us)
            continue;
        const auto track = std::find_if(timeline.tracks.begin(), timeline.tracks.end(),
                                        [&clip](const auto& value) { return value.id == clip.track; });
        if (track != timeline.tracks.end() && !track->enabled)
            continue;
        const auto source = std::find_if(snapshot.sources.begin(), snapshot.sources.end(),
                                         [&clip](const auto& value) { return value.id == clip.asset; });
        if (source == snapshot.sources.end() || source->metadata.width <= 0 || source->metadata.height <= 0) {
            result.admitted = false;
            continue;
        }
        const int64_t pixels = int64_t{source->metadata.width} * source->metadata.height;
        events.push_back({std::max(clip.start_us, in_us), 1, pixels});
        events.push_back({std::min(clip.end_us(), out_us), -1, -pixels});
    }
    // Ends sort before starts at one instant: clip intervals are half-open.
    std::sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
        return left.time_us != right.time_us ? left.time_us < right.time_us : left.sources < right.sources;
    });
    int64_t sources = 0;
    int64_t pixels = 0;
    for (const auto& event : events) {
        sources += event.sources;
        pixels += event.pixels;
        result.peak_sources = std::max(result.peak_sources, static_cast<size_t>(std::max<int64_t>(sources, 0)));
        result.peak_pixels = std::max(result.peak_pixels, static_cast<uint64_t>(std::max<int64_t>(pixels, 0)));
    }
    result.admitted = result.admitted && result.peak_sources <= kMaxTimelineVideoSources &&
                      result.peak_pixels <= kMaxTimelineVideoSourcePixels;
    return result;
}

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
            // A per-stream audio clip has its own decoder cursor. Only the legacy
            // single audio row follows its linked video placement.
            if (clip.stream)
                continue;
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
    if (!AssessTimelineVideoAdmission(snapshot, 0, snapshot.timeline.duration_us).admitted) {
        plan.reason = EXOSNAP_TRANSLATABLE(
            "EditRender", "Too many video layers are visible at the same time. Disable or shorten overlapping clips.");
        return plan;
    }
    plan.path = EditExportPath::Render;
    return plan;
}

struct EditTimelineReader::Impl {
    struct Decoder {
        uint64_t clip = 0;
        uint64_t asset = 0;
        bool video_reposition = false;
        int64_t prepared_first_sample = 0;
        std::vector<float> prepared_samples;
        std::unique_ptr<EditPlayerEngine> engine;
        // Unset only for a clip without a stream reference over a source with
        // several audio streams; that legacy path mixes the confirmed selection.
        std::optional<uint32_t> stream;
    };
    static constexpr size_t kMaxAudioDecoders = 8;
    TimelineRenderSnapshot snapshot;
    TimelineSnapshot source_timeline;
    const std::atomic<bool>* cancelled = nullptr;
    const VideoOpenAdmission* before_video_open = nullptr;
    uint64_t video_decoder_opens = 0;
    EditRenderPlan plan;
    std::vector<Decoder> video_decoders;
    std::vector<Decoder> audio_decoders;
    size_t peak_decoders = 0;
    VideoReadMeasurements retired_video_measurements;
    TimelineAudioReadMeasurements audio_measurements;

    void Retire(Decoder& decoder, bool audio) {
        if (decoder.engine && !audio) {
            const auto stats = decoder.engine->VideoReadStats();
            retired_video_measurements.seeks += stats.seeks;
            retired_video_measurements.flushes += stats.flushes;
            retired_video_measurements.decoded += stats.decoded;
            retired_video_measurements.selected += stats.selected;
            retired_video_measurements.held += stats.held;
        }
        if (decoder.engine && audio) {
            audio_measurements.seeks += decoder.engine->AudioSeekCount();
            audio_measurements.decoded_frames += decoder.engine->AudioDecodedFrameCount();
        }
        decoder = {};
    }
    BrickwallLimiter limiter{BrickwallLimiter::Config{}};

    void RefreshSourceTimeline() {
        source_timeline = snapshot.timeline;
        for (auto& clip : source_timeline.clips)
            if (clip.audio) {
                clip.enabled = true;
                clip.gain_db = 0;
            }
        for (auto& track : source_timeline.tracks)
            if (track.audio) {
                track.enabled = true;
                track.gain_db = 0;
            }
    }

    bool ValidateSourceAccess(const TimelineMediaSource& source, std::string& error) {
        if (source.identity_proof && (!source.access || source.identity_proof->Source() != source.access ||
                                      !source.metadata.content_identity_valid ||
                                      source.identity_proof->Identity().sha256 != source.metadata.content_sha256)) {
            error = EXOSNAP_TRANSLATABLE("EditRender", "Timeline identity proof does not match the admitted source.");
            return false;
        }
        if (source.access && source.metadata.content_identity_valid) {
            const auto proof = VerifyMediaIdentity(source.access, source.metadata.content_sha256, {}, {}, cancelled);
            if (!proof.succeeded()) {
                error = proof.error()->context;
                return false;
            }
        }
        return true;
    }

    bool UnresolvedLegacyAudio(const AudioIntervalContribution& contribution) const {
        if (contribution.stream)
            return false;
        const auto source = std::find_if(snapshot.sources.begin(), snapshot.sources.end(),
                                         [&](const auto& value) { return value.id == contribution.asset; });
        return source != snapshot.sources.end() && source->metadata.audio_metadata.monitoring_confirmation_required &&
               (!source->monitor_selection || !source->monitor_selection->overlap_confirmed);
    }

    [[nodiscard]] size_t OpenDecoders() const {
        size_t open = 0;
        for (const auto& decoder : video_decoders)
            open += decoder.engine ? 1 : 0;
        for (const auto& decoder : audio_decoders)
            open += decoder.engine ? 1 : 0;
        return open;
    }

    bool Prepare(const std::vector<TimelineContribution>& contributions, std::string& error) {
        std::set<uint64_t> clips;
        for (const auto& contribution : contributions)
            clips.insert(contribution.clip);
        if (clips.size() > kMaxTimelineVideoSources) {
            error = "More simultaneous timeline video sources than the compositor admits.";
            return false;
        }
        for (const auto& contribution : contributions) {
            const auto source = std::find_if(snapshot.sources.begin(), snapshot.sources.end(),
                                             [&](const auto& value) { return value.id == contribution.asset; });
            if (source == snapshot.sources.end()) {
                error = "Timeline source is missing.";
                return false;
            }
            if (!ValidateSourceAccess(*source, error))
                return false;
            std::optional<uint32_t> stream;
            if (contribution.stream) {
                if (contribution.stream->kind != TimelineStreamKind::Video ||
                    (contribution.stream->asset_id && contribution.stream->asset_id != contribution.asset)) {
                    error = EXOSNAP_TRANSLATABLE("EditRender",
                                                 "The selected timeline stream is not video for this source.");
                    return false;
                }
                stream = contribution.stream->container_index;
                if (!source->metadata.streams.empty() &&
                    std::none_of(source->metadata.streams.begin(), source->metadata.streams.end(),
                                 [&](const auto& value) {
                                     return value.kind == TimelineStreamKind::Video && value.container_index == *stream;
                                 })) {
                    error = EXOSNAP_TRANSLATABLE("EditRender",
                                                 "The selected video stream is absent from the source layout.");
                    return false;
                }
            }
            bool retained = false;
            for (auto& decoder : video_decoders)
                if (decoder.clip == contribution.clip && decoder.engine) {
                    if (decoder.asset == contribution.asset && decoder.stream == stream)
                        retained = true;
                    else
                        Retire(decoder, false);
                    break;
                }
            if (retained)
                continue;
            const auto reusable = std::find_if(video_decoders.begin(), video_decoders.end(), [&](const auto& decoder) {
                return decoder.engine && !clips.contains(decoder.clip) && decoder.asset == contribution.asset &&
                       decoder.stream == stream;
            });
            if (reusable != video_decoders.end()) {
                reusable->clip = contribution.clip;
                reusable->video_reposition = true;
                continue;
            }
            for (auto& decoder : video_decoders)
                if (decoder.engine && !clips.contains(decoder.clip))
                    Retire(decoder, false);
            std::erase_if(video_decoders, [](const auto& decoder) { return !decoder.engine; });
            Decoder decoder;
            decoder.engine = std::make_unique<EditPlayerEngine>();
            static const std::atomic<bool> never_cancelled{false};
            if (before_video_open && *before_video_open &&
                !(*before_video_open)(cancelled ? *cancelled : never_cancelled)) {
                error = EXOSNAP_TRANSLATABLE("EditRender", "Video source opening cancelled.");
                return false;
            }
            const bool opened =
                source->access ? (stream ? decoder.engine->OpenVideoStream(source->access, *stream, error, cancelled)
                                         : decoder.engine->Open(source->access, error, cancelled))
                               : (stream ? decoder.engine->OpenVideoStream(source->path, *stream, error, cancelled)
                                         : decoder.engine->Open(source->path, error, cancelled));
            if (!opened)
                return false;
            if (source->monitor_selection &&
                !decoder.engine->SetMonitorSelection(*source->monitor_selection, error, cancelled))
                return false;
            ++video_decoder_opens;
            decoder.clip = contribution.clip;
            decoder.asset = contribution.asset;
            decoder.stream = stream;
            video_decoders.push_back(std::move(decoder));
        }
        for (auto& decoder : video_decoders)
            if (decoder.engine && !clips.contains(decoder.clip))
                Retire(decoder, false);
        std::erase_if(video_decoders, [](const auto& decoder) { return !decoder.engine; });
        peak_decoders = std::max(peak_decoders, OpenDecoders());
        return true;
    }

    bool SameAudioSource(const Decoder& decoder, uint64_t asset, std::optional<uint32_t> stream) const {
        if (decoder.stream != stream)
            return false;
        if (decoder.asset == asset)
            return true;
        if (!stream)
            return false;
        const auto find = [&](uint64_t id) {
            return std::find_if(snapshot.sources.begin(), snapshot.sources.end(),
                                [&](const auto& value) { return value.id == id; });
        };
        const auto previous = find(decoder.asset);
        const auto next = find(asset);
        return previous != snapshot.sources.end() && next != snapshot.sources.end() && previous->access &&
               previous->access == next->access;
    }

    bool ReadSourceAudio(Decoder& decoder, int64_t first_sample, uint32_t count, std::vector<float>& output,
                         std::string& error) {
        const auto cached_count = static_cast<int64_t>(decoder.prepared_samples.size() / 2);
        if (cached_count && first_sample >= decoder.prepared_first_sample &&
            first_sample - decoder.prepared_first_sample <= cached_count - count) {
            const auto offset = static_cast<size_t>(first_sample - decoder.prepared_first_sample) * 2;
            output.assign(decoder.prepared_samples.begin() + static_cast<ptrdiff_t>(offset),
                          decoder.prepared_samples.begin() + static_cast<ptrdiff_t>(offset + count * 2));
            ++audio_measurements.prepared_cache_hits;
            if (first_sample + count == decoder.prepared_first_sample + cached_count)
                decoder.prepared_samples.clear();
            return true;
        }
        decoder.prepared_samples.clear();
        return decoder.stream ? decoder.engine->ReadAudioStreamSequential(*decoder.stream, first_sample, count, output,
                                                                          error, cancelled)
                              : decoder.engine->DecodeAudioRange(first_sample, count, output, error, cancelled);
    }
    Decoder* AudioDecoder(const AudioIntervalContribution& contribution, int64_t at_sample, std::string& error) {
        for (auto& decoder : audio_decoders)
            if (decoder.clip == contribution.clip && decoder.engine)
                return &decoder;
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
        for (auto& decoder : audio_decoders) {
            const auto previous = std::find_if(snapshot.timeline.clips.begin(), snapshot.timeline.clips.end(),
                                               [&](const auto& value) { return value.id == decoder.clip; });
            if (decoder.engine && SameAudioSource(decoder, contribution.asset, stream) &&
                (previous == snapshot.timeline.clips.end() || AudioSampleIndex(previous->end_us()) <= at_sample)) {
                if (!ValidateSourceAccess(*source, error))
                    return nullptr;
                decoder.clip = contribution.clip;
                decoder.asset = contribution.asset;
                decoder.prepared_samples.clear();
                ++audio_measurements.cursor_reuses;
                return &decoder;
            }
        }
        if (audio_decoders.size() >= kMaxAudioDecoders) {
            const auto expired = std::find_if(audio_decoders.begin(), audio_decoders.end(), [&](const auto& decoder) {
                const auto clip = std::find_if(snapshot.timeline.clips.begin(), snapshot.timeline.clips.end(),
                                               [&](const auto& value) { return value.id == decoder.clip; });
                return clip == snapshot.timeline.clips.end() || AudioSampleIndex(clip->end_us()) <= at_sample;
            });
            if (expired != audio_decoders.end()) {
                Retire(*expired, true);
                audio_decoders.erase(expired);
            }
        }
        if (audio_decoders.size() >= kMaxAudioDecoders) {
            const auto future = std::find_if(audio_decoders.begin(), audio_decoders.end(), [&](const auto& decoder) {
                const auto clip = std::find_if(snapshot.timeline.clips.begin(), snapshot.timeline.clips.end(),
                                               [&](const auto& value) { return value.id == decoder.clip; });
                return clip != snapshot.timeline.clips.end() && AudioSampleIndex(clip->start_us) > at_sample;
            });
            if (future != audio_decoders.end()) {
                Retire(*future, true);
                audio_decoders.erase(future);
            }
        }
        if (audio_decoders.size() >= kMaxAudioDecoders) {
            error = EXOSNAP_TRANSLATABLE("EditRender", "Too many simultaneous timeline audio sources.");
            return nullptr;
        }
        auto& decoder = audio_decoders.emplace_back();
        decoder.clip = contribution.clip;
        decoder.asset = contribution.asset;
        decoder.stream = stream;
        decoder.engine = std::make_unique<EditPlayerEngine>();
        const bool opened =
            ValidateSourceAccess(*source, error) &&
            (source->access ? (stream ? decoder.engine->OpenAudioStream(source->access, *stream, error, cancelled)
                                      : decoder.engine->Open(source->access, error, cancelled))
                            : (stream ? decoder.engine->OpenAudioStream(source->path, *stream, error, cancelled)
                                      : decoder.engine->Open(source->path, error, cancelled)));
        if (!opened || (!stream && source->monitor_selection &&
                        !decoder.engine->SetMonitorSelection(*source->monitor_selection, error, cancelled))) {
            audio_decoders.pop_back();
            return nullptr;
        }
        ++audio_measurements.decoder_opens;
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

bool EditTimelineReader::Open(TimelineRenderSnapshot snapshot, const EditRenderPlan& plan, std::string& error,
                              const std::atomic<bool>* cancelled) {
    if (plan.path != EditExportPath::Render) {
        error = plan.reason;
        return false;
    }
    impl_ = std::make_unique<Impl>();
    impl_->snapshot = std::move(snapshot);
    impl_->plan = plan;
    impl_->cancelled = cancelled;
    impl_->RefreshSourceTimeline();
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
        const bool held_source = source_before->access && source_before->access == source_after->access;
        const bool conservative_source = !source_before->access && !source_after->access && a.content_identity_valid &&
                                         b.content_identity_valid && a.file_identity == b.file_identity &&
                                         a.file_size == b.file_size && a.content_sha256 == b.content_sha256;
        return source_before->path == source_after->path && (held_source || conservative_source) &&
               (!audio || same_monitor(source_before->monitor_selection, source_after->monitor_selection));
    };
    for (const auto& source : snapshot.sources)
        if (!impl_->ValidateSourceAccess(source, error))
            return false;
    for (auto& decoder : impl_->video_decoders) {
        if (!decoder.engine)
            continue;
        const auto find_source = [&](const auto& sources) {
            return std::find_if(sources.begin(), sources.end(),
                                [&](const auto& source) { return source.id == decoder.asset; });
        };
        const auto before = find_source(impl_->snapshot.sources);
        const auto after = find_source(snapshot.sources);
        bool source_retained = before != impl_->snapshot.sources.end() && after != snapshot.sources.end();
        if (source_retained) {
            const auto& a = before->metadata;
            const auto& b = after->metadata;
            source_retained =
                before->path == after->path && ((before->access && before->access == after->access) ||
                                                (!before->access && !after->access && a.content_identity_valid &&
                                                 b.content_identity_valid && a.file_identity == b.file_identity &&
                                                 a.file_size == b.file_size && a.content_sha256 == b.content_sha256));
        }
        const auto old_clip = std::find_if(impl_->snapshot.timeline.clips.begin(), impl_->snapshot.timeline.clips.end(),
                                           [&](const auto& clip) { return clip.id == decoder.clip; });
        const auto same_stream =
            old_clip != impl_->snapshot.timeline.clips.end() &&
            std::any_of(snapshot.timeline.clips.begin(), snapshot.timeline.clips.end(), [&](const auto& clip) {
                return !clip.audio && clip.asset == decoder.asset && clip.stream == old_clip->stream;
            });
        if (!source_retained || !same_stream)
            impl_->Retire(decoder, false);
        else if (!compatible(decoder, false))
            decoder.video_reposition = true;
    }
    for (auto& decoder : impl_->audio_decoders)
        if (decoder.engine && !compatible(decoder, true))
            impl_->Retire(decoder, true);
    std::erase_if(impl_->audio_decoders, [](const auto& decoder) { return !decoder.engine; });
    impl_->snapshot = std::move(snapshot);
    impl_->plan = plan;
    impl_->RefreshSourceTimeline();
    return true;
}

std::optional<AudioMetadataReadResult> EditTimelineReader::VideoAudioMetadata(uint64_t asset) const {
    for (const auto& decoder : impl_->video_decoders) {
        const auto clip = std::find_if(impl_->snapshot.timeline.clips.begin(), impl_->snapshot.timeline.clips.end(),
                                       [&](const auto& value) { return value.id == decoder.clip; });
        if (decoder.engine && clip != impl_->snapshot.timeline.clips.end() && clip->asset == asset)
            return decoder.engine->AudioMetadata();
    }
    return std::nullopt;
}

uint64_t EditTimelineReader::VideoDecoderOpens() const noexcept {
    return impl_->video_decoder_opens;
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

TimelineAudioReadMeasurements EditTimelineReader::AudioReadStats() const noexcept {
    auto result = impl_->audio_measurements;
    for (const auto& decoder : impl_->audio_decoders) {
        if (!decoder.engine)
            continue;
        result.seeks += decoder.engine->AudioSeekCount();
        result.decoded_frames += decoder.engine->AudioDecodedFrameCount();
    }
    return result;
}
bool EditTimelineReader::VideoAt(int64_t time_us, TimelineVideoFrame& output, std::string& error) {
    return VideoAt(time_us, output, error, VideoReadMode::Sequential, 0);
}

bool EditTimelineReader::VideoAt(int64_t time_us, TimelineVideoFrame& output, std::string& error, VideoReadMode mode,
                                 uint64_t generation, const std::atomic<bool>* cancelled,
                                 const VideoOpenAdmission& before_open) {
    struct RestoreCancellation {
        const std::atomic<bool>*& target;
        const std::atomic<bool>* previous;
        const VideoOpenAdmission*& admission;
        const VideoOpenAdmission* previous_admission;
        ~RestoreCancellation() {
            target = previous;
            admission = previous_admission;
        }
    } restore{impl_->cancelled, impl_->cancelled, impl_->before_video_open, impl_->before_video_open};
    if (cancelled)
        impl_->cancelled = cancelled;
    impl_->before_video_open = &before_open;
    if (impl_->cancelled && impl_->cancelled->load(std::memory_order_relaxed)) {
        error = "Source video read cancelled.";
        return false;
    }
    const auto evaluation = EvaluateTimeline(impl_->snapshot.timeline, time_us);
    if (!evaluation.error.empty()) {
        error = evaluation.error;
        return false;
    }
    if (!impl_->Prepare(evaluation.video, error))
        return false;
    output = {time_us, impl_->plan.output, {}};
    // Evaluation lists layers back to front; the compositor draws a higher layer
    // index in front.
    for (uint32_t layer = 0; layer < evaluation.video_layers.size(); ++layer)
        for (const auto& contribution : evaluation.video_layers[layer].contributions) {
            auto& decoder =
                *std::find_if(impl_->video_decoders.begin(), impl_->video_decoders.end(),
                              [&](const auto& value) { return value.clip == contribution.clip && value.engine; });
            auto frame = decoder.engine->ReadFrameAtRaw(contribution.source_us,
                                                        decoder.video_reposition ? VideoReadMode::RandomAccess : mode,
                                                        generation, impl_->cancelled);
            decoder.video_reposition = false;
            if (!frame) {
                error = "Cannot decode a timeline video frame.";
                return false;
            }
            output.layers.push_back({std::move(*frame), contribution.weight, contribution.opacity, layer});
        }
    return true;
}

bool EditTimelineReader::ConfirmMonitorSelection(uint64_t asset, int64_t time_us,
                                                 const MonitorSelectionSnapshot& selection,
                                                 AudioMetadataReadResult& metadata, std::string& error,
                                                 const std::atomic<bool>* cancelled,
                                                 const VideoOpenAdmission& before_open) {
    struct Restore {
        Impl& impl;
        const std::atomic<bool>* cancelled = nullptr;
        const VideoOpenAdmission* admission = nullptr;
        ~Restore() {
            impl.cancelled = cancelled;
            impl.before_video_open = admission;
        }
    } restore{*impl_, impl_->cancelled, impl_->before_video_open};
    impl_->cancelled = cancelled;
    impl_->before_video_open = &before_open;
    if (cancelled && cancelled->load()) {
        error = EXOSNAP_TRANSLATABLE("EditRender", "Audio selection cancelled.");
        return false;
    }
    const auto evaluation = EvaluateTimeline(impl_->snapshot.timeline, time_us);
    if (!evaluation.error.empty()) {
        error = evaluation.error;
        return false;
    }
    const auto contribution = std::find_if(evaluation.video.begin(), evaluation.video.end(),
                                           [&](const auto& value) { return value.asset == asset; });
    if (contribution == evaluation.video.end()) {
        error = EXOSNAP_TRANSLATABLE("EditRender", "The monitoring source is no longer visible.");
        return false;
    }
    if (!impl_->Prepare(evaluation.video, error))
        return false;
    auto* engine = impl_->Find(contribution->clip);
    if (!engine || !engine->SetMonitorSelection(selection, error, cancelled))
        return false;
    metadata = engine->AudioMetadata();
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
            const auto source_sample = AudioSourceSample(contribution, from);
            if (!source_sample) {
                error = "The source audio position is outside the supported range.";
                return false;
            }
            const auto length = static_cast<uint32_t>(to - from);
            const bool decoded = impl_->ReadSourceAudio(*decoder, *source_sample, length, source, error);
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
    impl_->limiter.Process(output.data(), count);
    return true;
}

bool EditTimelineReader::SourceAudioAt(int64_t first_sample, uint32_t count, std::vector<TimelineSourceAudio>& output,
                                       std::string& error) {
    output.clear();
    if (first_sample < 0 || count > 48000 || first_sample > (std::numeric_limits<int64_t>::max)() / 1'000'000 - 48000) {
        error = EXOSNAP_TRANSLATABLE("EditRender", "Invalid source audio block.");
        return false;
    }
    if (count == 0)
        return true;
    const int64_t last_sample = first_sample + count;
    const auto evaluation = EvaluateAudioIntervals(impl_->source_timeline, first_sample * 1'000'000 / 48000,
                                                   (last_sample * 1'000'000 + 47999) / 48000);
    if (!evaluation.error.empty()) {
        error = evaluation.error;
        return false;
    }
    std::vector<float> decoded;
    for (const auto& interval : evaluation.intervals) {
        const auto begin = (std::max)(first_sample, AudioSampleIndex(interval.in_us));
        const auto end = (std::min)(last_sample, AudioSampleIndex(interval.out_us));
        for (const auto& contribution : interval.contributions) {
            if (impl_->cancelled && impl_->cancelled->load(std::memory_order_relaxed)) {
                error = EXOSNAP_TRANSLATABLE("EditRender", "Source audio preparation cancelled.");
                return false;
            }
            const auto from = (std::max)(begin, AudioSampleIndex(contribution.clip_start_us));
            const auto to = (std::min)(end, AudioSampleIndex(contribution.clip_start_us + contribution.source_out_us -
                                                             contribution.source_in_us));
            if (to <= from)
                continue;
            auto source = std::find_if(output.begin(), output.end(),
                                       [&](const auto& value) { return value.clip == contribution.clip; });
            if (source == output.end()) {
                if (output.size() >= Impl::kMaxAudioDecoders) {
                    error = EXOSNAP_TRANSLATABLE("EditRender", "Too many simultaneous timeline audio sources.");
                    return false;
                }
                output.push_back({contribution.clip, contribution.asset, contribution.track, first_sample,
                                  std::vector<float>(static_cast<size_t>(count) * 2)});
                source = std::prev(output.end());
            }
            if (impl_->UnresolvedLegacyAudio(contribution))
                continue;
            auto* decoder = impl_->AudioDecoder(contribution, from, error);
            const auto source_sample = AudioSourceSample(contribution, from);
            if (!decoder || !source_sample) {
                if (error.empty())
                    error = EXOSNAP_TRANSLATABLE("EditRender", "Invalid source audio position.");
                return false;
            }
            const auto length = static_cast<uint32_t>(to - from);
            const bool success = impl_->ReadSourceAudio(*decoder, *source_sample, length, decoded, error);
            if (!success)
                return false;
            std::copy(decoded.begin(), decoded.end(),
                      source->interleaved_stereo.begin() + static_cast<ptrdiff_t>((from - first_sample) * 2));
        }
    }
    return true;
}

bool EditTimelineReader::PrepareAudioLookahead(int64_t first_sample, int64_t through_sample, std::string& error,
                                               uint64_t* deferred) {
    if (deferred)
        *deferred = 0;
    if (first_sample < 0 || through_sample < first_sample ||
        through_sample > (std::numeric_limits<int64_t>::max)() / 1'000'000) {
        error = EXOSNAP_TRANSLATABLE("EditRender", "Invalid audio lookahead interval.");
        return false;
    }
    through_sample = (std::min)(through_sample, first_sample + 96000);
    const auto evaluation = EvaluateAudioIntervals(impl_->source_timeline, first_sample * 1'000'000 / 48000,
                                                   through_sample * 1'000'000 / 48000);
    if (!evaluation.error.empty()) {
        error = evaluation.error;
        return false;
    }
    struct PlannedCursor {
        uint64_t asset = 0;
        std::optional<TimelineStreamRef> stream;
        int64_t end_us = 0;
        int64_t source_end_us = 0;
    };
    std::vector<PlannedCursor> planned;
    for (const auto& decoder : impl_->audio_decoders) {
        const auto clip = std::find_if(impl_->snapshot.timeline.clips.begin(), impl_->snapshot.timeline.clips.end(),
                                       [&](const auto& value) { return value.id == decoder.clip; });
        if (clip != impl_->snapshot.timeline.clips.end())
            planned.push_back({clip->asset, clip->stream, clip->end_us(), clip->source_out_us});
    }
    std::set<uint64_t> visited;
    for (const auto& interval : evaluation.intervals)
        for (const auto& contribution : interval.contributions) {
            if (impl_->cancelled && impl_->cancelled->load(std::memory_order_relaxed))
                return false;
            if (!visited.insert(contribution.clip).second || impl_->UnresolvedLegacyAudio(contribution))
                continue;
            if (std::any_of(impl_->audio_decoders.begin(), impl_->audio_decoders.end(),
                            [&](const auto& decoder) { return decoder.clip == contribution.clip; }))
                continue;
            const auto end_us = contribution.clip_start_us + contribution.source_out_us - contribution.source_in_us;
            const auto continuous = std::find_if(planned.begin(), planned.end(), [&](const auto& cursor) {
                return cursor.asset == contribution.asset && cursor.stream == contribution.stream &&
                       cursor.end_us == contribution.clip_start_us && cursor.source_end_us == contribution.source_in_us;
            });
            if (continuous != planned.end()) {
                continuous->end_us = end_us;
                continuous->source_end_us = contribution.source_out_us;
                continue;
            }
            if (impl_->audio_decoders.size() >= Impl::kMaxAudioDecoders) {
                if (deferred)
                    ++*deferred;
                continue;
            }
            auto* decoder = impl_->AudioDecoder(contribution, first_sample, error);
            if (!decoder)
                return false;
            const auto from = (std::max)(first_sample, AudioSampleIndex(contribution.clip_start_us));
            const auto to = (std::min)(AudioSampleIndex(end_us), from + 480);
            const auto source_sample = AudioSourceSample(contribution, from);
            if (!source_sample || to <= from) {
                error = EXOSNAP_TRANSLATABLE("EditRender", "Invalid future audio source position.");
                return false;
            }
            std::vector<float> prepared;
            if (!impl_->ReadSourceAudio(*decoder, *source_sample, static_cast<uint32_t>(to - from), prepared, error))
                return false;
            decoder->prepared_first_sample = *source_sample;
            decoder->prepared_samples = std::move(prepared);
            ++impl_->audio_measurements.prepared_source_ranges;
            planned.push_back({contribution.asset, contribution.stream, end_us, contribution.source_out_us});
        }
    return true;
}
void EditTimelineReader::ResetAudio() {
    impl_->limiter.Reset();
}
size_t EditTimelineReader::PeakDecoders() const {
    return impl_->peak_decoders;
}

} // namespace exosnap::engine
