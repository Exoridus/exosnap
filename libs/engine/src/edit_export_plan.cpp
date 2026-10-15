#include <exosnap/engine/edit_export_plan.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/output_geometry.h>

#include "ffmpeg_aac_encoder.h"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include <algorithm>
#include <bit>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <map>
#include <new>
#include <set>
#include <system_error>

namespace exosnap::engine {
namespace {
class PlanDigest {
  public:
    PlanDigest() : sha_(av_sha_alloc()) {
        if (!sha_)
            throw std::bad_alloc();
        av_sha_init(sha_, 256);
    }
    ~PlanDigest() {
        av_free(sha_);
    }
    void number(uint64_t value) {
        uint8_t bytes[8];
        for (unsigned i = 0; i < 8; ++i)
            bytes[i] = static_cast<uint8_t>(value >> (i * 8));
        av_sha_update(sha_, bytes, sizeof(bytes));
    }
    void real(double value) {
        number(std::bit_cast<uint64_t>(value));
    }
    void bytes(const auto& value) {
        number(value.size());
        av_sha_update(sha_, reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
    void stream(const TimelineStreamRef& value) {
        number(value.asset_id);
        bytes(value.stream_identity.bytes);
        number(value.container_index);
        number(static_cast<uint64_t>(value.kind));
    }
    void curve(const CurveSpec& value) {
        number(static_cast<uint64_t>(value.kind));
        real(value.bias);
    }
    EditPlanHash finish() {
        EditPlanHash result{};
        av_sha_final(sha_, result.data());
        return result;
    }

  private:
    AVSHA* sha_;
};

bool Effect(const TimelineClip& clip) {
    return clip.enabled != clip.original_enabled || clip.gain_db != 0 || clip.opacity != 1 ||
           clip.fade_in.duration_us != 0 || clip.fade_out.duration_us != 0;
}

bool OriginalRecipe(const EditExportPlan& plan) {
    const auto& timeline = plan.snapshot.timeline;
    if (!timeline.crossfades.empty() || !timeline.junctions.empty())
        return false;
    if (std::any_of(timeline.tracks.begin(), timeline.tracks.end(),
                    [](const auto& t) { return !t.enabled || t.gain_db != 0; }))
        return false;
    for (const auto& clip : timeline.clips) {
        if (Effect(clip) || !clip.group)
            return false;
        const auto group = std::find_if(plan.facts.original_groups.begin(), plan.facts.original_groups.end(),
                                        [&](const auto& g) { return g.group == clip.group; });
        if (group == plan.facts.original_groups.end() || group->streams.empty())
            return false;
        for (const auto& stream : group->streams) {
            const auto count = std::count_if(timeline.clips.begin(), timeline.clips.end(), [&](const auto& other) {
                return other.group == clip.group && other.stream == stream && other.asset == clip.asset &&
                       other.start_us == clip.start_us && other.source_in_us == clip.source_in_us &&
                       other.source_out_us == clip.source_out_us;
            });
            if (count != 1)
                return false;
        }
        if (!clip.stream ||
            std::find(group->streams.begin(), group->streams.end(), *clip.stream) == group->streams.end())
            return false;
    }
    return true;
}

EditPlanHash Hash(const EditExportPlan& p) {
    PlanDigest h;
    h.number(1);
    const auto& i = p.intent;
    h.number(static_cast<uint64_t>(i.mode));
    h.number(static_cast<uint64_t>(i.span));
    h.number(static_cast<uint64_t>(i.container));
    h.number(static_cast<uint64_t>(i.video_profile));
    h.number(static_cast<uint64_t>(i.audio_profile));
    h.number(i.geometry.width);
    h.number(i.geometry.height);
    h.number(i.fps_num);
    h.number(i.fps_den);
    h.number(i.quality);
    h.bytes(i.destination.generic_u8string());
    h.number(i.replace_destination);
    h.number(p.revision);
    h.number(p.span_in_us);
    h.number(p.span_out_us);
    h.number(static_cast<uint64_t>(p.video_path));
    h.number(static_cast<uint64_t>(p.audio_path));
    h.number(p.output_audio_streams);
    h.number(p.facts.video_copy_qualified);
    h.number(p.facts.original_copy_qualified);
    h.number(p.facts.stereo_copy_qualified);
    h.number(p.facts.video_render_available);
    h.number(p.facts.stereo_render_available);
    h.number(p.facts.mp4_finished_available);
    h.number(p.facts.input_mix_validated);
    h.number(p.facts.destination_admitted);
    h.number(p.facts.original_groups.size());
    for (const auto& g : p.facts.original_groups) {
        h.number(g.group);
        h.number(g.streams.size());
        for (const auto& s : g.streams)
            h.stream(s);
    }
    h.number(p.facts.copy_recipe.size());
    for (const auto& r : p.facts.copy_recipe) {
        h.bytes(r.source.generic_u8string());
        h.number(r.source_in_us);
        h.number(r.source_out_us);
        h.number(r.timeline_start_us);
    }
    const auto& t = p.snapshot.timeline;
    h.number(t.clips.size());
    for (const auto& c : t.clips) {
        h.number(c.id);
        h.number(c.asset);
        h.number(c.group);
        h.number(c.track);
        h.number(c.audio);
        h.number(c.start_us);
        h.number(c.source_in_us);
        h.number(c.source_out_us);
        h.number(c.enabled);
        h.number(c.original_enabled);
        h.real(c.gain_db);
        h.real(c.opacity);
        h.number(c.stream.has_value());
        if (c.stream)
            h.stream(*c.stream);
        h.number(c.fade_in.duration_us);
        h.curve(c.fade_in.curve);
        h.number(c.fade_out.duration_us);
        h.curve(c.fade_out.curve);
    }
    h.number(t.tracks.size());
    for (const auto& track : t.tracks) {
        h.number(track.id);
        h.number(track.audio);
        h.number(track.enabled);
        h.real(track.gain_db);
    }
    h.number(t.junctions.size());
    for (const auto& j : t.junctions) {
        h.number(j.id);
        h.number(j.track);
        h.number(j.outgoing);
        h.number(j.incoming);
        h.number(static_cast<uint64_t>(j.kind));
        h.number(j.in_us);
        h.number(j.out_us);
        h.number(j.cut_us);
        h.curve(j.curve);
        h.number(static_cast<uint64_t>(j.audio_law));
        h.number(static_cast<uint64_t>(j.anchor));
    }
    h.number(t.crossfades.size());
    for (const auto& j : t.crossfades) {
        h.number(j.outgoing);
        h.number(j.incoming);
        h.number(j.duration_us);
    }
    h.number(t.output_width);
    h.number(t.output_height);
    h.number(t.output_fps_num);
    h.number(t.output_fps_den);
    h.number(p.snapshot.sources.size());
    for (const auto& s : p.snapshot.sources) {
        h.number(s.id);
        h.bytes(s.path.generic_u8string());
        h.number(s.metadata.file_identity.volume_serial);
        h.bytes(s.metadata.file_identity.file_id);
        h.number(s.metadata.file_size);
        h.bytes(s.metadata.content_sha256);
    }
    h.number(p.facts.deviations.size());
    for (const auto& e : p.facts.deviations) {
        h.number(e.clip);
        h.number(e.requested_in_us);
        h.number(e.requested_out_us);
        h.number(e.effective_in_us);
        h.number(e.effective_out_us);
        h.number(e.exposes_removed_content);
    }
    return h.finish();
}
} // namespace

std::shared_ptr<const EditExportPlan> PrepareEditExport(TimelineRenderSnapshot snapshot, EditExportIntent intent,
                                                        EditExportFacts facts) {
    auto p = std::make_shared<EditExportPlan>();
    p->snapshot = std::move(snapshot);
    p->intent = std::move(intent);
    p->facts = std::move(facts);
    for (auto& source : p->snapshot.sources)
        source.monitor_selection.reset();
    const auto& t = p->snapshot.timeline;
    p->revision = t.revision;
    p->span_in_us = p->intent.span == EditExportSpan::ResultRange ? t.result_in_us : 0;
    p->span_out_us =
        p->intent.span == EditExportSpan::ResultRange && t.result_out_us >= 0 ? t.result_out_us : t.duration_us;
    const auto issue = [&](EditExportIssueCode code, EditExportIssueScope scope, uint64_t id = 0) {
        p->issues.push_back({code, scope, id ? std::vector<uint64_t>{id} : std::vector<uint64_t>{}});
    };
    if (p->span_in_us < 0 || p->span_out_us <= p->span_in_us || p->span_out_us > t.duration_us)
        issue(EditExportIssueCode::EmptySpan, EditExportIssueScope::Timeline);
    if (!p->facts.destination_admitted)
        issue(EditExportIssueCode::DestinationAliasesSource, EditExportIssueScope::Destination);
    bool video_effect = !t.crossfades.empty();
    bool has_audio = false;
    bool audio_effect = false;
    size_t audio_clips = 0;
    for (const auto& c : t.clips) {
        const auto s = std::find_if(p->snapshot.sources.begin(), p->snapshot.sources.end(),
                                    [&](const auto& s) { return s.id == c.asset; });
        if (s == p->snapshot.sources.end() || !s->metadata.error.empty()) {
            issue(EditExportIssueCode::MissingSource, EditExportIssueScope::Source, c.asset);
            continue;
        }
        if (!s->metadata.content_identity_valid)
            issue(EditExportIssueCode::SourceChanged, EditExportIssueScope::Source, c.asset);
        if (c.start_us < 0 || c.source_in_us < 0 || c.source_out_us <= c.source_in_us ||
            c.source_out_us > s->metadata.duration_us || !std::isfinite(c.gain_db) || !std::isfinite(c.opacity))
            issue(EditExportIssueCode::InvalidTimeline, EditExportIssueScope::Timeline, c.id);
        const auto track =
            std::find_if(t.tracks.begin(), t.tracks.end(), [&](const auto& track) { return track.id == c.track; });
        const bool enabled = c.enabled && (track == t.tracks.end() || track->enabled);
        if (c.audio && enabled && c.start_us < p->span_out_us && c.end_us() > p->span_in_us) {
            has_audio = true;
            ++audio_clips;
            audio_effect = audio_effect || Effect(c) || (track != t.tracks.end() && track->gain_db != 0);
            if (!c.stream || c.stream->kind != TimelineStreamKind::Audio)
                issue(EditExportIssueCode::MixSelectionRequired, EditExportIssueScope::Audio, c.id);
        }
        if (!c.audio && (!enabled || Effect(c)))
            video_effect = true;
        if (!c.audio && p->facts.video_render_available && !s->metadata.render_color_supported)
            p->facts.video_render_available = false;
    }
    for (const auto& j : t.junctions) {
        const auto track =
            std::find_if(t.tracks.begin(), t.tracks.end(), [&](const auto& track) { return track.id == j.track; });
        if (track == t.tracks.end() || !track->audio)
            video_effect = true;
        else
            audio_effect = true;
    }
    if (p->intent.mode == EditExportMode::OriginalStreams) {
        if (!p->facts.original_copy_qualified || !OriginalRecipe(*p)) {
            issue(EditExportIssueCode::OriginalRecipeChanged, EditExportIssueScope::Timeline);
        } else {
            p->video_path = EditVideoPath::Copy;
            p->audio_path = EditAudioPath::CopyOriginalStreams;
            for (const auto& s : p->snapshot.sources)
                p->output_audio_streams = (std::max)(
                    p->output_audio_streams, static_cast<uint32_t>(s.metadata.audio_metadata.actual_tracks.size()));
        }
    } else {
        p->video_path = !video_effect && !p->intent.geometry.IsValid() && !p->intent.fps_num &&
                                p->facts.video_copy_qualified &&
                                p->intent.video_profile == EditVideoProfile::MatchSourceAuto && !p->intent.quality
                            ? EditVideoPath::Copy
                            : (p->facts.video_render_available ? EditVideoPath::Render : EditVideoPath::Unsupported);
        p->audio_path =
            !has_audio
                ? EditAudioPath::Absent
                : (p->facts.stereo_copy_qualified && !audio_effect && audio_clips == 1
                       ? EditAudioPath::CopyStereo
                       : (p->facts.stereo_render_available ? EditAudioPath::RenderStereo : EditAudioPath::Unsupported));
        p->output_audio_streams = has_audio ? 1 : 0;
        if (has_audio && !p->facts.input_mix_validated)
            issue(EditExportIssueCode::MixSelectionRequired, EditExportIssueScope::Audio);
        if (p->intent.container == EditExportContainer::Mp4 && !p->facts.mp4_finished_available)
            issue(EditExportIssueCode::UnsupportedProfile, EditExportIssueScope::Video);
        if (p->video_path == EditVideoPath::Unsupported)
            issue(EditExportIssueCode::UnsupportedProfile, EditExportIssueScope::Video);
        if (p->audio_path == EditAudioPath::Unsupported)
            issue(EditExportIssueCode::UnsupportedProfile, EditExportIssueScope::Audio);
    }
    if (!p->facts.deviations.empty())
        issue(EditExportIssueCode::BoundaryConsentRequired, EditExportIssueScope::Video);
    p->hash = Hash(*p);
    return p;
}

bool AcceptsBoundaryConsent(const EditExportPlan& plan, const EditBoundaryConsent& consent) {
    return plan.hash == consent.plan_hash && plan.facts.deviations == consent.edges;
}
namespace {

std::pair<int64_t, int64_t> Span(const TimelineSnapshot& timeline, EditExportSpan span) {
    const int64_t in = span == EditExportSpan::ResultRange ? timeline.result_in_us : 0;
    const int64_t out = span == EditExportSpan::ResultRange && timeline.result_out_us >= 0 ? timeline.result_out_us
                                                                                           : timeline.duration_us;
    return {in, out};
}

bool TrackEnabled(const TimelineSnapshot& timeline, const TimelineClip& clip) {
    const auto track = std::find_if(timeline.tracks.begin(), timeline.tracks.end(),
                                    [&clip](const auto& value) { return value.id == clip.track; });
    return clip.enabled && (track == timeline.tracks.end() || track->enabled);
}

const TimelineMediaSource* SourceOf(const TimelineRenderSnapshot& snapshot, uint64_t asset) {
    const auto found = std::find_if(snapshot.sources.begin(), snapshot.sources.end(),
                                    [asset](const auto& value) { return value.id == asset; });
    return found == snapshot.sources.end() ? nullptr : &*found;
}

// Hardlinks and other aliases resolve to one file identity; an unresolvable
// comparison is treated as an alias so a source is never overwritten.
bool SameFile(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code error;
    if (std::filesystem::exists(a, error) && std::filesystem::exists(b, error)) {
        const bool same = std::filesystem::equivalent(a, b, error);
        return same || static_cast<bool>(error);
    }
    const auto left = std::filesystem::weakly_canonical(a, error);
    const auto right = std::filesystem::weakly_canonical(b, error);
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

} // namespace

EditExportFacts AssessEditExportFacts(const TimelineRenderSnapshot& snapshot, const EditExportIntent& intent,
                                      bool input_mix_validated, const std::atomic<bool>* cancelled) {
    EditExportFacts facts;
    facts.input_mix_validated = input_mix_validated;
    const auto& timeline = snapshot.timeline;
    const auto span = Span(timeline, intent.span);
    const bool whole = span.first == 0 && span.second == timeline.duration_us;
    const bool mp4 = intent.container == EditExportContainer::Mp4;
    if (mp4) {
        FfmpegAacEncoder probe;
        std::string unavailable;
        facts.mp4_finished_available = probe.Init(48000, 2, unavailable);
        probe.Shutdown();
    }
    facts.destination_admitted = !intent.destination.empty();
    for (const auto& source : snapshot.sources)
        if (facts.destination_admitted && SameFile(intent.destination, source.path))
            facts.destination_admitted = false;

    std::vector<const TimelineClip*> video;
    std::set<uint64_t> video_tracks;
    for (const auto& clip : timeline.clips)
        if (!clip.audio && TrackEnabled(timeline, clip)) {
            video.push_back(&clip);
            video_tracks.insert(clip.track);
        }
    std::sort(video.begin(), video.end(), [](const auto* a, const auto* b) { return a->start_us < b->start_us; });
    bool render_video = !video.empty();
    bool complete = !video.empty() && video_tracks.size() == 1 && whole;
    int64_t covered = 0;
    for (const auto* clip : video) {
        const auto* source = SourceOf(snapshot, clip->asset);
        if (!source) {
            render_video = false;
            complete = false;
            continue;
        }
        facts.copy_recipe.push_back({source->path, clip->source_in_us, clip->source_out_us, clip->start_us});
        const auto& metadata = source->metadata;
        if (!metadata.render_color_supported || metadata.fps_num <= 0 || metadata.fps_den <= 0 || metadata.width <= 0 ||
            metadata.height <= 0 ||
            !IsEncoderAlignedSize({static_cast<uint32_t>(metadata.width), static_cast<uint32_t>(metadata.height)}))
            render_video = false;
        complete = complete && clip->start_us == covered && clip->source_in_us == 0 &&
                   std::abs(clip->source_out_us - metadata.duration_us) <= 1000;
        covered += clip->source_out_us - clip->source_in_us;
    }
    facts.video_render_available = render_video;
    if (!facts.copy_recipe.empty() && video_tracks.size() == 1 && whole) {
        facts.original_copy_qualified = AssessTimelineExport(facts.copy_recipe, mp4, cancelled).eligibility ==
                                        TimelineExportEligibility::StreamCopy;
        facts.video_copy_qualified =
            complete && facts.original_copy_qualified && std::abs(covered - timeline.duration_us) <= 1000;
    }

    std::map<uint64_t, EditOriginalGroup> groups;
    bool render_audio = true;
    for (const auto& clip : timeline.clips) {
        if (clip.group && clip.stream) {
            auto& group = groups[clip.group];
            group.group = clip.group;
            if (std::find(group.streams.begin(), group.streams.end(), *clip.stream) == group.streams.end())
                group.streams.push_back(*clip.stream);
        }
        if (!clip.audio || !TrackEnabled(timeline, clip) || clip.start_us >= span.second || clip.end_us() <= span.first)
            continue;
        const auto* source = SourceOf(snapshot, clip.asset);
        if (!source || !clip.stream || clip.stream->kind != TimelineStreamKind::Audio) {
            render_audio = false;
            continue;
        }
        const auto stream =
            std::find_if(source->metadata.streams.begin(), source->metadata.streams.end(), [&clip](const auto& value) {
                return value.kind == TimelineStreamKind::Audio && value.container_index == clip.stream->container_index;
            });
        if (stream == source->metadata.streams.end() || stream->codec.channels == 0 || stream->codec.channels > 2)
            render_audio = false;
    }
    for (auto& entry : groups)
        facts.original_groups.push_back(std::move(entry.second));
    facts.stereo_render_available = render_audio;
    return facts;
}

namespace {

// The finished MP4 profile is written as Matroska with an AAC mix first and then
// delivered through the same faststart stream copy that recordings use.
RemuxResult ExecuteFinished(const EditExportPlan& plan, RecorderConfig config, const std::filesystem::path& staging,
                            bool aac, RemuxProgressCallback progress);

} // namespace

RemuxResult ExecuteEditExport(const EditExportPlan& plan, RecorderConfig config, const std::filesystem::path& staging,
                              RemuxProgressCallback progress) {
    if (!plan.ready())
        return RemuxResult::Fail(-1, "The export plan is not ready.");
    if (!progress)
        progress = RemuxNoopCallback();
    const bool mp4 = plan.intent.container == EditExportContainer::Mp4;
    if (plan.intent.mode == EditExportMode::OriginalStreams)
        return ExportTimelineStreamCopy(plan.facts.copy_recipe, staging, mp4, std::move(progress));
    if (!mp4)
        return ExecuteFinished(plan, std::move(config), staging, false, std::move(progress));
    if (plan.video_path == EditVideoPath::Render && config.video_codec == VideoCodec::Av1)
        return RemuxResult::Fail(-1, "AV1 rendering into MP4 is not a qualified profile.");
    auto intermediate = staging;
    intermediate += L".mix.mkv";
    std::error_code ignored;
    std::filesystem::remove(intermediate, ignored);
    auto result = ExecuteFinished(plan, std::move(config), intermediate, true,
                                  [&progress](float fraction) { return progress(fraction * 0.9f); });
    if (result.success)
        result = RemuxToProgressiveMp4(intermediate, staging,
                                       [&progress](float fraction) { return progress(0.9f + fraction * 0.1f); });
    std::filesystem::remove(intermediate, ignored);
    return result;
}

namespace {

RemuxResult ExecuteFinished(const EditExportPlan& plan, RecorderConfig config, const std::filesystem::path& staging,
                            bool aac, RemuxProgressCallback progress) {
    if (plan.video_path == EditVideoPath::Copy)
        return ExportEditTimelineWithCopiedVideo(plan.snapshot, plan.facts.copy_recipe, staging,
                                                 plan.audio_path == EditAudioPath::RenderStereo ||
                                                     plan.audio_path == EditAudioPath::CopyStereo,
                                                 aac, std::move(progress));
    if (plan.video_path != EditVideoPath::Render)
        return RemuxResult::Fail(-1, "This export path is unavailable.");
    const auto& timeline = plan.snapshot.timeline;
    const TimelineMediaSource* reference = nullptr;
    for (const auto& clip : timeline.clips)
        if (!clip.audio && !reference)
            reference = SourceOf(plan.snapshot, clip.asset);
    if (!reference)
        return RemuxResult::Fail(-1, "The timeline has no video source to render.");
    EditRenderPlan render;
    render.path = EditExportPath::Render;
    render.span_in_us = plan.span_in_us;
    render.span_out_us = plan.span_out_us;
    render.aac_audio = aac;
    render.output = plan.intent.geometry.IsValid() ? plan.intent.geometry
                                                   : FrameSize{static_cast<uint32_t>(reference->metadata.width),
                                                               static_cast<uint32_t>(reference->metadata.height)};
    if (plan.intent.fps_num) {
        render.fps_num = static_cast<int>(plan.intent.fps_num);
        render.fps_den = static_cast<int>(plan.intent.fps_den);
    } else if (timeline.output_fps_num) {
        render.fps_num = static_cast<int>(timeline.output_fps_num);
        render.fps_den = static_cast<int>(timeline.output_fps_den);
    } else {
        render.fps_num = reference->metadata.fps_num;
        render.fps_den = reference->metadata.fps_den;
    }
    return RenderEditTimeline(plan.snapshot, render, std::move(config), staging, false, std::move(progress));
}

} // namespace

} // namespace exosnap::engine
