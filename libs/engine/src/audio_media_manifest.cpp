#include "audio_media_manifest_adapter.h"
#include "bounded_metadata_json.h"
#include "budgeted_media_avio.h"
#include "media_identity_sidecar.h"
#include <exosnap/engine/audio_limits.h>
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/audio_track_model.h>
#include <exosnap/engine/background_io.h>
#include <exosnap/engine/mic_dsp_config.h>
#include <exosnap/engine/recording_media_types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <ios>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

namespace exosnap::engine {
namespace {
using Json = MetadataJson;
constexpr size_t kManifestBytes = 1024 * 1024;
constexpr size_t kCodecPrivateBytes = 64 * 1024;
constexpr size_t kDerivationIds = 64;

EngineError MetadataError(const char* context, EngineErrorCode code = EngineErrorCode::MetadataIncomplete) {
    EngineError error;
    error.code = code;
    error.owner = EngineModule::FileOutput;
    error.context = context;
    return error;
}
template <typename Id> bool ValidId(const Id& id) {
    return std::any_of(id.bytes.begin(), id.bytes.end(), [](auto byte) { return byte != 0; });
}
bool ValidGain(float gain) {
    return std::isfinite(gain) && gain >= 0;
}
std::string LayoutName(const AVChannelLayout& layout) {
    std::array<char, 256> name{};
    if (layout.order == AV_CHANNEL_ORDER_UNSPEC || !av_channel_layout_check(&layout) ||
        av_channel_layout_describe(&layout, name.data(), name.size()) < 0)
        return {};
    return name.data();
}
bool ValidLayout(const MediaCodecDescriptor& codec) {
    if (codec.channel_layout.empty() || codec.channel_layout.size() >= 256 ||
        codec.channel_layout.find('\0') != std::string::npos)
        return false;
    AVChannelLayout layout{};
    const bool valid = av_channel_layout_from_string(&layout, codec.channel_layout.c_str()) >= 0 &&
                       layout.nb_channels == static_cast<int>(codec.channels) &&
                       LayoutName(layout) == codec.channel_layout;
    av_channel_layout_uninit(&layout);
    return valid;
}
bool ValidName(std::string_view name) {
    size_t count = 0;
    for (size_t at = 0; at < name.size();) {
        const auto first = static_cast<uint8_t>(name[at++]);
        uint32_t codepoint = first;
        unsigned continuation = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            codepoint = first & 0x1f;
            continuation = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            codepoint = first & 0x0f;
            continuation = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            codepoint = first & 7;
            continuation = 3;
            minimum = 0x10000;
        } else if (first >= 0x80)
            return false;
        for (unsigned index = 0; index < continuation; ++index) {
            if (at == name.size())
                return false;
            const auto byte = static_cast<uint8_t>(name[at++]);
            if ((byte & 0xc0) != 0x80)
                return false;
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            codepoint < 0x20 || (codepoint >= 0x7f && codepoint <= 0x9f) || codepoint == 0x2028 ||
            codepoint == 0x2029 || ++count > 128)
            return false;
    }
    return true;
}
template <typename Id> bool UniqueIds(const std::vector<Id>& ids, size_t maximum) {
    if (ids.size() > maximum)
        return false;
    std::set<Id> seen;
    for (const auto& id : ids)
        if (!ValidId(id) || !seen.insert(id).second)
            return false;
    return true;
}
bool HasOverlap(const std::vector<MonitorTrackSelection>& selected, const AudioManifest1& manifest) {
    std::set<AudioSourceId> previous;
    for (const auto& selection : selected) {
        if (selection.muted || selection.gain_linear == 0)
            continue;
        const auto track = std::find_if(manifest.tracks.begin(), manifest.tracks.end(), [&](const auto& value) {
            return value.container.track_id == selection.track_id;
        });
        if (track == manifest.tracks.end())
            continue;
        std::set<AudioSourceId> current;
        for (const auto& route : track->routes) {
            const auto source = std::find_if(manifest.sources.begin(), manifest.sources.end(),
                                             [&](const auto& value) { return value.id == route.source_id; });
            if (route.gain_linear == 0 ||
                (source != manifest.sources.end() && (source->muted || source->post_dsp_gain_linear == 0)))
                continue;
            current.insert(route.source_id);
        }
        for (const auto& id : current)
            if (previous.contains(id))
                return true;
        previous.insert(current.begin(), current.end());
    }
    return false;
}
std::vector<MonitorTrackSelection> DefaultSelection(const AudioManifest1& manifest) {
    std::vector<MonitorTrackSelection> result;
    for (const auto& id : manifest.default_monitor_track_ids)
        result.push_back({id, 1.0f, false});
    return result;
}
std::string Hex(std::span<const uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}
std::vector<uint8_t> Unhex(const Json& value) {
    if (!value.is_string())
        throw std::invalid_argument("Expected hexadecimal codec bytes");
    const auto& text = value.get_ref<const Json::string_t&>();
    if (text.size() % 2 || text.size() > kCodecPrivateBytes * 2)
        throw std::invalid_argument("Codec bytes exceed budget");
    const auto digit = [](char character) -> uint8_t {
        if (character >= '0' && character <= '9')
            return static_cast<uint8_t>(character - '0');
        if (character >= 'a' && character <= 'f')
            return static_cast<uint8_t>(character - 'a' + 10);
        throw std::invalid_argument("Invalid hexadecimal byte");
    };
    std::vector<uint8_t> result;
    result.reserve(text.size() / 2);
    for (size_t index = 0; index < text.size(); index += 2)
        result.push_back(static_cast<uint8_t>((digit(text[index]) << 4) | digit(text[index + 1])));
    return result;
}
std::string StringValue(const Json& value) {
    if (!value.is_string())
        throw std::invalid_argument("Expected metadata string");
    const auto& text = value.get_ref<const Json::string_t&>();
    return std::string(text.data(), text.size());
}
Json CodecJson(const MediaCodecDescriptor& codec) {
    return {{"codec_id", codec.codec_id},
            {"codec_private", Hex(codec.codec_private)},
            {"sample_rate", codec.sample_rate},
            {"channels", codec.channels},
            {"bit_depth", codec.bit_depth},
            {"codec_delay_samples", codec.codec_delay_samples},
            {"channel_layout", codec.channel_layout}};
}
std::string Fingerprint(const MediaCodecDescriptor& codec) {
    MetadataJsonQuotaScope json_quota;
    const auto bytes = CodecJson(codec).dump();
    std::unique_ptr<AVSHA, decltype(&av_free)> sha(av_sha_alloc(), av_free);
    if (!sha || av_sha_init(sha.get(), 256) < 0)
        throw std::runtime_error("Cannot initialize audio fingerprint");
    av_sha_update(sha.get(), reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    std::array<uint8_t, 32> digest{};
    av_sha_final(sha.get(), digest.data());
    return Hex(digest);
}
void Keys(const Json& value, std::initializer_list<const char*> keys) {
    if (!value.is_object() || value.size() != keys.size())
        throw std::invalid_argument("Unexpected metadata fields");
    for (const auto* key : keys)
        if (!value.contains(key))
            throw std::invalid_argument("Missing metadata field");
}
void KeysWithOptional(const Json& value, std::initializer_list<const char*> required,
                      std::initializer_list<const char*> optional) {
    if (!value.is_object())
        throw std::invalid_argument("Expected metadata object");
    for (const auto* key : required)
        if (!value.contains(key))
            throw std::invalid_argument("Missing metadata field");
    for (auto it = value.begin(); it != value.end(); ++it) {
        const auto known = [&](const char* key) { return it.key() == key; };
        if (std::none_of(required.begin(), required.end(), known) &&
            std::none_of(optional.begin(), optional.end(), known))
            throw std::invalid_argument("Unexpected metadata field");
    }
}
template <typename Integer> Integer Checked(const Json& value) {
    if (!value.is_number_integer())
        throw std::invalid_argument("Expected metadata integer");
    if constexpr (std::is_unsigned_v<Integer>) {
        if (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>() < 0)
            throw std::invalid_argument("Negative metadata integer");
        const auto number = value.get<uint64_t>();
        if (number > (std::numeric_limits<Integer>::max)())
            throw std::invalid_argument("Metadata integer overflow");
        return static_cast<Integer>(number);
    } else {
        if (value.is_number_unsigned() &&
            value.get<uint64_t>() > static_cast<uint64_t>((std::numeric_limits<Integer>::max)()))
            throw std::invalid_argument("Metadata integer overflow");
        return value.get<Integer>();
    }
}
float Number(const Json& value) {
    if (!value.is_number())
        throw std::invalid_argument("Expected metadata number");
    const auto number = value.get<double>();
    if (!std::isfinite(number) || std::abs(number) > (std::numeric_limits<float>::max)())
        throw std::invalid_argument("Metadata number overflow");
    return static_cast<float>(number);
}
bool Boolean(const Json& value) {
    if (!value.is_boolean())
        throw std::invalid_argument("Expected metadata boolean");
    return value.get<bool>();
}
template <typename Id> Id IdValue(const Json& value) {
    if (!value.is_string())
        throw std::invalid_argument("Expected metadata identity");
    const auto& text = value.get_ref<const Json::string_t&>();
    const auto id = FromUuid<Id>(std::string_view(text.data(), text.size()));
    if (!id || !ValidId(*id))
        throw std::invalid_argument("Invalid metadata identity");
    return *id;
}
template <typename Id> Json IdsJson(const std::vector<Id>& ids) {
    Json result = Json::array();
    for (const auto& id : ids)
        result.push_back(ToUuid(id));
    return result;
}
template <typename Id> std::vector<Id> Ids(const Json& value, size_t maximum) {
    if (!value.is_array() || value.size() > maximum)
        throw std::invalid_argument("Metadata identity budget exceeded");
    std::vector<Id> result;
    for (const auto& id : value)
        result.push_back(IdValue<Id>(id));
    return result;
}
Json DspJson(const MicDspConfig& dsp) {
    return {{"hpf_enabled", dsp.hpf_enabled},        {"hpf_cutoff_hz", dsp.hpf_cutoff_hz},
            {"gate_enabled", dsp.gate_enabled},      {"gate_threshold_db", dsp.gate_threshold_db},
            {"agc_enabled", dsp.agc_enabled},        {"agc_target_db", dsp.agc_target_db},
            {"rnnoise_enabled", dsp.rnnoise_enabled}};
}
MicDspConfig Dsp(const Json& value) {
    Keys(value, {"hpf_enabled", "hpf_cutoff_hz", "gate_enabled", "gate_threshold_db", "agc_enabled", "agc_target_db",
                 "rnnoise_enabled"});
    return {Boolean(value["hpf_enabled"]),      Number(value["hpf_cutoff_hz"]), Boolean(value["gate_enabled"]),
            Number(value["gate_threshold_db"]), Boolean(value["agc_enabled"]),  Number(value["agc_target_db"]),
            Boolean(value["rnnoise_enabled"])};
}
Json ManifestJson(const AudioManifest1& manifest) {
    Json sources = Json::array(), tracks = Json::array();
    for (const auto& source : manifest.sources)
        sources.push_back({{"id", ToUuid(source.id)},
                           {"channel_mode", static_cast<uint32_t>(source.channel_mode)},
                           {"dsp", DspJson(source.dsp)},
                           {"post_dsp_gain_linear", source.post_dsp_gain_linear},
                           {"muted", source.muted}});
    for (const auto& track : manifest.tracks) {
        Json routes = Json::array();
        for (const auto& route : track.routes)
            routes.push_back(
                {{"id", ToUuid(route.id)}, {"source_id", ToUuid(route.source_id)}, {"gain_linear", route.gain_linear}});
        Json item = {{"track_id", ToUuid(track.container.track_id)},
                     {"stream_index", track.container.stream_index.value},
                     {"track_uid", track.container.track_uid},
                     {"name", track.container.name},
                     {"role", track.container.role},
                     {"codec", CodecJson(track.container.codec)},
                     {"format_fingerprint", Fingerprint(track.container.codec)},
                     {"source_ids", IdsJson(track.container.source_ids)},
                     {"routes", std::move(routes)},
                     {"fixed_average_divisor", track.fixed_average_divisor},
                     {"limiter", {{"enabled", track.limiter.enabled}, {"ceiling_db", track.limiter.ceiling_db}}},
                     {"derived_from_track_ids", IdsJson(track.derived_from_track_ids)}};
        if (ValidId(track.container.encoder_instance_id)) {
            item["encoder_instance_id"] = ToUuid(track.container.encoder_instance_id);
            item["encoder_reset"] = track.container.encoder_reset;
        }
        if (track.container.first_decoded_sample_r)
            item["first_decoded_sample_r"] = *track.container.first_decoded_sample_r;
        if (track.container.packet_duration_ns)
            item["packet_duration_ns"] = *track.container.packet_duration_ns;
        if (track.container.preroll_ns)
            item["preroll_ns"] = *track.container.preroll_ns;
        tracks.push_back(std::move(item));
    }
    return {{"version", manifest.version},
            {"media_id", ToUuid(manifest.media_id)},
            {"recording_id", ToUuid(manifest.recording_id)},
            {"segment_id", ToUuid(manifest.segment_id)},
            {"derived_from_media_ids", IdsJson(manifest.derived_from_media_ids)},
            {"segment_origin_r_ns", manifest.segment_origin_r_ns},
            {"continuity_group", manifest.continuity_group},
            {"sources", std::move(sources)},
            {"tracks", std::move(tracks)},
            {"default_monitor_track_ids", IdsJson(manifest.default_monitor_track_ids)},
            {"monitor_selection_origin",
             manifest.monitor_selection_origin == MonitorSelectionOrigin::Default ? "default" : "user"}};
}
AudioManifest1 ManifestValue(const Json& value) {
    Keys(value, {"version", "media_id", "recording_id", "segment_id", "derived_from_media_ids", "segment_origin_r_ns",
                 "continuity_group", "sources", "tracks", "default_monitor_track_ids", "monitor_selection_origin"});
    AudioManifest1 result;
    result.version = Checked<uint32_t>(value["version"]);
    result.media_id = IdValue<MediaId>(value["media_id"]);
    result.recording_id = IdValue<RecordingId>(value["recording_id"]);
    result.segment_id = IdValue<SegmentId>(value["segment_id"]);
    result.derived_from_media_ids = Ids<MediaId>(value["derived_from_media_ids"], kDerivationIds);
    result.segment_origin_r_ns = Checked<int64_t>(value["segment_origin_r_ns"]);
    result.continuity_group = Checked<uint64_t>(value["continuity_group"]);
    if (!value["sources"].is_array() || value["sources"].size() > kMaxAudioSources || !value["tracks"].is_array() ||
        value["tracks"].size() > kMaxAudioTracks)
        throw std::invalid_argument("Audio manifest layout budget exceeded");
    for (const auto& source : value["sources"]) {
        Keys(source, {"id", "channel_mode", "dsp", "post_dsp_gain_linear", "muted"});
        result.sources.push_back({IdValue<AudioSourceId>(source["id"]),
                                  static_cast<MicChannelMode>(Checked<uint32_t>(source["channel_mode"])),
                                  Dsp(source["dsp"]), Number(source["post_dsp_gain_linear"]),
                                  Boolean(source["muted"])});
    }
    for (const auto& item : value["tracks"]) {
        KeysWithOptional(
            item,
            {"track_id", "stream_index", "track_uid", "name", "role", "codec", "format_fingerprint", "source_ids",
             "routes", "fixed_average_divisor", "limiter", "derived_from_track_ids"},
            {"encoder_instance_id", "encoder_reset", "first_decoded_sample_r", "packet_duration_ns", "preroll_ns"});
        AudioManifestTrack track;
        track.container.track_id = IdValue<AudioTrackId>(item["track_id"]);
        track.container.stream_index = {Checked<uint32_t>(item["stream_index"])};
        track.container.track_uid = Checked<uint64_t>(item["track_uid"]);
        track.container.name = StringValue(item["name"]);
        track.container.role = StringValue(item["role"]);
        const auto& codec = item["codec"];
        Keys(codec, {"codec_id", "codec_private", "sample_rate", "channels", "bit_depth", "codec_delay_samples",
                     "channel_layout"});
        track.container.codec = {
            StringValue(codec["codec_id"]),          Unhex(codec["codec_private"]),
            Checked<uint32_t>(codec["sample_rate"]), Checked<uint32_t>(codec["channels"]),
            Checked<uint32_t>(codec["bit_depth"]),   Checked<uint32_t>(codec["codec_delay_samples"]),
            StringValue(codec["channel_layout"])};
        if (item["format_fingerprint"] != Fingerprint(track.container.codec))
            throw std::invalid_argument("Audio fingerprint mismatch");
        track.container.source_ids = Ids<AudioSourceId>(item["source_ids"], kMaxAudioSources);
        if (item.contains("encoder_instance_id"))
            track.container.encoder_instance_id = IdValue<AudioEncoderInstanceId>(item["encoder_instance_id"]);
        if (item.contains("encoder_reset"))
            track.container.encoder_reset = Boolean(item["encoder_reset"]);
        if (item.contains("first_decoded_sample_r"))
            track.container.first_decoded_sample_r = Checked<int64_t>(item["first_decoded_sample_r"]);
        if (item.contains("packet_duration_ns"))
            track.container.packet_duration_ns = Checked<uint64_t>(item["packet_duration_ns"]);
        if (item.contains("preroll_ns"))
            track.container.preroll_ns = Checked<uint64_t>(item["preroll_ns"]);
        if (!item["routes"].is_array() || item["routes"].size() > kMaxAudioRoutes)
            throw std::invalid_argument("Route budget exceeded");
        for (const auto& route : item["routes"]) {
            Keys(route, {"id", "source_id", "gain_linear"});
            track.routes.push_back({IdValue<AudioRouteId>(route["id"]), IdValue<AudioSourceId>(route["source_id"]),
                                    Number(route["gain_linear"])});
        }
        track.fixed_average_divisor = Checked<uint32_t>(item["fixed_average_divisor"]);
        Keys(item["limiter"], {"enabled", "ceiling_db"});
        track.limiter = {Boolean(item["limiter"]["enabled"]), Number(item["limiter"]["ceiling_db"])};
        track.derived_from_track_ids = Ids<AudioTrackId>(item["derived_from_track_ids"], kDerivationIds);
        result.tracks.push_back(std::move(track));
    }
    result.default_monitor_track_ids = Ids<AudioTrackId>(value["default_monitor_track_ids"], kMaxAudioTracks);
    const auto origin = StringValue(value["monitor_selection_origin"]);
    if (origin != "default" && origin != "user")
        throw std::invalid_argument("Unknown monitor selection origin");
    result.monitor_selection_origin =
        origin == "default" ? MonitorSelectionOrigin::Default : MonitorSelectionOrigin::User;
    return result;
}
} // namespace

Outcome<std::monostate> ValidateAudioManifest1(const AudioManifest1& manifest) {
    if (manifest.version != 1 || !ValidId(manifest.media_id) || !ValidId(manifest.recording_id) ||
        !ValidId(manifest.segment_id) || manifest.sources.size() > kMaxAudioSources ||
        manifest.tracks.size() > kMaxAudioTracks || !UniqueIds(manifest.derived_from_media_ids, kDerivationIds) ||
        std::find(manifest.derived_from_media_ids.begin(), manifest.derived_from_media_ids.end(), manifest.media_id) !=
            manifest.derived_from_media_ids.end())
        return MetadataError("Invalid audio manifest identity or layout budget");
    std::set<AudioSourceId> sources;
    for (const auto& source : manifest.sources) {
        if (!ValidId(source.id) || !sources.insert(source.id).second || !ValidGain(source.post_dsp_gain_linear) ||
            static_cast<uint32_t>(source.channel_mode) > static_cast<uint32_t>(MicChannelMode::RightToStereo) ||
            !std::isfinite(source.dsp.hpf_cutoff_hz) || !std::isfinite(source.dsp.gate_threshold_db) ||
            !std::isfinite(source.dsp.agc_target_db))
            return MetadataError("Invalid audio source provenance");
    }
    std::set<AudioTrackId> tracks;
    std::set<uint64_t> uids;
    std::set<uint32_t> indices;
    std::set<AudioRouteId> routes;
    for (const auto& track : manifest.tracks) {
        const auto& container = track.container;
        if (!ValidId(container.track_id) || !tracks.insert(container.track_id).second || container.track_uid <= 1 ||
            !uids.insert(container.track_uid).second || container.stream_index.value >= manifest.tracks.size() ||
            !indices.insert(container.stream_index.value).second || !ValidName(container.name) ||
            (container.role != "standard" && container.role != "stem" && container.role != "mix") ||
            container.codec.codec_id.empty() || container.codec.codec_id.size() > 64 ||
            container.codec.codec_private.size() > kCodecPrivateBytes || container.codec.sample_rate == 0 ||
            container.codec.channels == 0 || container.codec.channels > 64 || container.codec.bit_depth > 64 ||
            !ValidLayout(container.codec) || !UniqueIds(container.source_ids, kMaxAudioSources) ||
            !UniqueIds(track.derived_from_track_ids, kDerivationIds) || !std::isfinite(track.limiter.ceiling_db) ||
            track.limiter.ceiling_db > 0 || track.fixed_average_divisor > kMaxAudioRoutes ||
            (container.encoder_reset && !ValidId(container.encoder_instance_id)) ||
            (container.packet_duration_ns && *container.packet_duration_ns == 0))
            return MetadataError("Invalid audio track mapping, format or provenance");
        if (std::find(track.derived_from_track_ids.begin(), track.derived_from_track_ids.end(), container.track_id) !=
            track.derived_from_track_ids.end())
            return MetadataError("Derived mix must have a new track identity");
        std::set<AudioSourceId> route_sources;
        for (const auto& route : track.routes) {
            if (!ValidId(route.id) || !routes.insert(route.id).second || routes.size() > kMaxAudioRoutes ||
                !sources.contains(route.source_id) || !ValidGain(route.gain_linear))
                return MetadataError("Invalid audio route provenance");
            route_sources.insert(route.source_id);
        }
        if (route_sources != std::set<AudioSourceId>(container.source_ids.begin(), container.source_ids.end()))
            return MetadataError("Audio track source mapping contradicts its routes");
    }
    if (!UniqueIds(manifest.default_monitor_track_ids, kMaxAudioTracks) ||
        static_cast<uint32_t>(manifest.monitor_selection_origin) > static_cast<uint32_t>(MonitorSelectionOrigin::User))
        return MetadataError("Invalid default monitor selection");
    for (const auto& id : manifest.default_monitor_track_ids)
        if (!tracks.contains(id))
            return MetadataError("Default monitor track is missing");
    return std::monostate{};
}
Outcome<std::string> SerializeAudioManifest1(const AudioManifest1& manifest) {
    const auto valid = ValidateAudioManifest1(manifest);
    if (!valid.succeeded())
        return *valid.error();
    try {
        MetadataJsonQuotaScope json_quota;
        auto json = ManifestJson(manifest).dump();
        if (json.size() > kManifestBytes)
            return MetadataError("Audio manifest exceeds serialization budget");
        return std::string(json.data(), json.size());
    } catch (const MetadataJsonBudgetExceeded&) {
        return MetadataError("Audio manifest JSON memory budget exceeded", EngineErrorCode::ResourceBudgetExceeded);
    } catch (...) {
        return MetadataError("Cannot serialize audio manifest");
    }
}
Outcome<AudioManifest1> ParseAudioManifest1(std::string_view text) {
    if (text.empty() || text.size() > kManifestBytes)
        return MetadataError("Audio manifest exceeds parse budget");
    try {
        MetadataJsonQuotaScope json_quota;
        MetadataJsonKeyStack keys;
        const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
            if (depth > 16)
                throw std::invalid_argument("Audio manifest nesting budget exceeded");
            if (event == Json::parse_event_t::object_start)
                keys.emplace_back();
            else if (event == Json::parse_event_t::object_end)
                keys.pop_back();
            else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<MetadataJsonString>()).second)
                throw std::invalid_argument("Duplicate audio manifest key");
            return true;
        };
        auto manifest = ManifestValue(Json::parse(text, callback));
        const auto valid = ValidateAudioManifest1(manifest);
        if (!valid.succeeded())
            return *valid.error();
        return manifest;
    } catch (const MetadataJsonBudgetExceeded&) {
        return MetadataError("Audio manifest JSON memory budget exceeded", EngineErrorCode::ResourceBudgetExceeded);
    } catch (...) {
        return MetadataError("Invalid bounded audio manifest");
    }
}
Outcome<std::monostate> ValidateMonitorSelection(const MonitorSelectionSnapshot& selection,
                                                 const AudioManifest1& manifest) {
    const auto valid = ValidateAudioManifest1(manifest);
    if (!valid.succeeded())
        return *valid.error();
    if (selection.media_id != manifest.media_id || selection.revision.value == 0 ||
        selection.tracks.size() > kMaxAudioTracks)
        return MetadataError("Monitor selection belongs to a different media identity",
                             EngineErrorCode::SourceIntegrityFailure);
    std::set<AudioTrackId> selected;
    for (const auto& track : selection.tracks) {
        if (!ValidGain(track.gain_linear) || track.gain_linear > GainDbToLinear(kMaxGainDb, false) ||
            !selected.insert(track.track_id).second ||
            std::none_of(manifest.tracks.begin(), manifest.tracks.end(),
                         [&](const auto& value) { return value.container.track_id == track.track_id; }))
            return MetadataError("Monitor selection has an invalid or missing track");
    }
    bool recorded_user_selection = manifest.monitor_selection_origin == MonitorSelectionOrigin::User &&
                                   selection.tracks.size() == manifest.default_monitor_track_ids.size();
    for (const auto& id : manifest.default_monitor_track_ids)
        recorded_user_selection &= selected.contains(id);
    if (HasOverlap(selection.tracks, manifest) && !selection.overlap_confirmed && !recorded_user_selection)
        return MetadataError("Overlapping monitor selection requires confirmation");
    return std::monostate{};
}

