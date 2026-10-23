#pragma once

#include "RecordingMarker.h"
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/edit_types.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
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
    [[nodiscard]] std::optional<int64_t> frameIndex(Time time) const {
        if (!valid() || time < 0)
            return std::nullopt;
        const auto scaled = scale(static_cast<uint64_t>(time) + 1, num, uint64_t{1'000'000} * den);
        if (!scaled || (!scaled->second && !scaled->first))
            return std::nullopt;
        return static_cast<int64_t>(scaled->first - (scaled->second == 0));
    }
    [[nodiscard]] std::optional<int64_t> nearestFrameIndex(Time time) const {
        const auto index = frameIndex(time);
        if (!index)
            return std::nullopt;
        const auto before = frameTime(*index);
        const auto after = *index == (std::numeric_limits<int64_t>::max)() ? std::nullopt : frameTime(*index + 1);
        if (!before)
            return std::nullopt;
        return after && *after - time < time - *before ? *index + 1 : *index;
    }
    [[nodiscard]] std::optional<Time> nearestFrame(Time time) const {
        const auto index = nearestFrameIndex(time);
        return index ? frameTime(*index) : std::nullopt;
    }
    [[nodiscard]] std::optional<Time> step(Time time, int64_t offset) const {
        const auto index = nearestFrameIndex(time);
        if (!index || (offset > 0 && *index > (std::numeric_limits<int64_t>::max)() - offset) ||
            (offset < 0 && offset < -*index))
            return std::nullopt;
        return frameTime(*index + offset);
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

struct AudioSampleGrid {
    [[nodiscard]] static std::optional<Time> sampleTime(int64_t sample) {
        const auto floor = RationalFps{48000, 1}.frameTime(sample);
        if (!floor)
            return std::nullopt;
        const auto round = (static_cast<uint64_t>(sample) % 48000) * 1'000'000 % 48000 >= 24000;
        if (round && *floor == (std::numeric_limits<Time>::max)())
            return std::nullopt;
        return *floor + round;
    }
    [[nodiscard]] static std::optional<int64_t> sampleIndex(Time time) {
        const auto index = RationalFps{48000, 1}.frameIndex(time);
        if (!index)
            return std::nullopt;
        const auto boundary = sampleTime(*index);
        return boundary && *boundary <= time ? *index : *index - 1;
    }
    [[nodiscard]] static std::optional<int64_t> nearestSampleIndex(Time time) {
        const auto index = sampleIndex(time);
        if (!index)
            return std::nullopt;
        const auto before = sampleTime(*index);
        const auto after = *index == (std::numeric_limits<int64_t>::max)() ? std::nullopt : sampleTime(*index + 1);
        if (!before)
            return std::nullopt;
        return after && *after - time < time - *before ? *index + 1 : *index;
    }
    [[nodiscard]] static std::optional<Time> nearestSample(Time time) {
        const auto index = nearestSampleIndex(time);
        return index ? sampleTime(*index) : std::nullopt;
    }
    [[nodiscard]] static std::optional<Time> step(Time time, int64_t offset) {
        const auto index = nearestSampleIndex(time);
        if (!index || (offset > 0 && *index > (std::numeric_limits<int64_t>::max)() - offset) ||
            (offset < 0 && offset < -*index))
            return std::nullopt;
        return sampleTime(*index + offset);
    }
};

inline std::optional<Time> BoundedGridTime(Time requested, Time lower, Time upper, RationalFps fps, bool audio) {
    if (lower < 0 || upper < lower)
        return std::nullopt;
    const auto clamped = std::clamp(requested, lower, upper);
    const auto candidate =
        (audio ? AudioSampleGrid::nearestSample(clamped) : fps.nearestFrame(clamped)).value_or(clamped);
    if (candidate >= lower && candidate <= upper)
        return candidate;
    const auto index = audio ? AudioSampleGrid::sampleIndex(candidate < lower ? lower : upper)
                             : fps.frameIndex(candidate < lower ? lower : upper);
    if (!index)
        return std::nullopt;
    auto boundary = audio ? AudioSampleGrid::sampleTime(*index) : fps.frameTime(*index);
    if (boundary && *boundary < lower) {
        if (*index == (std::numeric_limits<Time>::max)())
            return std::nullopt;
        boundary = audio ? AudioSampleGrid::sampleTime(*index + 1) : fps.frameTime(*index + 1);
    }
    return boundary && *boundary >= lower && *boundary <= upper ? boundary : std::nullopt;
}

enum class ResultOutMode { Fixed, FollowTimelineEnd };
enum class TimelineEndMode { FollowContentEnd, FixedEnd };
struct TimelineEnd {
    TimelineEndMode mode = TimelineEndMode::FollowContentEnd;
    Time end_us = 0;
    bool operator==(const TimelineEnd&) const = default;
};
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
    bool operator==(const InputMixSelection&) const = default;
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
    bool audio_identities_verified = false;
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
    // The imported audio streams are all disabled until the user chooses the
    // starting mix; a finished export must not proceed before that choice.
    bool mix_decision_required = false;
    std::optional<OriginalRecipeAnchor> original_recipe;
    uint64_t source_version = 0;
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

inline bool RebaseImportedAudioDefaults(std::vector<Clip>& clips, Id asset, const InputMixSelection& mix) {
    bool changed = false;
    for (auto& clip : clips) {
        if (clip.asset != asset || !clip.stream || clip.stream->kind != engine::TimelineStreamKind::Audio)
            continue;
        const bool audible = std::find(mix.selected_stream_identities.begin(), mix.selected_stream_identities.end(),
                                       clip.stream->stream_identity) != mix.selected_stream_identities.end();
        const bool enabled = clip.enabled == clip.original_enabled ? audible : clip.enabled;
        changed = changed || clip.enabled != enabled || clip.original_enabled != audible;
        clip.enabled = enabled;
        clip.original_enabled = audible;
    }
    return changed;
}

struct Track {
    Id id = 0;
    TrackType type = TrackType::Video;
    std::string name;
    bool edit_enabled = true;
    double gain_db = 0;
    bool operator==(const Track&) const = default;
};

struct Spacer {
    Id id = 0;
    Id track = 0;
    Time start_us = 0;
    Time duration_us = 0;
    [[nodiscard]] Time end() const {
        return start_us + duration_us;
    }
    bool operator==(const Spacer&) const = default;
};

inline std::vector<Id> ExpandClipTargetIds(const std::vector<Clip>& clips, const std::vector<Id>& requested,
                                           bool expand_linked) {
    std::set<Id> targets(requested.begin(), requested.end());
    if (expand_linked) {
        std::set<Id> groups;
        for (const auto& clip : clips)
            if (targets.contains(clip.id) && clip.group)
                groups.insert(clip.group);
        for (const auto& clip : clips)
            if (clip.group && groups.contains(clip.group))
                targets.insert(clip.id);
    }
    return {targets.begin(), targets.end()};
}

inline bool ClipTargetsUseSamples(const std::vector<Clip>& clips, const std::vector<Track>& tracks,
                                  const std::vector<Id>& requested) {
    const std::set<Id> ids(requested.begin(), requested.end());
    std::set<Id> video_tracks;
    for (const auto& track : tracks)
        if (track.type == TrackType::Video)
            video_tracks.insert(track.id);
    return std::none_of(clips.begin(), clips.end(),
                        [&](const auto& clip) { return ids.contains(clip.id) && video_tracks.contains(clip.track); });
}

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
    Time incoming_anchor_us = 0;
    bool operator==(const Transition&) const = default;
};

