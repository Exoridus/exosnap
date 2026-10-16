#pragma once

#include "models/EditCommandTypes.h"

#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/edit_timeline_export.h>

#include <algorithm>
#include <memory>
#include <optional>

namespace exosnap::edit {

struct SourceImport {
    // Null when the source cannot be bound to exact bytes or carries a stream
    // layout the edit model cannot represent; such a source uses the linked
    // single-audio-row import.
    std::shared_ptr<const ValidatedSourceDescriptor> descriptor;
    // Set only when the starting mix follows from the source itself. Otherwise
    // every audio stream is imported disabled and the user decides once.
    std::optional<InputMixSelection> input_mix;
    bool mix_decision_required = false;
};

namespace source_import_detail {

inline engine::AudioTrackId SyntheticIdentity(uint8_t kind, uint32_t container_index) {
    engine::AudioTrackId identity;
    identity.bytes[0] = kind;
    for (unsigned byte = 0; byte < 4; ++byte)
        identity.bytes[1 + byte] = static_cast<uint8_t>(container_index >> (byte * 8));
    return identity;
}

} // namespace source_import_detail

// Describes every video/audio stream of a probed source and resolves the
// starting mix once: a single audio stream, or a verified recorded
// default without overlapping alternatives, needs no question. Ambiguous
// provenance returns `mix_decision_required` with no selection. A positive
// `declared_duration_us` (a recording segment's own length) bounds every span.
[[nodiscard]] inline SourceImport DescribeSourceImport(const engine::EditMediaMetadata& metadata,
                                                       Time declared_duration_us = 0) {
    SourceImport result;
    if (!metadata.error.empty() || !metadata.content_identity_valid || metadata.duration_us <= 0)
        return result;
    auto descriptor = std::make_shared<ValidatedSourceDescriptor>();
    descriptor->file_identity = metadata.file_identity;
    descriptor->asset_identity_fingerprint = metadata.content_sha256;
    descriptor->audio_identities_verified = metadata.audio_metadata.status == engine::AudioMetadataStatus::Verified;
    std::vector<engine::AudioTrackId> audio;
    bool has_video = false;
    for (const auto& stream : metadata.streams) {
        const bool video = stream.kind == engine::TimelineStreamKind::Video;
        if (video && has_video)
            continue;
        if (!video && (stream.codec.channels == 0 || stream.codec.channels > 2))
            return {};
        SourceStreamDescriptor described;
        described.ref.kind = stream.kind;
        described.ref.container_index = stream.container_index;
        described.ref.stream_identity =
            !video && stream.stream_identity && *stream.stream_identity != engine::AudioTrackId{}
                ? *stream.stream_identity
                : source_import_detail::SyntheticIdentity(video ? 'V' : 'A', stream.container_index);
        described.source_span = {0, declared_duration_us > 0 ? std::min(declared_duration_us, metadata.duration_us)
                                                             : metadata.duration_us};
        described.codec = stream.codec;
        if (video) {
            has_video = true;
            described.fps = RationalFps{static_cast<uint32_t>(std::max(0, stream.fps_num)),
                                        static_cast<uint32_t>(std::max(1, stream.fps_den))}
                                .reduced();
        } else {
            audio.push_back(described.ref.stream_identity);
            const auto track = std::find_if(
                metadata.audio_metadata.actual_tracks.begin(), metadata.audio_metadata.actual_tracks.end(),
                [&](const auto& candidate) { return candidate.track_id == described.ref.stream_identity; });
            if (track != metadata.audio_metadata.actual_tracks.end()) {
                described.name = track->name;
                described.role = track->role;
                described.source_ids = track->source_ids;
            }
        }
        descriptor->streams.push_back(std::move(described));
    }
    result.descriptor = descriptor;
    if (audio.empty())
        return result;
    InputMixSelection mix;
    mix.asset_identity_fingerprint = metadata.content_sha256;
    mix.authority = InputMixAuthority::VerifiedRecordedDefault;
    const auto& recorded = metadata.audio_metadata;
    if (audio.size() == 1) {
        mix.selected_stream_identities = audio;
    } else if (recorded.status == engine::AudioMetadataStatus::Verified && recorded.manifest &&
               !recorded.monitoring_confirmation_required) {
        for (const auto identity : recorded.manifest->default_monitor_track_ids)
            if (std::find(audio.begin(), audio.end(), identity) != audio.end())
                mix.selected_stream_identities.push_back(identity);
    }
    if (mix.selected_stream_identities.empty()) {
        // Every stream stays available but disabled until the user chooses;
        // alternatives such as a recorded master and its stems are never summed.
        mix.authority = InputMixAuthority::UserConfirmed;
        result.input_mix = mix;
        result.mix_decision_required = true;
        return result;
    }
    result.input_mix = std::move(mix);
    return result;
}

} // namespace exosnap::edit
