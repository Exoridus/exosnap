#include <exosnap/engine/edit_export_plan.h>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include <set>

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
} // namespace exosnap::engine
