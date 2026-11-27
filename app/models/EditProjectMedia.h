#pragma once

#include "models/EditSourceImport.h"

#include <map>

namespace exosnap::edit {

// The caller has proved the original content hash. Foreign audio identities are
// per-open: bind the probe's runtime tables to the IDs already used by clips,
// mix choices and original recipe anchors. Manifest identities remain strict.
inline bool BindProjectStreamLayout(const ValidatedSourceDescriptor& saved, const ValidatedSourceDescriptor& observed,
                                    engine::EditMediaMetadata& metadata) {
    if (saved.streams.size() != observed.streams.size() ||
        (saved.audio_identities_verified && !observed.audio_identities_verified))
        return false;
    std::map<engine::AudioTrackId, engine::AudioTrackId> bindings;
    for (const auto& stream : saved.streams) {
        const auto found = std::find_if(observed.streams.begin(), observed.streams.end(), [&](const auto& value) {
            return stream.ref.container_index == value.ref.container_index && stream.ref.kind == value.ref.kind;
        });
        if (found == observed.streams.end() || stream.source_span.in_us < found->source_span.in_us ||
            stream.source_span.out_us > found->source_span.out_us ||
            stream.timeline_offset_us != found->timeline_offset_us || stream.fps != found->fps ||
            stream.codec.codec_id != found->codec.codec_id ||
            stream.codec.codec_private != found->codec.codec_private ||
            stream.codec.channels != found->codec.channels || stream.codec.bit_depth != found->codec.bit_depth ||
            stream.codec.codec_delay_samples != found->codec.codec_delay_samples ||
            stream.codec.channel_layout != found->codec.channel_layout ||
            stream.codec.sample_rate != found->codec.sample_rate)
            return false;
        if (stream.ref.stream_identity != found->ref.stream_identity) {
            if (observed.audio_identities_verified || stream.ref.kind != engine::TimelineStreamKind::Audio)
                return false;
            bindings.emplace(found->ref.stream_identity, stream.ref.stream_identity);
        }
    }
    if (bindings.empty())
        return true;
    const auto bind = [&](engine::AudioTrackId& id) {
        if (const auto it = bindings.find(id); it != bindings.end())
            id = it->second;
    };
    for (auto& stream : metadata.streams)
        if (stream.kind == engine::TimelineStreamKind::Audio && stream.stream_identity)
            bind(*stream.stream_identity);
    for (auto& track : metadata.audio_metadata.actual_tracks)
        bind(track.track_id);
    if (metadata.audio_metadata.unverified_asset_binding) {
        auto bound =
            std::make_shared<engine::UnverifiedAudioAssetBinding>(*metadata.audio_metadata.unverified_asset_binding);
        for (auto& track : bound->actual_tracks)
            bind(track.track_id);
        metadata.audio_metadata.unverified_asset_binding = std::move(bound);
    }
    return true;
}

// Resolve the still-pending import decision in a detached save/load snapshot.
// The ordinary importer uses the same clip rebase, preserving authored overrides.
inline void FinalizeProjectRecordedDefault(Asset& asset, std::vector<Clip>& clips, const SourceImport& imported) {
    if (!asset.mix_decision_required || !asset.source_descriptor || !imported.descriptor ||
        !imported.descriptor->audio_identities_verified || !imported.input_mix || imported.mix_decision_required ||
        imported.input_mix->authority != InputMixAuthority::VerifiedRecordedDefault ||
        imported.descriptor->asset_identity_fingerprint != asset.source_descriptor->asset_identity_fingerprint ||
        imported.descriptor->streams.size() != asset.source_descriptor->streams.size())
        return;
    for (size_t i = 0; i < imported.descriptor->streams.size(); ++i)
        if (imported.descriptor->streams[i].ref != asset.source_descriptor->streams[i].ref)
            return;
    auto descriptor = std::make_shared<ValidatedSourceDescriptor>(*asset.source_descriptor);
    descriptor->audio_identities_verified = true;
    asset.source_descriptor = std::move(descriptor);
    asset.input_mix_selection = imported.input_mix;
    asset.mix_decision_required = false;
    RebaseImportedAudioDefaults(clips, asset.id, *asset.input_mix_selection);
}

} // namespace exosnap::edit
