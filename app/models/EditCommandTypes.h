#pragma once

#include "RecordingMarker.h"
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/edit_types.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace exosnap::edit {

using Id = uint64_t;
using Time = int64_t;
using Revision = uint64_t;
using StreamRef = engine::TimelineStreamRef;
using CurveSpec = engine::CurveSpec;
using EdgeFade = engine::EdgeFade;
using AudioGainLaw = engine::AudioGainLaw;
using TransitionKind = engine::JunctionKind;
using JunctionAnchor = engine::JunctionAnchor;

enum class TrackType { Video, Audio };
enum class AssetState { Available, Missing, Failed };

struct Span {
    Time in_us = 0;
    Time out_us = 0;
    bool operator==(const Span&) const = default;
};

struct RationalFps {
    uint32_t num = 0;
    uint32_t den = 1;
    [[nodiscard]] bool valid() const {
        return num && den;
    }
    [[nodiscard]] RationalFps reduced() const {
        if (!valid())
            return {};
        const auto divisor = std::gcd(num, den);
        return {num / divisor, den / divisor};
    }
    [[nodiscard]] std::optional<Time> frameTime(int64_t frame) const {
        if (!valid() || frame < 0)
            return std::nullopt;
        const auto scaled = scale(static_cast<uint64_t>(frame), uint64_t{1'000'000} * den, num);
        return scaled ? std::optional<Time>{static_cast<Time>(scaled->first)} : std::nullopt;
    }
    [[nodiscard]] std::optional<Time> nextFrame(Time time) const {
        if (!valid() || time < 0 || time == (std::numeric_limits<Time>::max)())
            return std::nullopt;
        const auto index = ceiling(static_cast<uint64_t>(time + 1), num, uint64_t{1'000'000} * den);
        return index ? frameTime(*index) : std::nullopt;
    }
    [[nodiscard]] std::optional<Time> previousFrame(Time time) const {
        if (!valid() || time <= 0)
            return std::nullopt;
        const auto index = ceiling(static_cast<uint64_t>(time), num, uint64_t{1'000'000} * den);
        return index && *index ? frameTime(*index - 1) : std::nullopt;
    }
    bool operator==(const RationalFps&) const = default;

  private:
    static std::optional<std::pair<uint64_t, uint64_t>> scale(uint64_t value, uint64_t multiplier, uint64_t divisor) {
        uint64_t quotient = 0;
        uint64_t remainder = 0;
        constexpr auto limit = static_cast<uint64_t>((std::numeric_limits<Time>::max)());
        for (int bit = 63; bit >= 0; --bit) {
            if (quotient > limit / 2)
                return std::nullopt;
            quotient *= 2;
            remainder *= 2;
            if ((value >> bit) & 1u) {
                if (multiplier / divisor > limit - quotient)
                    return std::nullopt;
                quotient += multiplier / divisor;
                remainder += multiplier % divisor;
            }
            if (remainder / divisor > limit - quotient)
                return std::nullopt;
            quotient += remainder / divisor;
            remainder %= divisor;
        }
        return std::pair{quotient, remainder};
    }
    static std::optional<int64_t> ceiling(uint64_t value, uint64_t multiplier, uint64_t divisor) {
        const auto scaled = scale(value, multiplier, divisor);
        if (!scaled || (scaled->second && scaled->first == static_cast<uint64_t>((std::numeric_limits<Time>::max)())))
            return std::nullopt;
        return static_cast<int64_t>(scaled->first + (scaled->second != 0));
    }
};

enum class ResultOutMode { Fixed, FollowTimelineEnd };
struct ResultRange {
    Time in_us = 0;
    ResultOutMode out_mode = ResultOutMode::FollowTimelineEnd;
    Time out_us = 0;
    bool operator==(const ResultRange&) const = default;
};

enum class InputMixAuthority { VerifiedRecordedDefault, UserConfirmed };
struct InputMixSelection {
    std::array<uint8_t, 32> asset_identity_fingerprint{};
    std::vector<engine::AudioTrackId> selected_stream_identities;
    InputMixAuthority authority = InputMixAuthority::UserConfirmed;
    std::array<uint8_t, 32> provenance_fingerprint{};
    Id original_recipe_group_id = 0;
};

struct OriginalRecipeAnchor {
    Id group_id = 0;
    std::vector<StreamRef> streams;
    Span source_span;
};

struct SourceStreamDescriptor {
    StreamRef ref;
    Span source_span;
    Time timeline_offset_us = 0;
    engine::MediaCodecDescriptor codec;
    RationalFps fps;
    std::string name;
    std::string role;
    std::vector<engine::AudioSourceId> source_ids;
};

struct ValidatedSourceDescriptor {
    engine::FileIdentity file_identity;
    std::array<uint8_t, 32> asset_identity_fingerprint{};
    std::vector<SourceStreamDescriptor> streams;
};

struct Asset {
    Id id = 0;
    std::wstring path;
    std::string name;
    Time duration = 0;
    int width = 0;
    int height = 0;
    double fps = 0;
    int fps_num = 0;
    int fps_den = 1;
    bool audio = true;
    AssetState state = AssetState::Available;
    std::vector<RecordingMarker> markers;
    std::optional<engine::MonitorSelectionSnapshot> monitor_selection;
    std::shared_ptr<const ValidatedSourceDescriptor> source_descriptor;
    std::optional<InputMixSelection> input_mix_selection;
    std::optional<OriginalRecipeAnchor> original_recipe;
};

struct Clip {
    Id id = 0;
    Id asset = 0;
    Id track = 0;
    Id group = 0;
    Time start = 0;
    Time source_in = 0;
    Time source_out = 0;
    [[nodiscard]] Time duration() const {
        return source_out - source_in;
    }
    [[nodiscard]] Time end() const {
        return start + duration();
    }
    std::optional<StreamRef> stream;
    bool enabled = true;
    double gain_db = 0;
    double opacity = 1;
    EdgeFade fade_in;
    EdgeFade fade_out;
    bool original_enabled = true;
    bool operator==(const Clip&) const = default;
};

struct Track {
    Id id = 0;
    TrackType type = TrackType::Video;
    std::string name;
    bool edit_enabled = true;
    double gain_db = 0;
    bool operator==(const Track&) const = default;
};

struct Transition {
    Id outgoing = 0;
    Id incoming = 0;
    TransitionKind kind = TransitionKind::Crossfade;
    Time duration = 0;
    Id id = 0;
    Id track = 0;
    Time in_us = 0;
    Time out_us = 0;
    Time cut_us = 0;
    CurveSpec curve;
    AudioGainLaw audio_law = AudioGainLaw::EqualGain;
    JunctionAnchor anchor = JunctionAnchor::FullOverlap;
    bool operator==(const Transition&) const = default;
};

enum class EditErrorCode {
    InvalidTarget,
    StaleRevision,
    InvalidTime,
    SourceBounds,
    WrongTrackKind,
    UnsupportedOverlap,
    TransitionBoundaryConflict,
    RippleConflict,
    InvalidCurve,
    NonfiniteValue,
    MixSelectionRequired,
    UnsupportedAudioLayout,
    SourceChanged,
    MissingSource,
    UnsupportedProfile,
    BoundaryConsentRequired,
    ResourceLimitExceeded,
    Busy,
    Cancelled,
    IoFailure
};
struct EditError {
    EditErrorCode code = EditErrorCode::InvalidTarget;
    std::vector<Id> affected_ids;
    std::vector<Time> arguments;
};
enum class CommandStatus { Applied, NoOp, Rejected };
struct CommandResult {
    CommandStatus status = CommandStatus::Rejected;
    Revision revision = 0;
    std::optional<EditError> error;
    std::vector<Id> changed_ids;
    std::string undo_label_key;
};

struct AddAssetGroup {
    Id asset_id = 0;
    Time placement_us = 0;
    Id video_track_id = 0;
    std::vector<Id> audio_track_ids;
    std::optional<InputMixSelection> input_mix_selection;
    bool include_video = true;
};
struct Split {
    std::vector<Id> targets;
    Time at_us = 0;
    bool expand_linked = true;
};
struct LiftClips {
    std::vector<Id> targets;
    bool expand_linked = true;
};
struct RippleRemoveClips {
    std::vector<Id> targets;
    bool expand_linked = true;
};
struct LiftRange {
    Span span;
};
struct RippleRemoveRange {
    Span span;
};
struct KeepRange {
    Span span;
};
enum class TrimEdge { Left, Right };
enum class TrimMode { Ripple, Fixed };
struct Trim {
    Id lead_id = 0;
    TrimEdge edge = TrimEdge::Right;
    Time delta_us = 0;
    TrimMode mode = TrimMode::Ripple;
    std::vector<Id> targets;
    bool expand_linked = true;
};
struct Move {
    std::vector<Id> targets;
    Time delta_us = 0;
    std::vector<std::pair<Id, Id>> target_tracks;
    std::vector<Track> new_tracks;
    bool expand_linked = true;
};
struct CreateTrack {
    TrackType kind = TrackType::Video;
    size_t index = 0;
    std::string name;
};
struct DeleteTrack {
    Id track_id = 0;
    std::optional<Revision> confirmed_contents_revision;
};
struct ReorderTrack {
    Id track_id = 0;
    size_t index = 0;
};
struct SetResultRange {
    ResultRange range;
};
struct Unlink {
    std::vector<Id> targets;
};
struct Relink {
    std::vector<Id> targets;
};
struct SetTrackEnabled {
    Id id = 0;
    bool enabled = true;
};
struct SetClipEnabled {
    Id id = 0;
    bool enabled = true;
};
enum class GainTarget { Track, Clip };
struct SetGain {
    GainTarget target = GainTarget::Clip;
    Id id = 0;
    double gain_db = 0;
};
struct SetClipOpacity {
    Id id = 0;
    double opacity = 1;
};
enum class FadeEdge { In, Out };
struct SetFade {
    Id clip_id = 0;
    FadeEdge edge = FadeEdge::In;
    EdgeFade fade;
};
using EditCommand = std::variant<AddAssetGroup, Split, LiftClips, RippleRemoveClips, LiftRange, RippleRemoveRange,
                                 KeepRange, Trim, Move, CreateTrack, DeleteTrack, ReorderTrack, SetResultRange, Unlink,
                                 Relink, SetTrackEnabled, SetClipEnabled, SetGain, SetClipOpacity, SetFade>;

struct EditSnapshot {
    Revision revision = 0;
    std::vector<std::shared_ptr<const Asset>> assets;
    std::vector<Track> tracks;
    std::vector<Clip> clips;
    std::vector<Transition> junctions;
    ResultRange result_range;
    RationalFps output_fps;
    int output_width = 0;
    int output_height = 0;
    Id id_high_watermark = 0;
};

struct EditPreview {
    Revision base_revision = 0;
    bool valid = false;
    std::optional<EditError> error;
    std::vector<Track> tracks;
    std::vector<Clip> clips;
    std::vector<Transition> junctions;
    Time effective_delta_us = 0;
};

EditPreview PreviewEdit(const EditSnapshot& snapshot, const EditCommand& command);

} // namespace exosnap::edit
