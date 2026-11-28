#pragma once

#include "EditCommandTypes.h"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <span>
#include <string_view>
#include <type_traits>

namespace exosnap::edit::project {

inline constexpr size_t MaxBytes = 16 * 1024 * 1024;
inline constexpr size_t MaxEntries = 16384;
enum class Status { Ok, Invalid, Unsupported };

namespace detail {

// Each field has a fixed little-endian wire representation. Counts are checked
// before allocation, including a shared allocation budget for nested vectors.
struct Reader {
    std::span<const uint8_t> data;
    size_t budget = 4 * MaxBytes;
    bool ok = true;

    void bytes(uint8_t* target, size_t count) {
        if (count > data.size()) {
            ok = false;
            return;
        }
        std::copy_n(data.begin(), count, target);
        data = data.subspan(count);
    }
    bool allocate(size_t count, size_t width) {
        if (count > budget / width || count > data.size()) {
            ok = false;
            return false;
        }
        budget -= count * width;
        return true;
    }
};

struct Writer {
    std::vector<uint8_t> data;
    bool ok = true;
    void bytes(const uint8_t* source, size_t count) {
        if (count > MaxBytes - data.size()) {
            ok = false;
            return;
        }
        data.insert(data.end(), source, source + count);
    }
};

template <class Archive, class T> void value(Archive& archive, T& item);
template <class Archive, class... Fields> void fields(Archive& archive, Fields&... items) {
    (value(archive, items), ...);
}

template <class A> void record(A& archive, Span& item) {
    fields(archive, item.in_us, item.out_us);
}
template <class A> void record(A& archive, RationalFps& item) {
    fields(archive, item.num, item.den);
}
template <class A> void record(A& archive, TimelineEnd& item) {
    fields(archive, item.mode, item.end_us);
}
template <class A> void record(A& archive, ResultRange& item) {
    fields(archive, item.in_us, item.out_mode, item.out_us);
}
template <class A> void record(A& archive, RecordingMarker& item) {
    fields(archive, item.time_ms, item.type, item.label);
}
template <class A> void record(A& archive, engine::FileIdentity& item) {
    fields(archive, item.volume_serial, item.file_id);
}
template <class A> void record(A& archive, engine::MediaCodecDescriptor& item) {
    fields(archive, item.codec_id, item.codec_private, item.sample_rate, item.channels, item.bit_depth,
           item.codec_delay_samples, item.channel_layout);
}
template <class A> void record(A& archive, StreamRef& item) {
    fields(archive, item.asset_id, item.stream_identity, item.container_index, item.kind);
}
template <class A> void record(A& archive, SourceStreamDescriptor& item) {
    fields(archive, item.ref, item.source_span, item.timeline_offset_us, item.codec, item.fps, item.name, item.role,
           item.source_ids);
}
template <class A> void record(A& archive, ValidatedSourceDescriptor& item) {
    fields(archive, item.file_identity, item.asset_identity_fingerprint, item.audio_identities_verified, item.streams);
}
template <class A> void record(A& archive, InputMixSelection& item) {
    fields(archive, item.asset_identity_fingerprint, item.selected_stream_identities, item.authority,
           item.provenance_fingerprint, item.original_recipe_group_id);
}
template <class A> void record(A& archive, OriginalRecipeAnchor& item) {
    fields(archive, item.group_id, item.streams, item.source_span);
}
template <class A> void record(A& archive, Asset& item) {
    fields(archive, item.id, item.path, item.name, item.duration, item.width, item.height, item.fps, item.fps_num,
           item.fps_den, item.audio, item.markers, item.source_descriptor, item.input_mix_selection,
           item.mix_decision_required, item.original_recipe, item.source_version, item.legacy_content_sha256);
}
template <class A> void record(A& archive, Track& item) {
    fields(archive, item.id, item.type, item.name, item.edit_enabled, item.gain_db);
}
template <class A> void record(A& archive, Spacer& item) {
    fields(archive, item.id, item.track, item.start_us, item.duration_us);
}
template <class A> void record(A& archive, CurveSpec& item) {
    fields(archive, item.kind, item.bias);
}
template <class A> void record(A& archive, EdgeFade& item) {
    fields(archive, item.duration_us, item.curve);
}
template <class A> void record(A& archive, engine::PropertyKey& item) {
    fields(archive, item.source_us, item.value, item.interpolation);
}
template <class A> void record(A& archive, engine::PropertyChannel& item) {
    fields(archive, item.keys);
}
template <class A> void record(A& archive, engine::VideoTransform& item) {
    fields(archive, item.x, item.y, item.scale, item.rotation);
}
template <class A> void record(A& archive, engine::RangeGainRamp& item) {
    fields(archive, item.in_sample, item.out_sample, item.ramp_samples, item.delta_db);
}
template <class A> void record(A& archive, engine::GainEnvelope& item) {
    fields(archive, item.channel, item.ranges);
}
template <class A> void record(A& archive, Clip& item) {
    fields(archive, item.id, item.asset, item.track, item.group, item.start, item.source_in, item.source_out,
           item.stream, item.enabled, item.gain_db, item.opacity, item.fade_in, item.fade_out, item.original_enabled,
           item.opacity_channel, item.transform, item.transform_channels, item.gain_envelope);
}
template <class A> void record(A& archive, Transition& item) {
    fields(archive, item.outgoing, item.incoming, item.kind, item.duration, item.id, item.track, item.in_us,
           item.out_us, item.cut_us, item.curve, item.audio_law, item.anchor, item.incoming_anchor_us);
}
template <class A> void record(A& archive, EditSnapshot& item) {
    fields(archive, item.assets, item.tracks, item.clips, item.junctions, item.result_range, item.output_fps,
           item.output_width, item.output_height, item.id_high_watermark, item.spacers, item.locked_track_ids,
           item.timeline_end, item.source_version_high_watermark);
}

template <class Archive, class T> void value(Archive& archive, T& item) {
    constexpr bool reading = std::is_same_v<Archive, Reader>;
    if (!archive.ok)
        return;
    if constexpr (std::is_same_v<T, bool> || std::is_enum_v<T>) {
        uint8_t code = static_cast<uint8_t>(item);
        archive.bytes(&code, 1);
        constexpr uint8_t maximum = std::is_same_v<T, RecordingMarkerType> || std::is_same_v<T, engine::CurveKind> ||
                                            std::is_same_v<T, engine::KeyInterpolation>
                                        ? 2
                                        : 1;
        if (code > maximum)
            archive.ok = false;
        item = static_cast<T>(code);
    } else if constexpr (std::is_integral_v<T> || std::is_same_v<T, double>) {
        auto bytes = std::bit_cast<std::array<uint8_t, sizeof(T)>>(item);
        if constexpr (std::endian::native == std::endian::big)
            std::reverse(bytes.begin(), bytes.end());
        archive.bytes(bytes.data(), bytes.size());
        if constexpr (std::endian::native == std::endian::big)
            std::reverse(bytes.begin(), bytes.end());
        item = std::bit_cast<T>(bytes);
        if constexpr (std::is_same_v<T, double>)
            archive.ok = archive.ok && std::isfinite(item);
    } else if constexpr (requires { item.bytes; }) {
        value(archive, item.bytes);
    } else if constexpr (std::is_same_v<T, std::wstring>) {
        using Unit = std::conditional_t<sizeof(wchar_t) == 2, char16_t, char32_t>;
        std::u8string utf8;
        if constexpr (!reading)
            utf8 = std::filesystem::path(std::basic_string<Unit>(item.begin(), item.end())).u8string();
        value(archive, utf8);
        if constexpr (reading) {
            if (archive.ok) {
                const auto path = std::filesystem::path(utf8);
                if constexpr (sizeof(wchar_t) == 2) {
                    const auto units = path.u16string();
                    item.assign(units.begin(), units.end());
                } else {
                    const auto units = path.u32string();
                    item.assign(units.begin(), units.end());
                }
            }
        }
    } else if constexpr (requires { item.resize(0); }) {
        using Element = typename T::value_type;
        constexpr size_t limit = sizeof(Element) == 1 ? 1024 * 1024 : MaxEntries;
        if (item.size() > limit) {
            archive.ok = false;
            return;
        }
        auto count = static_cast<uint32_t>(item.size());
        value(archive, count);
        if (!archive.ok || count > limit) {
            archive.ok = false;
            return;
        }
        if constexpr (reading) {
            if (!archive.allocate(count, sizeof(Element)))
                return;
            item.resize(count);
        }
        if constexpr (sizeof(Element) == 1 && std::is_integral_v<Element>) {
            if (count)
                archive.bytes(reinterpret_cast<uint8_t*>(item.data()), count);
        } else {
            for (auto& child : item)
                value(archive, child);
        }
    } else if constexpr (requires { item.has_value(); }) {
        bool present = item.has_value();
        value(archive, present);
        if (present) {
            if constexpr (reading)
                item.emplace();
            value(archive, *item);
        }
    } else if constexpr (requires { typename T::element_type; }) {
        bool present = static_cast<bool>(item);
        value(archive, present);
        if (present) {
            using Element = std::remove_const_t<typename T::element_type>;
            Element copy;
            if constexpr (!reading)
                copy = *item;
            value(archive, copy);
            if constexpr (reading)
                item = std::make_shared<const Element>(std::move(copy));
        }
    } else if constexpr (requires {
                             item.begin();
                             item.end();
                         }) {
        for (auto& child : item)
            value(archive, child);
    } else {
        record(archive, item);
    }
}

} // namespace detail

inline constexpr std::array<uint8_t, 8> Magic{'E', 'X', 'O', 'P', 'R', 'O', 'J', 0};

inline bool ValidMarkers(const std::vector<RecordingMarker>& markers) {
    return markers.size() <= kMaxRecordingMarkers &&
           std::all_of(markers.begin(), markers.end(), [](const auto& marker) {
               return marker.time_ms <= static_cast<uint64_t>((std::numeric_limits<Time>::max)() / 1000);
           });
}

inline std::optional<std::vector<uint8_t>> Encode(EditSnapshot snapshot, ProjectSessionState session = {}) {
    if (!ValidMarkers(session.markers))
        return std::nullopt;
    detail::Writer writer;
    writer.bytes(Magic.data(), Magic.size());
    uint32_t version = 1;
    try {
        detail::fields(writer, version, snapshot, session.markers, session.auto_crossfade);
    } catch (const std::filesystem::filesystem_error&) {
        return std::nullopt;
    }
    if (!writer.ok)
        return std::nullopt;
    return std::move(writer.data);
}

inline Status Decode(std::span<const uint8_t> bytes, EditSnapshot& result, ProjectSessionState* session = nullptr) {
    if (bytes.size() < Magic.size() + 4 || bytes.size() > MaxBytes ||
        !std::equal(Magic.begin(), Magic.end(), bytes.begin()))
        return Status::Invalid;
    detail::Reader reader{bytes.subspan(Magic.size())};
    uint32_t version = 0;
    detail::value(reader, version);
    if (version != 1)
        return Status::Unsupported;
    EditSnapshot decoded;
    ProjectSessionState decoded_session;
    try {
        detail::fields(reader, decoded, decoded_session.markers, decoded_session.auto_crossfade);
    } catch (const std::filesystem::filesystem_error&) {
        return Status::Invalid;
    }
    if (!reader.ok || !reader.data.empty() || !ValidMarkers(decoded_session.markers))
        return Status::Invalid;
    result = std::move(decoded);
    if (session)
        *session = std::move(decoded_session);
    return Status::Ok;
}

} // namespace exosnap::edit::project
