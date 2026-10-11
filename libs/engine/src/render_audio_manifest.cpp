#include "render_audio_manifest.h"

#include "media_identity_sidecar.h"

#include <capability/translatable.h>

#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/edit_player_engine.h>
#include <exosnap/engine/edit_timeline_render.h>
#include <exosnap/engine/recording_media_types.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace exosnap::engine {
namespace {

EngineError RenderMetadataError(std::string context) {
    EngineError error;
    error.code = EngineErrorCode::SourceIntegrityFailure;
    error.owner = EngineModule::Editor;
    error.context = std::move(context);
    return error;
}

template <typename Id> void AppendUnique(std::vector<Id>& ids, Id id) {
    if (std::find(ids.begin(), ids.end(), id) == ids.end())
        ids.push_back(id);
}

} // namespace

Outcome<AudioManifest1> BuildRenderAudioManifest(const std::vector<MonitorSelectionSnapshot>& validated_selections) {
    AudioManifest1 manifest;
    manifest.media_id = NewId<MediaId>();
    manifest.recording_id = NewId<RecordingId>();
    manifest.segment_id = NewId<SegmentId>();
    manifest.monitor_selection_origin = MonitorSelectionOrigin::User;
    AudioManifestTrack mix;
    mix.container.track_id = NewId<AudioTrackId>();
    mix.container.stream_index = {0};
    mix.container.track_uid = 2;
    mix.container.name = EXOSNAP_TRANSLATABLE("EditRender", "Rendered Mix");
    mix.container.role = "mix";
    mix.container.codec = {"A_PCM/INT/LIT", {}, 48000, 2, 24, 0, "stereo"};
    for (const auto& selection : validated_selections) {
        AppendUnique(manifest.derived_from_media_ids, selection.media_id);
        for (const auto& track : selection.tracks)
            AppendUnique(mix.derived_from_track_ids, track.track_id);
    }
    if (!mix.derived_from_track_ids.empty()) {
        manifest.default_monitor_track_ids = {mix.container.track_id};
        manifest.tracks.push_back(std::move(mix));
    }
    const auto valid = ValidateAudioManifest1(manifest);
    if (!valid.succeeded())
        return *valid.error();
    return manifest;
}

Outcome<RenderAudioPreflight> PrepareRenderAudioManifest(const TimelineRenderSnapshot& snapshot) {
    RenderAudioPreflight preflight;
    std::set<uint64_t> referenced;
    std::set<uint64_t> audio_assets;
    for (const auto& clip : snapshot.timeline.clips) {
        referenced.insert(clip.asset);
        if (clip.audio)
            audio_assets.insert(clip.asset);
    }
    std::vector<MonitorSelectionSnapshot> contributing;
    for (const auto asset : referenced) {
        const auto source = std::find_if(snapshot.sources.begin(), snapshot.sources.end(),
                                         [asset](const auto& value) { return value.id == asset; });
        if (source == snapshot.sources.end() ||
            std::count_if(snapshot.sources.begin(), snapshot.sources.end(),
                          [asset](const auto& value) { return value.id == asset; }) != 1)
            return RenderMetadataError(
                EXOSNAP_TRANSLATABLE("EditRender", "Render source asset is missing or ambiguous."));
        auto lease = AcquireMediaReadLease(source->path);
        if (!lease.succeeded())
            return *lease.error();
        preflight.source_leases.push_back(std::move(*lease.value()));
        EditPlayerEngine player;
        std::string error;
        if (!player.Open(source->path, error))
            return RenderMetadataError(
                std::string(EXOSNAP_TRANSLATABLE("EditRender", "Cannot validate render source: ")) + error);
        if (!source->monitor_selection)
            return RenderMetadataError(
                EXOSNAP_TRANSLATABLE("EditRender", "Render source requires an explicit frozen monitoring selection."));
        if (!player.SetMonitorSelection(*source->monitor_selection, error))
            return RenderMetadataError(std::string(EXOSNAP_TRANSLATABLE(
                                           "EditRender", "Cannot validate frozen render monitoring selection: ")) +
                                       error);
        if (player.AudioMetadata().monitoring_confirmation_required)
            return RenderMetadataError(
                EXOSNAP_TRANSLATABLE("EditRender", "Render monitoring selection requires explicit confirmation."));
        if (!audio_assets.contains(asset) && !source->monitor_selection->tracks.empty())
            return RenderMetadataError(
                EXOSNAP_TRANSLATABLE("EditRender", "Video-only render requires an empty frozen monitoring selection."));
        if (audio_assets.contains(asset))
            contributing.push_back(*source->monitor_selection);
    }
    auto manifest = BuildRenderAudioManifest(contributing);
    if (!manifest.succeeded())
        return *manifest.error();
    preflight.manifest = std::move(*manifest.value());
    return preflight;
}

} // namespace exosnap::engine
