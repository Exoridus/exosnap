#include "AudioRoutingMigration.h"

#include <algorithm>
#include <cstring>

namespace exosnap {
namespace {

uint64_t Mix(uint64_t value) {
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

template <typename Id> Id Derived(engine::PresetNamespaceId id_namespace, size_t ordinal, uint64_t domain) {
    uint64_t first = 0;
    uint64_t second = 0;
    for (size_t i = 0; i < 8; ++i) {
        first = (first << 8) | id_namespace.bytes[i];
        second = (second << 8) | id_namespace.bytes[i + 8];
    }
    first = Mix(first ^ Mix(ordinal + domain));
    second = Mix(second ^ Mix(ordinal + domain + 0x9e3779b97f4a7c15ULL));
    Id id;
    for (size_t i = 0; i < 8; ++i) {
        id.bytes[i] = static_cast<uint8_t>(first >> (i * 8));
        id.bytes[i + 8] = static_cast<uint8_t>(second >> (i * 8));
    }
    id.bytes[6] = (id.bytes[6] & 0x0f) | 0x80;
    id.bytes[8] = (id.bytes[8] & 0x3f) | 0x80;
    return id;
}

} // namespace

engine::LegacyRows25Intent CreateLegacyAudioIntent(const capability::AudioUiState& audio,
                                                   engine::PresetNamespaceId id_namespace) {
    engine::LegacyRows25Intent intent;
    intent.id_namespace = id_namespace;
    for (size_t i = 0; i < audio.source_rows.size(); ++i) {
        engine::LegacyAudioRow row;
        row.row_id = Derived<engine::LegacyRowId>(id_namespace, i, 1);
        row.source_id = Derived<engine::AudioSourceId>(id_namespace, i, 2);
        row.anchor_track_id = Derived<engine::AudioTrackId>(id_namespace, i, 3);
        row.row = audio.source_rows[i];
        intent.rows.push_back(row);
    }
    intent.microphone_binding.kind = engine::AudioBindingKind::MicrophoneEndpoint;
    intent.microphone_binding.endpoint_role = engine::AudioEndpointRole::Console;
    intent.microphone_binding.follow_default = !audio.selected_mic_device_id.has_value();
    intent.microphone_binding.endpoint_id = audio.selected_mic_device_id.value_or("");
    intent.microphone_channel_mode = audio.mic_channel_mode;
    intent.microphone_gain_linear = audio.mic_gain_linear;
    intent.microphone_dsp = {audio.mic_hpf_enabled,       audio.mic_hpf_cutoff_hz, audio.mic_gate_enabled,
                             audio.mic_gate_threshold_db, audio.mic_agc_enabled,   audio.mic_agc_target_db,
                             audio.mic_rnnoise_enabled};
    intent.limiter = {audio.limiter_enabled, audio.limiter_ceiling_db};
    return intent;
}

engine::TrackGraph1Intent MakeDefaultAudioIntent() {
    const auto id_namespace = *engine::FromUuid<engine::PresetNamespaceId>("4dfd7652-66c6-470b-a736-43eec5d182bb");
    engine::TrackGraph1Intent graph;
    engine::AudioTrackSpec track;
    track.id = Derived<engine::AudioTrackId>(id_namespace, 0, 3);
    graph.tracks.push_back(track);
    for (size_t i = 0; i < 2; ++i) {
        engine::AudioSourceSpec source;
        source.id = Derived<engine::AudioSourceId>(id_namespace, i, 2);
        source.binding.kind =
            i == 0 ? engine::AudioBindingKind::PlaybackEndpoint : engine::AudioBindingKind::MicrophoneEndpoint;
        source.name = i == 0 ? "System" : "Microphone";
        source.participation = i == 0;
        graph.sources.push_back(source);
        if (i == 0)
            graph.routes.push_back({Derived<engine::AudioRouteId>(id_namespace, i, 4), source.id, track.id});
    }
    graph.default_monitor_track_ids = {track.id};
    return graph;
}

engine::TrackGraph1Intent MakeNewAudioIntent() {
    auto graph = MakeDefaultAudioIntent();
    const auto track_id = engine::NewId<engine::AudioTrackId>();
    graph.tracks[0].id = track_id;
    for (auto& source : graph.sources) {
        const auto previous = source.id;
        source.id = engine::NewId<engine::AudioSourceId>();
        for (auto& route : graph.routes)
            if (route.source_id == previous) {
                route.source_id = source.id;
                route.track_id = track_id;
                route.id = engine::NewId<engine::AudioRouteId>();
            }
    }
    graph.default_monitor_track_ids = {track_id};
    return graph;
}

void ProjectLegacyAudioIntent(const engine::LegacyRows25Intent& intent, capability::AudioUiState& audio) {
    audio.limiter_enabled = intent.limiter.enabled;
    audio.limiter_ceiling_db = intent.limiter.ceiling_db;
    audio.source_rows.clear();
    for (const auto& row : intent.rows)
        audio.source_rows.push_back(row.row);
    audio.selected_mic_device_id =
        intent.microphone_binding.follow_default ? std::nullopt : std::optional{intent.microphone_binding.endpoint_id};
    audio.mic_channel_mode = intent.microphone_channel_mode;
    audio.mic_gain_linear = intent.microphone_gain_linear;
    audio.mic_hpf_enabled = intent.microphone_dsp.hpf_enabled;
    audio.mic_hpf_cutoff_hz = intent.microphone_dsp.hpf_cutoff_hz;
    audio.mic_gate_enabled = intent.microphone_dsp.gate_enabled;
    audio.mic_gate_threshold_db = intent.microphone_dsp.gate_threshold_db;
    audio.mic_agc_enabled = intent.microphone_dsp.agc_enabled;
    audio.mic_agc_target_db = intent.microphone_dsp.agc_target_db;
    audio.mic_rnnoise_enabled = intent.microphone_dsp.rnnoise_enabled;
}

void ProjectNativeAudioIntent(const engine::TrackGraph1Intent& intent, capability::AudioUiState& audio) {
    audio.limiter_enabled = intent.limiter.enabled;
    audio.limiter_ceiling_db = intent.limiter.ceiling_db;
    audio.source_rows.clear();
    for (const auto& source : intent.sources) {
        engine::AudioSourceRow row;
        switch (source.binding.kind) {
        case engine::AudioBindingKind::MicrophoneEndpoint:
            row.kind = engine::AudioSourceKind::Mic;
            break;
        case engine::AudioBindingKind::PlaybackEndpoint:
            row.kind = engine::AudioSourceKind::SystemOutput;
            break;
        case engine::AudioBindingKind::LegacyExcludeTarget:
            row.kind = engine::AudioSourceKind::Sys;
            break;
        default:
            row.kind = engine::AudioSourceKind::App;
            break;
        }
        row.enabled = source.participation;
        row.muted = source.muted;
        const auto route = std::find_if(intent.routes.begin(), intent.routes.end(),
                                        [&](const auto& item) { return item.source_id == source.id; });
        if (route != intent.routes.end()) {
            row.gain_db = route->gain_db;
            row.muted = row.muted || route->muted;
        }
        audio.source_rows.push_back(row);
    }
}

engine::Outcome<engine::LegacyRows25Intent> SynchronizeLegacyAudioRows(const engine::LegacyRows25Intent& intent,
                                                                       const capability::AudioUiState& audio) {
    auto revised = intent;
    if (audio.source_rows.size() != revised.rows.size()) {
        engine::EngineError error;
        error.context = "A legacy structural edit requires explicit track layout conversion";
        return error;
    }
    for (size_t i = 0; i < revised.rows.size(); ++i) {
        if (audio.source_rows[i].kind != revised.rows[i].row.kind) {
            engine::EngineError error;
            error.context = "Legacy source order or kind cannot be changed through its row projection";
            return error;
        }
        revised.rows[i].row = audio.source_rows[i];
    }
    const auto parameters = CreateLegacyAudioIntent(audio, intent.id_namespace);
    revised.microphone_binding = parameters.microphone_binding;
    revised.microphone_channel_mode = parameters.microphone_channel_mode;
    revised.microphone_dsp = parameters.microphone_dsp;
    revised.microphone_gain_linear = parameters.microphone_gain_linear;
    revised.limiter = parameters.limiter;
    return revised;
}

engine::Outcome<AudioIntentConversion>
PrepareAudioIntentConversion(const engine::LegacyRows25Intent& intent,
                             const engine::AudioEnvironmentSnapshot& environment,
                             const std::function<void(engine::TrackGraph1Intent&)>& edit) {
    auto graph = engine::MaterializeLegacyIntent(intent, environment);
    if (!graph.succeeded())
        return *graph.error();
    edit(*graph.value());
    const auto valid = engine::ValidateTrackGraph1Intent(*graph.value());
    if (!valid.succeeded())
        return *valid.error();
    return AudioIntentConversion{intent, std::move(*graph.value())};
}

} // namespace exosnap
