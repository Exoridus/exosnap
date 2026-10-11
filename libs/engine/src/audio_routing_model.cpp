#include <cstddef>
#include <cstdint>
#include <exosnap/engine/audio_limits.h>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/audio_track_model.h>
#include <exosnap/engine/recording_media_types.h>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <algorithm>
#include <cmath>
#include <set>

namespace exosnap::engine {
namespace {

EngineError InvalidGraph(std::string context, EngineErrorCode code = EngineErrorCode::InvalidConfig) {
    EngineError error;
    error.code = code;
    error.owner = EngineModule::Configuration;
    error.context = std::move(context);
    return error;
}

template <typename Id> bool IsNil(const Id& id) {
    return std::all_of(id.bytes.begin(), id.bytes.end(), [](uint8_t byte) { return byte == 0; });
}

bool ValidTrackName(std::string_view text) {
    size_t characters = 0;
    for (size_t offset = 0; offset < text.size();) {
        const uint8_t first = static_cast<uint8_t>(text[offset++]);
        uint32_t codepoint = first;
        uint32_t minimum = 0;
        size_t continuation = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            codepoint = first & 0x1f;
            minimum = 0x80;
            continuation = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            codepoint = first & 0x0f;
            minimum = 0x800;
            continuation = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            codepoint = first & 0x07;
            minimum = 0x10000;
            continuation = 3;
        } else if (first >= 0x80) {
            return false;
        }
        if (continuation > text.size() - offset)
            return false;
        for (size_t byte = 0; byte < continuation; ++byte) {
            const uint8_t next = static_cast<uint8_t>(text[offset++]);
            if ((next & 0xc0) != 0x80)
                return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            codepoint < 0x20 || (codepoint >= 0x7f && codepoint <= 0x9f) || codepoint == 0x2028 ||
            codepoint == 0x2029 || ++characters > 128)
            return false;
    }
    return characters != 0;
}

} // namespace

Outcome<std::monostate> ValidateTrackGraph1Intent(const TrackGraph1Intent& intent) {
    if (intent.sources.size() > kMaxAudioSources || intent.routes.size() > kMaxAudioRoutes)
        return InvalidGraph("Audio source or route budget exceeded", EngineErrorCode::ResourceBudgetExceeded);
    std::set<AudioSourceId> sources;
    for (const auto& source : intent.sources) {
        if (IsNil(source.id) || !sources.insert(source.id).second)
            return InvalidGraph("Audio source identity is missing or duplicated");
        if (!std::isfinite(source.dsp.hpf_cutoff_hz) || !std::isfinite(source.dsp.gate_threshold_db) ||
            !std::isfinite(source.dsp.agc_target_db))
            return InvalidGraph("Audio source DSP parameters must be finite");
        if (!std::isfinite(source.post_dsp_gain_linear) || source.post_dsp_gain_linear < 0)
            return InvalidGraph("Audio source gain must be finite and nonnegative");
    }
    std::set<AudioTrackId> tracks;
    std::set<uint32_t> orders;
    for (const auto& track : intent.tracks) {
        if (IsNil(track.id) || !tracks.insert(track.id).second || !orders.insert(track.order).second)
            return InvalidGraph("Audio track identity or order is missing or duplicated");
        if (track.name_kind == AudioTrackNameKind::Custom && !ValidTrackName(track.name))
            return InvalidGraph("Custom audio track name must contain 1 to 128 UTF-8 characters without controls");
    }
    std::set<AudioRouteId> routes;
    std::set<std::pair<AudioSourceId, AudioTrackId>> pairs;
    std::set<AudioTrackId> routed_tracks;
    for (const auto& route : intent.routes) {
        if (IsNil(route.id) || !routes.insert(route.id).second)
            return InvalidGraph("Audio route identity is missing or duplicated");
        if (!sources.contains(route.source_id) || !tracks.contains(route.track_id))
            return InvalidGraph("Audio route references a missing source or track");
        if (!pairs.insert({route.source_id, route.track_id}).second)
            return InvalidGraph("Audio source and track pair has more than one route");
        if (!std::isfinite(route.gain_db) || route.gain_db < kMinGainDb || route.gain_db > kMaxGainDb)
            return InvalidGraph("Audio route gain is outside the supported range");
        if (route.enabled)
            routed_tracks.insert(route.track_id);
    }
    size_t recorded = 0;
    for (const auto& track : intent.tracks)
        if (track.record_enabled && routed_tracks.contains(track.id))
            ++recorded;
    if (recorded > kMaxAudioTracks)
        return InvalidGraph("Audio track budget exceeded", EngineErrorCode::ResourceBudgetExceeded);
    std::set<AudioTrackId> monitor;
    for (const auto& id : intent.default_monitor_track_ids)
        if (!tracks.contains(id) || !monitor.insert(id).second)
            return InvalidGraph("Monitor selection references a missing or duplicated track");
    if (!std::isfinite(intent.limiter.ceiling_db) || intent.limiter.ceiling_db > 0)
        return InvalidGraph("Limiter ceiling must be finite and no greater than zero dBFS");
    return std::monostate{};
}

} // namespace exosnap::engine
