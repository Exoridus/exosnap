#include <exosnap/engine/audio_routing_model.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace exosnap::engine {
namespace {

EngineError Invalid(std::string context, EngineErrorCode code = EngineErrorCode::InvalidConfig) {
    EngineError error;
    error.owner = EngineModule::Audio;
    error.code = code;
    error.context = std::move(context);
    return error;
}

AudioSourceSpec LegacySource(const LegacyRows25Intent& intent, const LegacyAudioRow& row) {
    AudioSourceSpec source;
    source.id = row.source_id;
    source.legacy_row_id = row.row_id;
    source.name = AudioSourceKindDisplayName(row.row.kind);
    source.participation = row.row.enabled;
    source.muted = row.row.muted;
    source.binding.endpoint_role = AudioEndpointRole::Console;
    switch (row.row.kind) {
    case AudioSourceKind::Mic:
        source.binding = intent.microphone_binding;
        source.channel_mode = intent.microphone_channel_mode;
        source.dsp = intent.microphone_dsp;
        source.post_dsp_gain_linear = intent.microphone_gain_linear;
        break;
    case AudioSourceKind::App:
        source.binding.kind = AudioBindingKind::CaptureTargetProcess;
        break;
    case AudioSourceKind::Sys:
        source.binding.kind = AudioBindingKind::LegacyExcludeTarget;
        break;
    case AudioSourceKind::SystemOutput:
        source.binding.kind = AudioBindingKind::PlaybackEndpoint;
        break;
    }
    return source;
}

Outcome<TrackGraph1Intent> LegacyGraph(const LegacyRows25Intent& intent, bool window_target) {
    if (intent.id_namespace == PresetNamespaceId{})
        return Invalid("Legacy preset namespace is missing");
    TrackGraph1Intent graph;
    graph.limiter = intent.limiter;
    graph.monitor_selection_origin = intent.monitor_selection_origin;
    std::set<LegacyRowId> row_ids;
    std::set<AudioTrackId> anchor_ids;
    for (const auto& row : intent.rows) {
        if (row.row_id == LegacyRowId{} || row.anchor_track_id == AudioTrackId{} ||
            !row_ids.insert(row.row_id).second || !anchor_ids.insert(row.anchor_track_id).second)
            return Invalid("Legacy row or anchor identity is missing or duplicated");
        graph.sources.push_back(LegacySource(intent, row));
    }

    std::vector<AudioSourceRow> raw;
    for (const auto& row : intent.rows)
        raw.push_back(row.row);
    const auto normalized = NormalizeSourceRowsForTarget(raw, window_target);
    const auto oracle = ResolveAudioTracks(normalized);

    // Retain ordinals separately because duplicate kinds are different logical sources.
    std::vector<size_t> ordinals;
    bool seen_system_output = false;
    for (size_t i = 0; i < intent.rows.size(); ++i) {
        const auto kind = intent.rows[i].row.kind;
        if (!window_target && kind == AudioSourceKind::App) {
            graph.sources[i].participation = false;
            continue;
        }
        if (!window_target && (kind == AudioSourceKind::Sys || kind == AudioSourceKind::SystemOutput)) {
            graph.sources[i].binding.kind = AudioBindingKind::PlaybackEndpoint;
            if (seen_system_output) {
                graph.sources[i].participation = false;
                continue;
            }
            seen_system_output = true;
        }
        if (intent.rows[i].row.enabled)
            ordinals.push_back(i);
    }
    size_t cursor = 0;
    for (const auto& resolved : oracle.tracks) {
        const auto& anchor = intent.rows[ordinals[cursor]];
        AudioTrackSpec track;
        track.id = anchor.anchor_track_id;
        track.order = static_cast<uint32_t>(graph.tracks.size());
        track.name_kind = anchor.name_kind;
        track.name = anchor.name;
        track.role = anchor.role;
        track.record_enabled = anchor.record_enabled;
        graph.tracks.push_back(track);
        for (size_t j = 0; j < resolved.sources.size(); ++j) {
            const auto& row = intent.rows[ordinals[cursor++]];
            AudioRouteSpec route;
            route.id.bytes = row.source_id.bytes;
            route.source_id = row.source_id;
            route.track_id = track.id;
            route.gain_db = row.row.gain_db;
            graph.routes.push_back(route);
        }
    }
    for (const auto& id : intent.default_monitor_track_ids)
        if (std::any_of(graph.tracks.begin(), graph.tracks.end(), [&](const auto& track) { return track.id == id; }))
            graph.default_monitor_track_ids.push_back(id);
    auto valid = ValidateTrackGraph1Intent(graph);
    if (!valid.succeeded())
        return *valid.error();
    return graph;
}

ResolvedAudioSource ResolveSource(const AudioSourceSpec& spec, const AudioEnvironmentSnapshot& environment) {
    ResolvedAudioSource result;
    result.spec = spec;
    const auto& binding = spec.binding;
    PhysicalCaptureKey key;
    key.kind =
        binding.kind == AudioBindingKind::CaptureTargetProcess ? AudioBindingKind::IndependentProcess : binding.kind;
    if (binding.kind == AudioBindingKind::PlaybackEndpoint || binding.kind == AudioBindingKind::MicrophoneEndpoint) {
        const bool microphone = binding.kind == AudioBindingKind::MicrophoneEndpoint;
        const auto& endpoints = microphone ? environment.microphone_endpoints : environment.playback_endpoints;
        const auto& defaults =
            microphone ? environment.default_microphone_by_role : environment.default_playback_by_role;
        const auto role = static_cast<size_t>(binding.endpoint_role);
        if (binding.follow_default) {
            if (role < defaults.size())
                key.endpoint_id = defaults[role];
            if (key.endpoint_id.empty() && binding.endpoint_role == AudioEndpointRole::Console)
                key.endpoint_id =
                    microphone ? environment.default_microphone_endpoint : environment.default_playback_endpoint;
        } else {
            key.endpoint_id = binding.endpoint_id;
        }
        if (key.endpoint_id.empty() ||
            std::find(endpoints.begin(), endpoints.end(), key.endpoint_id) == endpoints.end())
            result.availability = AudioSourceAvailability::EndpointUnavailable;
    } else {
        if (!environment.process_loopback_supported) {
            result.availability = AudioSourceAvailability::UnsupportedPlatform;
        } else {
            key.process = binding.kind == AudioBindingKind::IndependentProcess ? binding.process
                                                                               : environment.capture_target_process;
            if (!key.process || key.process->process_id == 0 ||
                (binding.kind != AudioBindingKind::IndependentProcess && !environment.window_target)) {
                result.availability = AudioSourceAvailability::NoTargetProcess;
            } else {
                const auto match =
                    std::find_if(environment.confirmed_processes.begin(), environment.confirmed_processes.end(),
                                 [&](const auto& process) { return process.process_id == key.process->process_id; });
                if (match == environment.confirmed_processes.end())
                    result.availability = AudioSourceAvailability::UnconfirmedProcess;
                else if (match->creation_time != key.process->creation_time)
                    result.availability = AudioSourceAvailability::ProcessIdentityChanged;
                else if (key.process->creation_time == 0)
                    result.availability = AudioSourceAvailability::UnconfirmedProcess;
                else if (!key.process->identity_hint.empty() && match->identity_hint != key.process->identity_hint)
                    result.availability = AudioSourceAvailability::ProcessIdentityChanged;
            }
        }
    }
    if (result.availability == AudioSourceAvailability::Available) {
        if (key.process)
            key.process->identity_hint.clear();
        result.capture_key = key;
    } else
        result.silence_for_this_start =
            std::find(environment.allow_silent_sources.begin(), environment.allow_silent_sources.end(), spec.id) !=
            environment.allow_silent_sources.end();
    return result;
}

} // namespace

