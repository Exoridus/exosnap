#pragma once

#include <exosnap/engine/audio_limits.h>
#include <exosnap/engine/audio_track_model.h>
#include <exosnap/engine/mic_dsp_config.h>
#include <exosnap/engine/recording_media_types.h>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace exosnap::engine {

enum class AudioEndpointRole { Console, Multimedia, Communications };
enum class AudioBindingKind {
    PlaybackEndpoint,
    MicrophoneEndpoint,
    CaptureTargetProcess,
    IndependentProcess,
    LegacyExcludeTarget,
};

struct ProcessInstanceIdentity {
    uint32_t process_id = 0;
    uint64_t creation_time = 0;
    std::string identity_hint;
    auto operator<=>(const ProcessInstanceIdentity&) const = default;
};

struct AudioSourceBinding {
    AudioBindingKind kind = AudioBindingKind::PlaybackEndpoint;
    bool follow_default = true;
    AudioEndpointRole endpoint_role = AudioEndpointRole::Multimedia;
    std::string endpoint_id;
    std::optional<ProcessInstanceIdentity> process;
    // Runtime-only authority from explicit selection. Preset loading clears it.
    bool process_selection_confirmed = true;
    bool operator==(const AudioSourceBinding&) const = default;
};

struct AudioSourceSpec {
    AudioSourceId id;
    std::optional<LegacyRowId> legacy_row_id;
    std::string name;
    AudioSourceBinding binding;
    MicChannelMode channel_mode = MicChannelMode::Auto;
    MicDspConfig dsp;
    float post_dsp_gain_linear = 1.0f;
    bool participation = true;
    bool muted = false;
    bool operator==(const AudioSourceSpec&) const = default;
};

enum class AudioTrackNameKind { Automatic, Custom };
enum class AudioTrackRole { Standard, Stem, Mix };
enum class MonitorSelectionOrigin { Default, User };

struct TrackLimiterConfig {
    bool enabled = true;
    float ceiling_db = 0.0f;
    bool operator==(const TrackLimiterConfig&) const = default;
};

struct AudioTrackSpec {
    AudioTrackId id;
    uint32_t order = 0;
    AudioTrackNameKind name_kind = AudioTrackNameKind::Automatic;
    std::string name;
    bool record_enabled = true;
    AudioTrackRole role = AudioTrackRole::Standard;
    bool operator==(const AudioTrackSpec&) const = default;
};

struct AudioRouteSpec {
    AudioRouteId id;
    AudioSourceId source_id;
    AudioTrackId track_id;
    float gain_db = 0.0f;
    bool muted = false;
    bool enabled = true;
    bool operator==(const AudioRouteSpec&) const = default;
};

struct TrackGraph1Intent {
    std::vector<AudioSourceSpec> sources;
    std::vector<AudioTrackSpec> tracks;
    std::vector<AudioRouteSpec> routes;
    std::vector<AudioTrackId> default_monitor_track_ids;
    MonitorSelectionOrigin monitor_selection_origin = MonitorSelectionOrigin::Default;
    TrackLimiterConfig limiter;
    bool operator==(const TrackGraph1Intent&) const = default;
};

struct LegacyAudioRow {
    LegacyRowId row_id;
    AudioSourceId source_id;
    AudioTrackId anchor_track_id;
    AudioSourceRow row;
    AudioTrackNameKind name_kind = AudioTrackNameKind::Automatic;
    std::string name;
    AudioTrackRole role = AudioTrackRole::Standard;
    bool record_enabled = true;
    bool operator==(const LegacyAudioRow&) const = default;
};