namespace {
struct HeaderTrack {
    uint64_t number = 0;
    uint64_t uid = 0;
    uint64_t type = 0;
};
struct HeaderElement {
    uint32_t id = 0;
    uint64_t size = 0;
    size_t payload = 0;
    bool unknown = false;
};
HeaderElement Element(std::span<const uint8_t> bytes, size_t at) {
    HeaderElement result;
    const auto read = [&]() {
        if (at == bytes.size())
            throw std::invalid_argument("Truncated Matroska header");
        return bytes[at++];
    };
    const auto first = read();
    uint8_t mask = 0x80;
    unsigned length = 1;
    while (!(first & mask) && length <= 4) {
        mask >>= 1;
        ++length;
    }
    if (length > 4)
        throw std::invalid_argument("Invalid Matroska element ID");
    result.id = first;
    for (unsigned i = 1; i < length; ++i)
        result.id = (result.id << 8) | read();
    const auto size_first = read();
    mask = 0x80;
    length = 1;
    while (!(size_first & mask) && length <= 8) {
        mask >>= 1;
        ++length;
    }
    if (length > 8)
        throw std::invalid_argument("Invalid Matroska element length");
    result.size = size_first & (mask - 1);
    for (unsigned i = 1; i < length; ++i)
        result.size = (result.size << 8) | read();
    result.unknown = result.size == ((uint64_t{1} << (7 * length)) - 1);
    result.payload = at;
    return result;
}
std::vector<HeaderTrack> ReadHeaderTracks(const std::filesystem::path& path, const AVFormatContext* input,
                                          const BackgroundIoAdmission& before_io, const std::atomic<bool>* cancelled) {
    constexpr uint64_t kHeaderBytes = 16 * 1024 * 1024;
    constexpr uint64_t kTracksBytes = 2 * 1024 * 1024;
    bool denied = false;
    std::unique_ptr<BudgetedMediaAvio> budgeted;
    std::ifstream file;
    int64_t end = -1;
    if (before_io || cancelled) {
        budgeted = BudgetedMediaAvio::Open(path, false, &before_io, cancelled, &denied);
        if (budgeted)
            end = avio_size(budgeted->context());
    } else {
        file.open(path, std::ios::binary);
        file.seekg(0, std::ios::end);
        if (file)
            end = static_cast<int64_t>(file.tellg());
    }
    if (end < 0)
        throw std::invalid_argument("Cannot read Matroska header identity");
    const auto length = static_cast<uint64_t>(end);
    const auto read_at = [&](uint64_t offset, uint8_t* bytes, size_t count) {
        if (budgeted) {
            if (avio_seek(budgeted->context(), static_cast<int64_t>(offset), SEEK_SET) < 0)
                return size_t{0};
            const int read = avio_read(budgeted->context(), bytes, static_cast<int>(count));
            return read > 0 ? static_cast<size_t>(read) : size_t{0};
        }
        file.clear();
        file.seekg(static_cast<std::streamoff>(offset));
        file.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(count));
        return static_cast<size_t>(file.gcount());
    };
    uint64_t position = 0;
    bool segment = false;
    std::vector<uint8_t> tracks;
    for (size_t count = 0; count < 4096 && position < (std::min)(length, kHeaderBytes); ++count) {
        std::array<uint8_t, 12> header{};
        const auto header_read = read_at(position, header.data(), header.size());
        const auto element = Element({header.data(), header_read}, 0);
        const auto payload = position + element.payload;
        if (payload > length || (!element.unknown && element.size > length - payload))
            throw std::invalid_argument("Matroska header element exceeds file bounds");
        if (element.id == 0x18538067 && !segment) {
            segment = true;
            position = payload;
            continue;
        }
        if (segment && element.id == 0x1654AE6B) {
            if (element.unknown || element.size > kTracksBytes || payload > kHeaderBytes ||
                element.size > kHeaderBytes - payload)
                throw std::invalid_argument("Matroska tracks exceed header budget");
            tracks.resize(static_cast<size_t>(element.size));
            if (read_at(payload, tracks.data(), tracks.size()) != tracks.size())
                throw std::invalid_argument("Truncated Matroska tracks");
            break;
        }
        if (element.unknown || element.id == 0x1F43B675 || payload > kHeaderBytes ||
            element.size > kHeaderBytes - payload)
            throw std::invalid_argument("Matroska tracks are unavailable within header budget");
        position = payload + element.size;
    }
    if (tracks.empty())
        throw std::invalid_argument("Matroska tracks are missing");
    std::vector<HeaderTrack> result;
    std::set<uint64_t> numbers, uids;
    for (size_t at = 0; at < tracks.size();) {
        const auto entry = Element(tracks, at);
        if (entry.unknown || entry.size > tracks.size() - entry.payload)
            throw std::invalid_argument("Invalid Matroska track entry length");
        const auto end_entry = entry.payload + static_cast<size_t>(entry.size);
        if (entry.id == 0xAE) {
            if (result.size() == 32)
                throw std::invalid_argument("Matroska track count exceeds budget");
            HeaderTrack track;
            size_t fields = 0;
            std::set<uint32_t> identity_fields;
            for (size_t field = entry.payload; field < end_entry;) {
                const auto item = Element(std::span(tracks).first(end_entry), field);
                if (++fields > 128 || item.unknown || item.size > end_entry - item.payload)
                    throw std::invalid_argument("Invalid Matroska track field");
                uint64_t* value = item.id == 0xD7     ? &track.number
                                  : item.id == 0x73C5 ? &track.uid
                                  : item.id == 0x83   ? &track.type
                                                      : nullptr;
                if (value) {
                    if (!identity_fields.insert(item.id).second || item.size == 0 || item.size > 8)
                        throw std::invalid_argument("Duplicate or invalid Matroska track identity");
                    for (size_t byte = item.payload; byte < item.payload + item.size; ++byte)
                        *value = (*value << 8) | tracks[byte];
                }
                field = item.payload + static_cast<size_t>(item.size);
            }
            if (!track.number || !track.uid || !numbers.insert(track.number).second || !uids.insert(track.uid).second ||
                (track.type != 1 && track.type != 2))
                throw std::invalid_argument("Ambiguous or unsupported Matroska track mapping");
            result.push_back(track);
        }
        at = end_entry;
    }
    if (!input || result.size() != input->nb_streams)
        throw std::invalid_argument("Physical and decoded Matroska stream counts differ");
    // TrackNumber belongs to the demuxer's private track map. AVStream::id
    // does not expose it, so stream association uses TrackEntry order and type.
    for (size_t i = 0; i < result.size(); ++i)
        if (input->streams[i]->codecpar->codec_type != (result[i].type == 1 ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO))
            throw std::invalid_argument("Physical and decoded Matroska stream order differs");
    return result;
}
bool MatchHeaderTracks(const AudioManifest1& manifest, const std::vector<HeaderTrack>& header) {
    size_t audio = 0;
    for (const auto& physical : header) {
        if (physical.type != 2)
            continue;
        const auto track = std::find_if(manifest.tracks.begin(), manifest.tracks.end(), [audio](const auto& value) {
            return value.container.stream_index.value == audio;
        });
        if (track == manifest.tracks.end() || track->container.track_uid != physical.uid)
            return false;
        ++audio;
    }
    return audio == manifest.tracks.size();
}
std::vector<const AVStream*> AudioStreams(const AVFormatContext* input) {
    std::vector<const AVStream*> result;
    if (input)
        for (unsigned index = 0; index < input->nb_streams; ++index)
            if (input->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                result.push_back(input->streams[index]);
                if (result.size() > kMaxAudioTracks)
                    break;
            }
    return result;
}
const char* Metadata(const AVDictionary* dictionary, const char* name) {
    const auto* entry = av_dict_get(dictionary, name, nullptr, 0);
    return entry && entry->value ? entry->value : nullptr;
}
std::string TrackName(const AVStream* stream) {
    if (const auto* name = Metadata(stream->metadata, "title"))
        return name;
    if (const auto* name = Metadata(stream->metadata, "handler_name"))
        if (std::string_view(name) != "SoundHandler")
            return name;
    return {};
}
bool Matroska(const AVFormatContext* input) {
    return input && input->iformat && std::string_view(input->iformat->name).find("matroska") != std::string_view::npos;
}
MediaCodecDescriptor ActualCodec(const AVCodecParameters* codec) {
    MediaCodecDescriptor result;
    switch (codec->codec_id) {
    case AV_CODEC_ID_AAC:
        result.codec_id = "A_AAC";
        break;
    case AV_CODEC_ID_OPUS:
        result.codec_id = "A_OPUS";
        break;
    case AV_CODEC_ID_FLAC:
        result.codec_id = "A_FLAC";
        break;
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S32LE:
        result.codec_id = "A_PCM/INT/LIT";
        break;
    case AV_CODEC_ID_PCM_F32LE:
    case AV_CODEC_ID_PCM_F64LE:
        result.codec_id = "A_PCM/FLOAT/IEEE";
        break;
    default:
        result.codec_id = avcodec_get_name(codec->codec_id);
        break;
    }
    if (codec->extradata && codec->extradata_size > 0 &&
        static_cast<size_t>(codec->extradata_size) <= kCodecPrivateBytes)
        result.codec_private.assign(codec->extradata, codec->extradata + codec->extradata_size);
    if (codec->codec_id == AV_CODEC_ID_FLAC && result.codec_private.size() == 34) {
        const std::array<uint8_t, 8> header{'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
        result.codec_private.insert(result.codec_private.begin(), header.begin(), header.end());
    }
    result.sample_rate = static_cast<uint32_t>((std::max)(0, codec->sample_rate));
    result.channels = static_cast<uint32_t>((std::max)(0, codec->ch_layout.nb_channels));
    result.bit_depth = static_cast<uint32_t>(
        (std::max)(0, codec->bits_per_raw_sample > 0 ? codec->bits_per_raw_sample : codec->bits_per_coded_sample));
    // Transform codecs have no fixed coded sample precision. Container sample
    // size fields describe a compatibility entry rather than encoded precision.
    if (codec->codec_id == AV_CODEC_ID_AAC || codec->codec_id == AV_CODEC_ID_OPUS)
        result.bit_depth = 0;
    result.codec_delay_samples = static_cast<uint32_t>((std::max)(0, codec->initial_padding));
    result.channel_layout = LayoutName(codec->ch_layout);
    // Matroska PCM carries channel count without a layout mask. Mono and stereo
    // have an unambiguous supported layout; wider unspecified layouts do not.
    if (codec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC && result.channels >= 1 && result.channels <= 2)
        result.channel_layout = DefaultAudioChannelLayout(result.channels);
    return result;
}
bool CodecMatches(const MediaCodecDescriptor& declared, const AVCodecParameters* codec) {
    if (codec->extradata_size < 0 || static_cast<size_t>(codec->extradata_size) > kCodecPrivateBytes)
        return false;
    const auto actual = ActualCodec(codec);
    return declared.codec_id == actual.codec_id && declared.codec_private == actual.codec_private &&
           declared.sample_rate == actual.sample_rate && declared.channels == actual.channels &&
           declared.bit_depth == actual.bit_depth && !declared.channel_layout.empty() &&
           declared.channel_layout == actual.channel_layout &&
           declared.codec_delay_samples == actual.codec_delay_samples;
}
void PopulateActual(AudioMetadataReadResult& result, const AVFormatContext* input) {
    if (!ValidId(result.selection_media_id))
        result.selection_media_id = NewId<MediaId>();
    const auto streams = AudioStreams(input);
    for (size_t index = 0; index < streams.size() && index < kMaxAudioTracks; ++index) {
        ContainerAudioTrack track;
        track.track_id = NewId<AudioTrackId>();
        track.stream_index = {static_cast<uint32_t>(index)};
        track.track_uid = streams[index]->id > 1 ? static_cast<uint64_t>(streams[index]->id) : 2 + index;
        track.name = TrackName(streams[index]);
        track.codec = ActualCodec(streams[index]->codecpar);
        result.actual_tracks.push_back(std::move(track));
    }
}
} // namespace