Outcome<LegacyRows25Intent> ApplyLegacyRowEdit(const LegacyRows25Intent& intent, LegacyRowId row_id,
                                               const LegacyRowPatch& patch) {
    auto edited = intent;
    auto row =
        std::find_if(edited.rows.begin(), edited.rows.end(), [&](const auto& item) { return item.row_id == row_id; });
    if (row == edited.rows.end() || std::count_if(edited.rows.begin(), edited.rows.end(),
                                                  [&](const auto& item) { return item.row_id == row_id; }) != 1)
        return Invalid("Legacy row identity is missing or duplicated");
    if (patch.gain_db && (!std::isfinite(*patch.gain_db) || *patch.gain_db < kMinGainDb || *patch.gain_db > kMaxGainDb))
        return Invalid("Legacy row gain is outside the supported range");
    if (patch.enabled)
        row->row.enabled = *patch.enabled;
    if (patch.muted)
        row->row.muted = *patch.muted;
    if (patch.gain_db)
        row->row.gain_db = *patch.gain_db;
    return edited;
}

Outcome<TrackGraph1Intent> MaterializeLegacyIntent(const LegacyRows25Intent& intent,
                                                   const AudioEnvironmentSnapshot& environment) {
    return LegacyGraph(intent, environment.window_target);
}