struct ClipClipboard {
    std::vector<std::shared_ptr<const Asset>> assets;
    std::vector<Track> tracks;
    std::vector<Clip> clips;
    std::vector<Transition> junctions;
    Time duration_us = 0;
};

struct PasteClips {
    std::shared_ptr<const ClipClipboard> clipboard;
    Time at_us = 0;
    Id active_track = 0;
    bool insert = false;
    bool auto_crossfade = true;
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
    IoFailure,
    TrackLocked,
    LinkedScopeRequired
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
    bool auto_crossfade = true;
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
    std::vector<Id> track_ids;
};
struct RippleRemoveRange {
    Span span;
    std::vector<Id> track_ids;
};
struct SplitRange {
    Span span;
    std::vector<Id> track_ids;
};
struct AdjustRangeGain {
    Span span;
    std::vector<Id> track_ids;
    double delta_db = 0;
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
    TrimMode mode = TrimMode::Fixed;
    std::vector<Id> targets;
    bool expand_linked = true;
    bool auto_crossfade = true;
    bool clamp_to_bounds = true;
};
struct Move {
    std::vector<Id> targets;
    Time delta_us = 0;
    std::vector<std::pair<Id, Id>> target_tracks;
    std::vector<Track> new_tracks;
    bool expand_linked = true;
    bool auto_crossfade = true;
    bool ripple = false;
    bool new_video_front = false;
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
struct SetTimelineEnd {
    TimelineEnd value;
};
struct AddSpacer {
    Id track_id = 0;
    Time start_us = 0;
    Time duration_us = 0;
};
struct MoveSpacer {
    Id id = 0;
    Time delta_us = 0;
};
struct TrimSpacer {
    Id id = 0;
    TrimEdge edge = TrimEdge::Right;
    Time delta_us = 0;
};
struct RemoveSpacer {
    Id id = 0;
};
struct CloseGap {
    Span span;
};
struct Unlink {
    std::vector<Id> targets;
};
struct Relink {
    std::vector<Id> targets;
};
struct ReplaceAssetSource {
    Id asset_id = 0;
    uint64_t expected_source_version = 0;
    Asset replacement;
    bool adopt_changed_bytes = false;
};
struct SetTrackEnabled {
    Id id = 0;
    bool enabled = true;
};
struct RenameTrack {
    Id id = 0;
    std::string name;
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
    std::vector<Id> targets;
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
struct SetJunctionWindow {
    Id junction_id = 0;
    Span span;
    JunctionAnchor anchor = JunctionAnchor::Custom;
};
struct SetJunctionCurve {
    Id junction_id = 0;
    CurveSpec curve;
};
struct SetAudioLaw {
    Id junction_id = 0;
    AudioGainLaw law = AudioGainLaw::EqualGain;
};
struct SetHardCut {
    Id junction_id = 0;
    Time cut_us = 0;
};
struct CopyJunctionToLinked {
    Id source_junction_id = 0;
    std::vector<Id> targets;
};
struct CrossfadeAtCut {
    Id outgoing = 0;
    Id incoming = 0;
    Time duration_us = 500'000;
    bool expand_linked = true;
};
struct RemoveOverlap {
    Id junction_id = 0;
    bool expand_linked = true;
};
using EditCommand =
    std::variant<AddAssetGroup, Split, LiftClips, RippleRemoveClips, LiftRange, RippleRemoveRange, KeepRange, Trim,
                 Move, CreateTrack, DeleteTrack, ReorderTrack, SetResultRange, Unlink, Relink, SetTrackEnabled,
                 SetClipEnabled, SetGain, SetClipOpacity, SetFade, SetJunctionWindow, SetJunctionCurve, SetAudioLaw,
                 SetHardCut, CopyJunctionToLinked, SetTimelineEnd, AddSpacer, MoveSpacer, TrimSpacer, RemoveSpacer,
                 CloseGap, SplitRange, AdjustRangeGain, RenameTrack, PasteClips, ReplaceAssetSource, CrossfadeAtCut,
                 RemoveOverlap>;

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
    std::vector<Spacer> spacers;
    std::vector<Id> locked_track_ids;
    TimelineEnd timeline_end;
    uint64_t source_version_high_watermark = 0;
};

struct EditPreview {
    Revision base_revision = 0;
    bool valid = false;
    std::optional<EditError> error;
    std::vector<Track> tracks;
    std::vector<Clip> clips;
    std::vector<Transition> junctions;
    Time effective_delta_us = 0;
    std::vector<Spacer> spacers;
    TimelineEnd timeline_end;
    std::vector<std::shared_ptr<const Asset>> source_bindings;
};

EditPreview PreviewEdit(const EditSnapshot& snapshot, const EditCommand& command);

} // namespace exosnap::edit