Outcome<std::string> SerializeAudioTrackProvenance(const AudioManifest1& manifest, const AudioTrackId& track_id) {
    const auto valid = ValidateAudioManifest1(manifest);
    if (!valid.succeeded())
        return *valid.error();
    try {
        MetadataJsonQuotaScope json_quota;
        const auto json = ManifestJson(manifest);
        for (const auto& track : json["tracks"])
            if (track["track_id"] == ToUuid(track_id)) {
                Json sources = Json::array();
                for (const auto& source : json["sources"])
                    for (const auto& id : track["source_ids"])
                        if (source["id"] == id)
                            sources.push_back(source);
                const auto serialized = Json{{"version", 1}, {"track", track}, {"sources", std::move(sources)}}.dump();
                return std::string(serialized.data(), serialized.size());
            }
        return MetadataError("Audio provenance track is missing");
    } catch (const MetadataJsonBudgetExceeded&) {
        return MetadataError("Audio provenance JSON memory budget exceeded", EngineErrorCode::ResourceBudgetExceeded);
    } catch (...) {
        return MetadataError("Cannot serialize audio provenance");
    }
}
AudioManifest1 DeriveAudioManifest(const AudioManifest1& source) {
    auto derived = source;
    derived.media_id = NewId<MediaId>();
    derived.derived_from_media_ids = {source.media_id};
    return derived;
}
std::string DefaultAudioChannelLayout(uint32_t channels) {
    if (channels == 0 || channels > 64)
        return {};
    AVChannelLayout layout{};
    av_channel_layout_default(&layout, static_cast<int>(channels));
    auto name = LayoutName(layout);
    av_channel_layout_uninit(&layout);
    return name;
}
Outcome<std::monostate> ValidateAudioManifestStreams(const AudioManifest1& manifest, const AVFormatContext* input) {
    const auto valid = ValidateAudioManifest1(manifest);
    if (!valid.succeeded())
        return *valid.error();
    const auto streams = AudioStreams(input);
    if (streams.size() != manifest.tracks.size())
        return MetadataError("Audio manifest track count differs from actual media");
    for (const auto& track : manifest.tracks) {
        const auto* stream = streams[track.container.stream_index.value];
        if (!CodecMatches(track.container.codec, stream->codecpar) || track.container.name != TrackName(stream)) {
            const auto actual = ActualCodec(stream->codecpar);
            const auto& declared = track.container.codec;
            const std::string reason =
                "Audio manifest differs from actual media: codec=" + declared.codec_id + "/" + actual.codec_id +
                ", private_size=" + std::to_string(declared.codec_private.size()) + "/" +
                std::to_string(actual.codec_private.size()) +
                ", private_equal=" + std::to_string(declared.codec_private == actual.codec_private) +
                ", rate=" + std::to_string(declared.sample_rate) + "/" + std::to_string(actual.sample_rate) +
                ", channels=" + std::to_string(declared.channels) + "/" + std::to_string(actual.channels) +
                ", layout=" + declared.channel_layout + "/" + actual.channel_layout +
                ", precision=" + std::to_string(declared.bit_depth) + "/" + std::to_string(actual.bit_depth) +
                ", delay=" + std::to_string(declared.codec_delay_samples) + "/" +
                std::to_string(actual.codec_delay_samples) +
                ", name_equal=" + std::to_string(track.container.name == TrackName(stream));
            return MetadataError(reason.c_str());
        }
        if (Matroska(input) && Metadata(input->metadata, "EXOSNAP_AUDIO_MANIFEST")) {
            const auto* id = Metadata(stream->metadata, "EXOSNAP_TRACK_ID");
            const auto* role = Metadata(stream->metadata, "EXOSNAP_ROLE");
            const auto* provenance = Metadata(stream->metadata, "EXOSNAP_PROVENANCE");
            const auto expected = SerializeAudioTrackProvenance(manifest, track.container.track_id);
            if (!id || std::string_view(id) != ToUuid(track.container.track_id) || !role ||
                role != track.container.role || !provenance || !expected.succeeded() ||
                std::string_view(provenance) != *expected.value())
                return MetadataError("Matroska track tags contradict the audio manifest");
        }
    }
    return std::monostate{};
}
Outcome<std::monostate> WriteAudioManifestMetadata(AVFormatContext* output, const AudioManifest1& manifest) {
    const auto serialized = SerializeAudioManifest1(manifest);
    if (!serialized.succeeded())
        return *serialized.error();
    const auto streams = AudioStreams(output);
    if (!output || streams.size() != manifest.tracks.size())
        return MetadataError("Cannot map audio manifest to output streams");
    if (av_dict_set(&output->metadata, "EXOSNAP_AUDIO_MANIFEST", serialized.value()->c_str(), 0) < 0)
        return MetadataError("Cannot write audio manifest tag");
    for (const auto& track : manifest.tracks) {
        auto* stream = const_cast<AVStream*>(streams[track.container.stream_index.value]);
        const auto provenance = SerializeAudioTrackProvenance(manifest, track.container.track_id);
        if (!provenance.succeeded())
            return *provenance.error();
        if (av_dict_set(&stream->metadata, "title", track.container.name.c_str(), 0) < 0 ||
            av_dict_set(&stream->metadata, "handler_name", track.container.name.c_str(), 0) < 0 ||
            av_dict_set(&stream->metadata, "EXOSNAP_TRACK_ID", ToUuid(track.container.track_id).c_str(), 0) < 0 ||
            av_dict_set(&stream->metadata, "EXOSNAP_ROLE", track.container.role.c_str(), 0) < 0 ||
            av_dict_set(&stream->metadata, "EXOSNAP_PROVENANCE", provenance.value()->c_str(), 0) < 0)
            return MetadataError("Cannot write audio track metadata");
    }
    return std::monostate{};
}
AudioMetadataReadResult ReadAudioMediaMetadata(const std::filesystem::path& path, const AVFormatContext* input,
                                               const std::function<bool()>& continue_io,
                                               const BackgroundIoAdmission& before_io,
                                               const std::atomic<bool>* cancelled) {
    AudioMetadataReadResult result;
    const auto lease = AcquireMediaReadLease(path);
    PopulateActual(result, input);
    const auto unverified = [&]() {
        result.monitoring_confirmation_required = !AudioStreams(input).empty();
        if (lease.succeeded()) {
            const auto identity = HashFinalMedia(path, continue_io, before_io, cancelled);
            if (identity.succeeded()) {
                auto binding = std::make_shared<UnverifiedAudioAssetBinding>();
                binding->file_identity = identity.value()->file_identity;
                binding->file_size = identity.value()->file_size;
                binding->sha256 = identity.value()->sha256;
                binding->actual_tracks = result.actual_tracks;
                result.unverified_asset_binding = std::move(binding);
            } else {
                result.warnings.push_back(identity.error()->context);
            }
        } else {
            result.warnings.push_back(lease.error()->context);
        }
        return result;
    };
    if (!lease.succeeded()) {
        result.status = AudioMetadataStatus::Invalid;
        return unverified();
    }
    std::vector<HeaderTrack> header;
    if (Matroska(input)) {
        try {
            header = ReadHeaderTracks(path, input, before_io, cancelled);
            size_t audio = 0;
            for (const auto& track : header)
                if (track.type == 2 && audio < result.actual_tracks.size())
                    result.actual_tracks[audio++].track_uid = track.uid;
        } catch (const std::exception& error) {
            result.status = AudioMetadataStatus::Invalid;
            result.warnings.push_back(error.what());
            return unverified();
        }
    }
    if (const auto* tag = input ? Metadata(input->metadata, "EXOSNAP_AUDIO_MANIFEST") : nullptr;
        tag && Matroska(input)) {
        auto manifest = ParseAudioManifest1(tag);
        const auto valid = manifest.succeeded() ? ValidateAudioManifestStreams(*manifest.value(), input)
                                                : Outcome<std::monostate>{*manifest.error()};
        if (!valid.succeeded() || !MatchHeaderTracks(*manifest.value(), header)) {
            result.status = AudioMetadataStatus::Invalid;
            result.warnings.push_back(valid.succeeded() ? "Audio manifest contradicts physical TrackUID mapping"
                                                        : valid.error()->context);
            return unverified();
        }
        result.status = AudioMetadataStatus::Verified;
        result.selection_media_id = manifest.value()->media_id;
        result.manifest = std::move(*manifest.value());
        result.actual_tracks.clear();
        for (const auto& track : result.manifest->tracks)
            result.actual_tracks.push_back(track.container);
        std::error_code error;
        if (std::filesystem::exists(MediaIdentitySidecarPath(path), error)) {
            const auto sidecar = ReadMediaIdentitySidecar(path, continue_io, before_io, cancelled);
            const auto canonical = SerializeAudioManifest1(*result.manifest);
            const auto external = sidecar.manifest ? SerializeAudioManifest1(*sidecar.manifest)
                                                   : Outcome<std::string>{MetadataError("Missing sidecar authority")};
            if (sidecar.status != AudioMetadataStatus::Verified || !external.succeeded() || !canonical.succeeded() ||
                *external.value() != *canonical.value())
                result.warnings.push_back(
                    "External sidecar is stale; valid Matroska container metadata remains authoritative");
            else
                result.references = sidecar.references;
        }
    } else {
        auto sidecar = ReadMediaIdentitySidecar(path, continue_io, before_io, cancelled);
        const auto valid = sidecar.manifest ? ValidateAudioManifestStreams(*sidecar.manifest, input)
                                            : Outcome<std::monostate>{MetadataError("Missing sidecar")};
        if (sidecar.status == AudioMetadataStatus::Verified && sidecar.manifest && valid.succeeded() &&
            (!Matroska(input) || MatchHeaderTracks(*sidecar.manifest, header))) {
            result = std::move(sidecar);
        } else {
            result.status =
                sidecar.status == AudioMetadataStatus::Verified ? AudioMetadataStatus::Invalid : sidecar.status;
            result.warnings = std::move(sidecar.warnings);
            if (sidecar.manifest && !valid.succeeded())
                result.warnings.push_back(valid.error()->context);
            result.monitoring_confirmation_required = !AudioStreams(input).empty();
            if (result.monitoring_confirmation_required)
                result.warnings.push_back("Audio monitoring requires explicit track confirmation");
            return unverified();
        }
    }
    result.monitoring_confirmation_required =
        result.manifest->monitor_selection_origin == MonitorSelectionOrigin::Default &&
        HasOverlap(DefaultSelection(*result.manifest), *result.manifest);
    return result;
}

} // namespace exosnap::engine