Outcome<ResolvedAudioPlan> ResolveAudioIntent(const AudioIntent& intent, const AudioEnvironmentSnapshot& environment,
                                              ConfigRevision revision) {
    TrackGraph1Intent graph;
    if (const auto* legacy = std::get_if<LegacyRows25Intent>(&intent)) {
        auto materialized = LegacyGraph(*legacy, environment.window_target);
        if (!materialized.succeeded())
            return *materialized.error();
        graph = *materialized.value();
    } else {
        graph = std::get<TrackGraph1Intent>(intent);
    }
    auto valid = ValidateTrackGraph1Intent(graph);
    if (!valid.succeeded())
        return *valid.error();
    ResolvedAudioPlan plan;
    plan.revision = revision;
    plan.monitor_selection_origin = graph.monitor_selection_origin;
    plan.default_monitor_track_ids = graph.default_monitor_track_ids;
    for (const auto& source : graph.sources) {
        const bool used = std::any_of(graph.routes.begin(), graph.routes.end(), [&](const auto& route) {
            if (!route.enabled || route.source_id != source.id)
                return false;
            return std::any_of(graph.tracks.begin(), graph.tracks.end(),
                               [&](const auto& track) { return track.record_enabled && track.id == route.track_id; });
        });
        auto resolved = ResolveSource(source, environment);
        if (!used || !source.participation)
            resolved.capture_key.reset();
        if (used && source.participation && resolved.availability != AudioSourceAvailability::Available &&
            !resolved.silence_for_this_start)
            return Invalid("Audio source is unavailable: " + ToUuid(source.id), EngineErrorCode::UnavailableSource);
        if (resolved.silence_for_this_start)
            plan.warnings.push_back("Audio source starts with silence: " + ToUuid(source.id));
        plan.sources.push_back(std::move(resolved));
    }
    std::stable_sort(graph.tracks.begin(), graph.tracks.end(),
                     [](const auto& a, const auto& b) { return a.order < b.order; });
    for (size_t position = 0; position < graph.tracks.size(); ++position) {
        const auto& configured = graph.tracks[position];
        if (!configured.record_enabled)
            continue;
        ResolvedRoutingTrack track;
        track.id = configured.id;
        track.mux_index.value = static_cast<uint32_t>(plan.tracks.size());
        track.visible_position = static_cast<uint32_t>(position + 1);
        track.name = configured.name_kind == AudioTrackNameKind::Custom ? configured.name
                                                                        : "Audio " + std::to_string(position + 1);
        track.role = configured.role;
        track.limiter = graph.limiter;
        for (const auto& route : graph.routes)
            if (route.track_id == configured.id && route.enabled)
                track.routes.push_back({route.id, route.source_id, GainDbToLinear(route.gain_db, route.muted)});
        if (track.routes.empty())
            continue;
        track.fixed_average_divisor = static_cast<uint32_t>(track.routes.size());
        if (plan.monitor_selection_origin == MonitorSelectionOrigin::Default &&
            std::find(plan.default_monitor_track_ids.begin(), plan.default_monitor_track_ids.end(), track.id) ==
                plan.default_monitor_track_ids.end())
            plan.default_monitor_track_ids.push_back(track.id);
        plan.tracks.push_back(std::move(track));
    }
    std::erase_if(plan.default_monitor_track_ids, [&](const auto& id) {
        if (std::any_of(plan.tracks.begin(), plan.tracks.end(), [&](const auto& track) { return track.id == id; }))
            return false;
        plan.warnings.push_back("Selected monitor track is not recorded: " + ToUuid(id));
        return true;
    });
    bool monitor_overlap = false;
    std::set<AudioSourceId> monitored;
    std::set<PhysicalCaptureKey> monitored_captures;
    for (const auto& track : plan.tracks) {
        if (std::find(plan.default_monitor_track_ids.begin(), plan.default_monitor_track_ids.end(), track.id) ==
            plan.default_monitor_track_ids.end()) {
            plan.warnings.push_back("Recorded track is not monitored: " + ToUuid(track.id));
            continue;
        }
        for (const auto& route : track.routes) {
            const auto source = std::find_if(plan.sources.begin(), plan.sources.end(),
                                             [&](const auto& item) { return item.spec.id == route.source_id; });
            if (!monitored.insert(route.source_id).second) {
                monitor_overlap = true;
                plan.warnings.push_back("Monitor selection contains overlapping source: " + ToUuid(route.source_id));
            } else if (source != plan.sources.end() && source->capture_key &&
                       !monitored_captures.insert(*source->capture_key).second) {
                monitor_overlap = true;
                plan.warnings.push_back("Monitor selection contains sources sharing a physical capture: " +
                                        ToUuid(route.source_id));
            }
        }
    }
    const bool full_system = std::any_of(monitored_captures.begin(), monitored_captures.end(), [](const auto& key) {
        return key.kind == AudioBindingKind::PlaybackEndpoint;
    });
    const bool included_process =
        std::any_of(monitored_captures.begin(), monitored_captures.end(), [](const auto& key) {
            return key.kind == AudioBindingKind::CaptureTargetProcess ||
                   key.kind == AudioBindingKind::IndependentProcess;
        });
    if (full_system && included_process)
        plan.warnings.push_back("System and application audio may overlap in the monitor selection");
    plan.monitor_confirmation_required = plan.monitor_selection_origin == MonitorSelectionOrigin::Default &&
                                         (monitor_overlap || (full_system && included_process));
    std::set<PhysicalCaptureKey> captures;
    for (const auto& source : plan.sources)
        if (source.capture_key)
            captures.insert(*source.capture_key);
    if (captures.size() > kMaxPhysicalAudioCaptures)
        return Invalid("Physical audio capture budget exceeded", EngineErrorCode::ResourceBudgetExceeded);
    return plan;
}

} // namespace exosnap::engine