struct LegacyRows25Intent {
    PresetNamespaceId id_namespace;
    std::vector<LegacyAudioRow> rows;
    AudioSourceBinding microphone_binding{AudioBindingKind::MicrophoneEndpoint, true, AudioEndpointRole::Console};
    MicChannelMode microphone_channel_mode = MicChannelMode::Auto;
    MicDspConfig microphone_dsp;
    float microphone_gain_linear = 1.0f;
    std::vector<AudioTrackId> default_monitor_track_ids;
    MonitorSelectionOrigin monitor_selection_origin = MonitorSelectionOrigin::Default;
    TrackLimiterConfig limiter;
    bool operator==(const LegacyRows25Intent&) const = default;
};

using AudioIntent = std::variant<LegacyRows25Intent, TrackGraph1Intent>;

struct PhysicalCaptureKey {
    AudioBindingKind kind = AudioBindingKind::PlaybackEndpoint;
    std::string endpoint_id;
    std::optional<ProcessInstanceIdentity> process;
    uint64_t format_generation = 0;
    uint32_t capture_flags = 0;
    auto operator<=>(const PhysicalCaptureKey&) const = default;
};

enum class AudioSourceAvailability {
    Available,
    UnsupportedPlatform,
    EndpointUnavailable,
    ProcessExited,
    ProcessIdentityChanged,
    NoTargetProcess,
    UnconfirmedProcess,
};

struct AudioEnvironmentSnapshot {
    bool window_target = false;
    bool process_loopback_supported = false;
    std::optional<ProcessInstanceIdentity> capture_target_process;
    // Legacy default fields identify the Console endpoint.
    std::string default_playback_endpoint;
    std::string default_microphone_endpoint;
    std::array<std::string, 3> default_playback_by_role;
    std::array<std::string, 3> default_microphone_by_role;
    std::vector<std::string> playback_endpoints;
    std::vector<std::string> microphone_endpoints;
    std::vector<ProcessInstanceIdentity> confirmed_processes;
    std::vector<AudioSourceId> allow_silent_sources;
};

struct ResolvedAudioSource {
    AudioSourceSpec spec;
    std::optional<PhysicalCaptureKey> capture_key;
    AudioSourceAvailability availability = AudioSourceAvailability::Available;
    bool silence_for_this_start = false;
};

struct ResolvedAudioRoute {
    AudioRouteId id;
    AudioSourceId source_id;
    float gain_linear = 1.0f;
};

struct ResolvedRoutingTrack {
    AudioTrackId id;
    AudioStreamIndex mux_index;
    uint32_t visible_position = 0;
    std::string name;
    AudioTrackRole role = AudioTrackRole::Standard;
    std::vector<ResolvedAudioRoute> routes;
    uint32_t fixed_average_divisor = 0;
    TrackLimiterConfig limiter;
};

struct ResolvedAudioPlan {
    ConfigRevision revision;
    std::vector<ResolvedAudioSource> sources;
    std::vector<ResolvedRoutingTrack> tracks;
    std::vector<AudioTrackId> default_monitor_track_ids;
    MonitorSelectionOrigin monitor_selection_origin = MonitorSelectionOrigin::Default;
    bool monitor_confirmation_required = false;
    std::vector<std::string> warnings;
};

struct LegacyRowPatch {
    std::optional<bool> enabled;
    std::optional<bool> muted;
    std::optional<float> gain_db;
};

[[nodiscard]] Outcome<std::monostate> ValidateTrackGraph1Intent(const TrackGraph1Intent& intent);

[[nodiscard]] Outcome<ResolvedAudioPlan> ResolveAudioIntent(const AudioIntent& intent,
                                                            const AudioEnvironmentSnapshot& environment,
                                                            ConfigRevision revision = {});
[[nodiscard]] Outcome<LegacyRows25Intent> ApplyLegacyRowEdit(const LegacyRows25Intent& intent, LegacyRowId row_id,
                                                             const LegacyRowPatch& patch);
[[nodiscard]] Outcome<TrackGraph1Intent> MaterializeLegacyIntent(const LegacyRows25Intent& intent,
                                                                 const AudioEnvironmentSnapshot& environment);

} // namespace exosnap::engine
