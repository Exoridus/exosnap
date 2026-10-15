#pragma once
#include "EditCommandTypes.h"
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/edit_timeline.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace exosnap::edit {

// Values use microseconds. Assets reference immutable source descriptors; history stores metadata deltas.
class Workspace {
    friend EditPreview PreviewEdit(const EditSnapshot&, const EditCommand&);

  public:
    Workspace() : tracks_{{nextId(), TrackType::Video}, {nextId(), TrackType::Audio}} {
    }

    [[nodiscard]] Revision revision() const {
        return revision_;
    }
    [[nodiscard]] size_t historyBytes() const {
        return history_bytes_;
    }
    [[nodiscard]] std::shared_ptr<const EditSnapshot> snapshot() const {
        if (snapshot_cache_ && snapshot_cache_->revision == revision_)
            return snapshot_cache_;
        auto value = std::make_shared<EditSnapshot>();
        value->revision = revision_;
        value->tracks = tracks_;
        value->clips = clips_;
        value->junctions = transitions_;
        value->result_range = result_range_;
        value->output_fps = output_fps_;
        value->output_width = output_width_;
        value->output_height = output_height_;
        value->id_high_watermark = next_id_;
        for (const auto& source : assets_) {
            auto asset = std::make_shared<Asset>(source);
            asset->monitor_selection.reset();
            value->assets.push_back(std::move(asset));
        }
        snapshot_cache_ = value;
        return value;
    }
    CommandResult apply(const EditCommand& command, Revision expected_revision) {
        if (expected_revision != revision_)
            return rejected(EditErrorCode::StaleRevision);
        auto candidate = workingCopy();
        candidate.last_error_.reset();
        std::visit([&candidate](const auto& value) { candidate.execute(value); }, command);
        candidate.validate();
        if (candidate.last_error_)
            return {CommandStatus::Rejected, revision_, candidate.last_error_, {}, {}};
        if (candidate.clips_ == clips_ && candidate.tracks_ == tracks_ && candidate.transitions_ == transitions_ &&
            candidate.result_range_ == result_range_ && candidate.output_fps_ == output_fps_ &&
            candidate.output_width_ == output_width_ && candidate.output_height_ == output_height_)
            return {CommandStatus::NoOp, revision_, {}, {}, {}};
        const auto old_ids = clips_;
        const auto old_tracks = tracks_;
        if (!publish(candidate))
            return {CommandStatus::Rejected,
                    revision_,
                    last_error_.value_or(EditError{EditErrorCode::InvalidTarget, {}, {}}),
                    {},
                    {}};
        CommandResult result{CommandStatus::Applied, revision_, {}, {}, "edit.command"};
        for (const auto& clip : clips_)
            if (std::find(old_ids.begin(), old_ids.end(), clip) == old_ids.end())
                result.changed_ids.push_back(clip.id);
        for (const auto& clip : old_ids)
            if (!this->clip(clip.id))
                result.changed_ids.push_back(clip.id);
        for (const auto& track : tracks_)
            if (std::find(old_tracks.begin(), old_tracks.end(), track) == old_tracks.end())
                result.changed_ids.push_back(track.id);
        return result;
    }
    CommandResult undoCommand() {
        if (!undo())
            return {CommandStatus::NoOp, revision_, {}, {}, {}};
        return {CommandStatus::Applied, revision_, {}, {}, "edit.undo"};
    }
    CommandResult redoCommand() {
        if (!redo())
            return {CommandStatus::NoOp, revision_, {}, {}, {}};
        return {CommandStatus::Applied, revision_, {}, {}, "edit.redo"};
    }
    [[nodiscard]] const std::vector<Asset>& assets() const {
        return assets_;
    }
    [[nodiscard]] uint64_t monitoringRevision() const {
        return monitoring_revision_;
    }
    bool setMonitorSelection(Id id, engine::MonitorSelectionSnapshot selection) {
        const auto found =
            std::find_if(assets_.begin(), assets_.end(), [id](const auto& asset) { return asset.id == id; });
        if (found == assets_.end())
            return false;
        found->monitor_selection = std::move(selection);
        ++monitoring_revision_;
        return true;
    }
    // Records the user's choice of starting mix for a source whose provenance was
    // ambiguous. The audio clips of that source are enabled exactly for the chosen
    // streams in one undoable edit; the choice itself is not undone with it.
    bool resolveInputMix(Id asset_id, const std::vector<engine::AudioTrackId>& selected) {
        const auto found = std::find_if(assets_.begin(), assets_.end(),
                                        [asset_id](const auto& asset) { return asset.id == asset_id; });
        if (found == assets_.end() || !found->source_descriptor)
            return false;
        auto candidate = workingCopy();
        for (auto& clip : candidate.clips_) {
            if (clip.asset != asset_id || !clip.stream || clip.stream->kind != engine::TimelineStreamKind::Audio)
                continue;
            clip.enabled = std::find(selected.begin(), selected.end(), clip.stream->stream_identity) != selected.end();
            clip.original_enabled = clip.enabled;
        }
        if (!candidate.validate()) {
            last_error_ = candidate.last_error_;
            return false;
        }
        publish(candidate);
        InputMixSelection mix;
        mix.asset_identity_fingerprint = found->source_descriptor->asset_identity_fingerprint;
        mix.selected_stream_identities = selected;
        mix.authority = InputMixAuthority::UserConfirmed;
        found->input_mix_selection = std::move(mix);
        found->mix_decision_required = false;
        return true;
    }
    [[nodiscard]] const std::vector<Track>& tracks() const {
        return tracks_;
    }
    [[nodiscard]] const std::vector<Clip>& clips() const {
        return clips_;
    }
    [[nodiscard]] const std::vector<Transition>& transitions() const {
        return transitions_;
    }
    [[nodiscard]] engine::TimelineSnapshot timeline() const {
        engine::TimelineSnapshot result;
        result.duration_us = duration();
        result.revision = revision_;
        result.result_in_us = result_range_.in_us;
        result.result_out_us =
            result_range_.out_mode == ResultOutMode::FollowTimelineEnd ? result.duration_us : result_range_.out_us;
        result.output_fps_num = static_cast<int>(output_fps_.num);
        result.output_fps_den = static_cast<int>(output_fps_.den);
        result.output_width = output_width_;
        result.output_height = output_height_;
        for (const auto& track : tracks_)
            result.tracks.push_back({track.id, track.type == TrackType::Audio, track.edit_enabled, track.gain_db});
        for (const auto& c : clips_) {
            const auto t =
                std::find_if(tracks_.begin(), tracks_.end(), [&c](const auto& track) { return track.id == c.track; });
            engine::TimelineClip projected{c.id,    c.asset,     c.group,     t->type == TrackType::Audio,
                                           c.start, c.source_in, c.source_out};
            projected.track = c.track;
            projected.stream = c.stream;
            projected.enabled = c.enabled;
            projected.gain_db = c.gain_db;
            projected.opacity = c.opacity;
            projected.fade_in = c.fade_in;
            projected.fade_out = c.fade_out;
            projected.original_enabled = c.original_enabled;
            result.clips.push_back(std::move(projected));
        }
        for (const auto& t : transitions_) {
            if (t.kind == TransitionKind::Crossfade)
                result.crossfades.push_back({t.outgoing, t.incoming, t.duration});
            result.junctions.push_back({t.id, t.track, t.outgoing, t.incoming, t.kind, t.in_us, t.out_us, t.cut_us,
                                        t.curve, t.audio_law, t.anchor});
        }
        return result;
    }
    bool crossfade(Id outgoing, Id incoming, Time requested = 500'000) {
        const auto* a = clip(outgoing);
        const auto* b = clip(incoming);
        if (!a || !b || a->track != b->track || requested <= 0 || a->start >= b->start)
            return false;
        const auto track =
            std::find_if(tracks_.begin(), tracks_.end(), [a](const auto& t) { return t.id == a->track; });
        if (track == tracks_.end() || track->type != TrackType::Video)
            return false;
        auto transitions = transitions_;
        auto existing = std::find_if(transitions.begin(), transitions.end(), [outgoing, incoming](const auto& t) {
            return t.outgoing == outgoing && t.incoming == incoming;
        });
        const Time old = existing == transitions.end() ? 0 : existing->duration;
        if (a->end() - b->start != old)
            return false;
        Time available_a = a->duration();
        Time available_b = b->duration();
        for (const auto& t : transitions) {
            if (t.incoming == outgoing)
                available_a -= t.duration;
            if (t.outgoing == incoming)
                available_b -= t.duration;
        }
        const Time duration = std::min({requested, available_a - 1, available_b - 1});
        if (duration <= 0)
            return false;
        if (existing == transitions.end())
            transitions.push_back({outgoing, incoming, TransitionKind::Crossfade, duration});
        else
            existing->duration = duration;
        auto after = clips_;
        // Ripple the suffix as one transaction so later cuts do not acquire gaps.
        for (auto& c : after)
            if (c.start >= b->start)
                if (!addTime(c.start, old - duration, c.start))
                    return false;
        return commit(std::move(after), outgoing, std::move(transitions));
    }
    bool removeCrossfade(Id outgoing) {
        const auto found = std::find_if(transitions_.begin(), transitions_.end(),
                                        [outgoing](const auto& junction) { return junction.outgoing == outgoing; });
        if (found == transitions_.end() || found->kind != TransitionKind::Crossfade)
            return false;
        const auto midpoint = found->in_us + (found->out_us - found->in_us) / 2;
        return apply(SetHardCut{found->id, midpoint}, revision_).status == CommandStatus::Applied;
    }
    [[nodiscard]] Id selection() const {
        return selection_;
    }
    void select(Id id) {
        selection_ = clip(id) ? id : 0;
    }
    [[nodiscard]] Time playhead() const {
        return playhead_;
    }
    void seek(Time value) {
        playhead_ = std::clamp<Time>(value, 0, duration());
    }
    [[nodiscard]] Time duration() const {
        Time end = 0;
        for (const auto& c : clips_)
            end = std::max(end, c.end());
        return end;
    }
    [[nodiscard]] const Asset* asset(Id id) const {
        auto it = std::find_if(assets_.begin(), assets_.end(), [id](const auto& a) { return a.id == id; });
        return it == assets_.end() ? nullptr : &*it;
    }
    [[nodiscard]] const Clip* clip(Id id) const {
        auto it = std::find_if(clips_.begin(), clips_.end(), [id](const auto& c) { return c.id == id; });
        return it == clips_.end() ? nullptr : &*it;
    }
    [[nodiscard]] const Clip* active(Time at, TrackType type = TrackType::Video) const {
        for (auto track = tracks_.rbegin(); track != tracks_.rend(); ++track) {
            if (track->type != type)
                continue;
            for (const auto& c : clips_)
                if (c.track == track->id && c.start <= at && at < c.end())
                    return &c;
        }
        return nullptr;
    }
    Id addTrack(TrackType type) {
        const Id previous = next_id_;
        const auto result = apply(CreateTrack{type, tracks_.size(), {}}, revision_);
        return result.status == CommandStatus::Applied ? previous + 1 : 0;
    }
    Id addAsset(Asset asset) {
        for (auto& current : assets_)
            if (current.path == asset.path && current.state == asset.state &&
                (current.duration == asset.duration || current.duration <= 0)) {
                if (current.duration <= 0 && asset.duration > 0) {
                    asset.id = current.id;
                    current = std::move(asset);
                    ++revision_;
                    snapshot_cache_.reset();
                }
                return current.id;
            }
        asset.id = nextId();
        if (!asset.id)
            return 0;
        ++revision_;
        snapshot_cache_.reset();
        assets_.push_back(std::move(asset));
        return assets_.back().id;
    }
    bool insert(Id asset_id, Time start = -1) {
        return insertAssets({asset_id}, start);
    }
    bool insertAssets(const std::vector<Id>& ids, Time start = -1) {
        auto candidate = workingCopy();
        if (start < 0)
            start = duration();
        Id first = 0;
        for (const auto id : ids) {
            const auto* source = asset(id);
            if (!source)
                return false;
            candidate.execute(AddAssetGroup{id, start, 0, {}, source->input_mix_selection, true});
            if (candidate.last_error_)
                return false;
            if (!first)
                first = candidate.selection_;
            if (!addTime(start, source->duration, start))
                return false;
        }
        candidate.selection_ = first;
        return publish(candidate);
    }
    bool move(Id id, Time start) {
        const auto* selected = clip(id);
        if (!selected || start < 0)
            return false;
        return apply(Move{{id}, start - selected->start, {}, {}, true}, revision_).status == CommandStatus::Applied;
    }
    bool trim(Id id, Time source_in, Time source_out) {
        const auto* selected = clip(id);
        if (!selected || source_in < 0 || source_out <= source_in)
            return false;
        auto candidate = workingCopy();
        candidate.execute(Trim{id, TrimEdge::Left, source_in - selected->source_in, TrimMode::Fixed, {id}, true});
        if (candidate.last_error_)
            return false;
        const auto* adjusted = candidate.clip(id);
        candidate.execute(Trim{id, TrimEdge::Right, source_out - adjusted->source_out, TrimMode::Fixed, {id}, true});
        return !candidate.last_error_ && publish(candidate);
    }
    bool split(Id id, Time at) {
        return apply(Split{{id}, at, true}, revision_).status == CommandStatus::Applied;
    }
    bool remove(Id id, bool ripple) {
        const EditCommand command =
            ripple ? EditCommand{RippleRemoveClips{{id}, true}} : EditCommand{LiftClips{{id}, true}};
        return apply(command, revision_).status == CommandStatus::Applied;
    }
    [[nodiscard]] Time snap(Time requested, Id moving, Time tolerance) const {
        Time best = requested;
        Time distance = tolerance + 1;
        const auto* selected = clip(moving);
        auto consider = [&](Time candidate) {
            const Time delta = candidate > requested ? candidate - requested : requested - candidate;
            if (delta <= tolerance && delta < distance) {
                best = candidate;
                distance = delta;
            }
        };
        consider(0);
        consider(playhead_);
        for (const auto& c : clips_) {
            if (selected && linked(c, *selected))
                continue;
            consider(c.start);
            consider(c.end());
            if (const auto* a = asset(c.asset))
                for (const auto& marker : a->markers) {
                    const Time time = static_cast<Time>(marker.time_ms) * 1000;
                    if (time >= c.source_in && time <= c.source_out)
                        consider(c.start + time - c.source_in);
                }
        }
        return best;
    }
    [[nodiscard]] bool canUndo() const {
        return cursor_ > 0;
    }
    [[nodiscard]] bool canRedo() const {
        return cursor_ < history_.size();
    }
    bool undo() {
        if (!canUndo() || revision_ == (std::numeric_limits<Revision>::max)())
            return false;
        const auto& command = history_[--cursor_];
        apply(command.after, command.before);
        patchEntities(transitions_, command.transitions_after, command.transitions_before);
        patchTracks(command.tracks_after, command.tracks_before, command.order_before);
        result_range_ = command.range_before;
        output_fps_ = command.fps_before;
        output_width_ = command.width_before;
        output_height_ = command.height_before;
        if (!clip(selection_))
            selection_ = 0;
        ++revision_;
        snapshot_cache_.reset();
        seek(playhead_);
        return true;
    }
    bool redo() {
        if (!canRedo() || revision_ == (std::numeric_limits<Revision>::max)())
            return false;
        const auto& command = history_[cursor_++];
        apply(command.before, command.after);
        patchEntities(transitions_, command.transitions_before, command.transitions_after);
        patchTracks(command.tracks_before, command.tracks_after, command.order_after);
        result_range_ = command.range_after;
        output_fps_ = command.fps_after;
        output_width_ = command.width_after;
        output_height_ = command.height_after;
        if (!clip(selection_))
            selection_ = 0;
        ++revision_;
        snapshot_cache_.reset();
        seek(playhead_);
        return true;
    }

  private:
    CommandResult rejected(EditErrorCode code, std::vector<Id> ids = {}) const {
        return {CommandStatus::Rejected, revision_, EditError{code, std::move(ids), {}}, {}, {}};
    }
    void fail(EditErrorCode code, std::vector<Id> ids = {}) {
        if (!last_error_)
            last_error_ = EditError{code, std::move(ids), {}};
    }
    static bool addTime(Time left, Time right, Time& result) {
        if ((right > 0 && left > (std::numeric_limits<Time>::max)() - right) ||
            (right < 0 && left < (std::numeric_limits<Time>::min)() - right))
            return false;
        result = left + right;
        return true;
    }
    Workspace workingCopy() const {
        Workspace value;
        value.next_id_ = next_id_;
        value.monitoring_revision_ = monitoring_revision_;
        value.selection_ = selection_;
        value.playhead_ = playhead_;
        value.assets_ = assets_;
        value.tracks_ = tracks_;
        value.clips_ = clips_;
        value.transitions_ = transitions_;
        value.revision_ = revision_;
        value.result_range_ = result_range_;
        value.output_fps_ = output_fps_;
        value.output_width_ = output_width_;
        value.output_height_ = output_height_;
        value.candidate_mode_ = true;
        return value;
    }
    [[nodiscard]] std::vector<Id> targets(const std::vector<Id>& requested, bool expand) {
        std::set<Id> result;
        for (const auto id : requested) {
            const auto* selected = clip(id);
            if (!selected) {
                fail(EditErrorCode::InvalidTarget, {id});
                return {};
            }
            for (const auto& candidate : clips_)
                if (candidate.id == id || (expand && linked(candidate, *selected)))
                    result.insert(candidate.id);
        }
        return {result.begin(), result.end()};
    }
    static bool contains(const std::vector<Id>& ids, Id id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }
    Track* mutableTrack(Id id) {
        const auto found = std::find_if(tracks_.begin(), tracks_.end(), [id](const auto& t) { return t.id == id; });
        return found == tracks_.end() ? nullptr : &*found;
    }
    Clip* mutableClip(Id id) {
        const auto found = std::find_if(clips_.begin(), clips_.end(), [id](const auto& c) { return c.id == id; });
        return found == clips_.end() ? nullptr : &*found;
    }
    bool fitFades(Clip& clip, std::optional<FadeEdge> edited = {}) {
        if (clip.fade_in.duration_us < 0 || clip.fade_out.duration_us < 0 ||
            !engine::EvaluateCurve(clip.fade_in.curve, 0) || !engine::EvaluateCurve(clip.fade_out.curve, 0)) {
            fail(EditErrorCode::InvalidCurve, {clip.id});
            return false;
        }
        const auto duration = clip.duration();
        clip.fade_in.duration_us = std::min(clip.fade_in.duration_us, duration);
        clip.fade_out.duration_us = std::min(clip.fade_out.duration_us, duration);
        if (clip.fade_in.duration_us <= duration - clip.fade_out.duration_us)
            return true;
        if (edited == FadeEdge::In) {
            clip.fade_in.duration_us = duration - clip.fade_out.duration_us;
        } else if (edited == FadeEdge::Out) {
            clip.fade_out.duration_us = duration - clip.fade_in.duration_us;
        } else {
            Time total = 0;
            if (!addTime(clip.fade_in.duration_us, clip.fade_out.duration_us, total)) {
                fail(EditErrorCode::InvalidTime, {clip.id});
                return false;
            }
            const auto incoming = scaleRatio(clip.fade_in.duration_us, duration, total);
            if (!incoming) {
                fail(EditErrorCode::InvalidTime, {clip.id});
                return false;
            }
            clip.fade_in.duration_us = *incoming;
            clip.fade_out.duration_us = duration - *incoming;
        }
        return true;
    }
    Transition* mutableJunction(Id id) {
        const auto found =
            std::find_if(transitions_.begin(), transitions_.end(), [id](const auto& item) { return item.id == id; });
        return found == transitions_.end() ? nullptr : &*found;
    }
    Time quantizeBoundary(Time requested, bool audio) const {
        const auto grid = audio ? RationalFps{48000, 1} : output_fps_;
        if (!grid.valid())
            return requested;
        const auto before = requested == (std::numeric_limits<Time>::max)() ? grid.previousFrame(requested)
                                                                            : grid.previousFrame(requested + 1);
        const auto after = requested == 0 ? std::optional<Time>{0} : grid.nextFrame(requested - 1);
        if (!before)
            return after.value_or(requested);
        if (!after)
            return *before;
        return requested - *before < *after - requested ? *before : *after;
    }
    void execute(const SetJunctionWindow& command) {
        auto* junction = mutableJunction(command.junction_id);
        if (!junction) {
            fail(EditErrorCode::InvalidTarget, {command.junction_id});
            return;
        }
        const auto* a = clip(junction->outgoing);
        const auto* b = clip(junction->incoming);
        if (!a || !b || command.span.in_us < b->start || command.span.out_us > a->end() ||
            command.span.out_us <= command.span.in_us) {
            fail(EditErrorCode::InvalidTime, {command.junction_id});
            return;
        }
        junction->kind = TransitionKind::Crossfade;
        junction->anchor = command.anchor;
        junction->in_us = command.anchor == JunctionAnchor::FullOverlap ? b->start : command.span.in_us;
        junction->out_us = command.anchor == JunctionAnchor::FullOverlap ? a->end() : command.span.out_us;
        junction->incoming_anchor_us = b->start;
    }
    void execute(const SetJunctionCurve& command) {
        auto* junction = mutableJunction(command.junction_id);
        if (!junction)
            fail(EditErrorCode::InvalidTarget, {command.junction_id});
        else if (!engine::EvaluateCurve(command.curve, 0))
            fail(EditErrorCode::InvalidCurve, {command.junction_id});
        else
            junction->curve = command.curve;
    }
    void execute(const SetAudioLaw& command) {
        auto* junction = mutableJunction(command.junction_id);
        const auto* track = junction ? mutableTrack(junction->track) : nullptr;
        if (!junction || !track || track->type != TrackType::Audio)
            fail(EditErrorCode::WrongTrackKind, {command.junction_id});
        else if (command.law != AudioGainLaw::EqualGain && command.law != AudioGainLaw::EqualPower)
            fail(EditErrorCode::InvalidCurve, {command.junction_id});
        else
            junction->audio_law = command.law;
    }
    void execute(const SetHardCut& command) {
        auto* junction = mutableJunction(command.junction_id);
        if (!junction) {
            fail(EditErrorCode::InvalidTarget, {command.junction_id});
            return;
        }
        const auto* a = clip(junction->outgoing);
        const auto* b = clip(junction->incoming);
        if (!a || !b || command.cut_us < b->start || command.cut_us > a->end()) {
            fail(EditErrorCode::InvalidTime, {command.junction_id});
            return;
        }
        const auto* track = mutableTrack(a->track);
        if (!track) {
            fail(EditErrorCode::InvalidTarget, {command.junction_id});
            return;
        }
        junction->kind = TransitionKind::HardCut;
        junction->cut_us =
            std::clamp(quantizeBoundary(command.cut_us, track->type == TrackType::Audio), b->start, a->end());
        junction->anchor = JunctionAnchor::Custom;
        junction->incoming_anchor_us = b->start;
    }
    static std::optional<Time> scaleRatio(Time value, Time multiplier, Time divisor) {
        if (value < 0 || multiplier < 0 || divisor <= 0)
            return std::nullopt;
        uint64_t quotient = 0;
        uint64_t remainder = 0;
        const auto d = static_cast<uint64_t>(divisor);
        const auto m = static_cast<uint64_t>(multiplier);
        constexpr auto limit = static_cast<uint64_t>((std::numeric_limits<Time>::max)());
        for (int bit = 63; bit >= 0; --bit) {
            if (quotient > limit / 2)
                return std::nullopt;
            quotient *= 2;
            remainder *= 2;
            if (remainder >= d) {
                remainder -= d;
                if (quotient == limit)
                    return std::nullopt;
                ++quotient;
            }
            if ((static_cast<uint64_t>(value) >> bit) & 1u) {
                if (m / d > limit - quotient)
                    return std::nullopt;
                quotient += m / d;
                const auto add = m % d;
                if (remainder >= d - add) {
                    remainder -= d - add;
                    if (quotient == limit)
                        return std::nullopt;
                    ++quotient;
                } else {
                    remainder += add;
                }
            }
        }
        return static_cast<Time>(quotient);
    }
    void execute(const CopyJunctionToLinked& command) {
        const auto* found = mutableJunction(command.source_junction_id);
        if (!found) {
            fail(EditErrorCode::InvalidTarget, {command.source_junction_id});
            return;
        }
        const auto source = *found;
        const auto* source_a = clip(source.outgoing);
        const auto* source_b = clip(source.incoming);
        if (!source_a || !source_b || !source_a->group || !source_b->group) {
            fail(EditErrorCode::InvalidTarget, {source.id});
            return;
        }
        const auto source_duration = source_a->end() - source_b->start;
        for (const auto id : command.targets) {
            auto* target = mutableJunction(id);
            const auto* a = target ? clip(target->outgoing) : nullptr;
            const auto* b = target ? clip(target->incoming) : nullptr;
            if (!target || !a || !b || a->group != source_a->group || b->group != source_b->group) {
                fail(EditErrorCode::InvalidTarget, {id});
                return;
            }
            const auto duration = a->end() - b->start;
            const auto start = scaleRatio(source.in_us - source_b->start, duration, source_duration);
            const auto end = scaleRatio(source.out_us - source_b->start, duration, source_duration);
            const auto cut = scaleRatio(source.cut_us - source_b->start, duration, source_duration);
            if ((source.kind == TransitionKind::Crossfade && (!start || !end)) ||
                (source.kind == TransitionKind::HardCut && !cut)) {
                fail(EditErrorCode::InvalidTime, {id});
                return;
            }
            target->kind = source.kind;
            target->curve = source.curve;
            target->audio_law = source.audio_law;
            target->anchor = JunctionAnchor::Custom;
            target->incoming_anchor_us = b->start;
            if (source.kind == TransitionKind::HardCut) {
                target->cut_us = b->start + *cut;
            } else {
                target->in_us = b->start + *start;
                target->out_us = b->start + *end;
                if (*end <= *start) {
                    target->kind = TransitionKind::HardCut;
                    target->cut_us = target->in_us;
                }
            }
        }
    }
    bool normalizeJunctions() {
        std::vector<Transition> retained;
        for (auto junction : transitions_) {
            const auto* a = clip(junction.outgoing);
            const auto* b = clip(junction.incoming);
            if (!a || !b || a->end() <= b->start)
                continue;
            if (a->track != b->track || a->start >= b->start || a->end() >= b->end()) {
                fail(EditErrorCode::UnsupportedOverlap, {junction.outgoing, junction.incoming});
                return false;
            }
            const auto overlap = a->end() - b->start;
            const auto previous_anchor = junction.incoming_anchor_us ? junction.incoming_anchor_us : junction.in_us;
            const auto midpoint = junction.in_us + (junction.out_us - junction.in_us) / 2;
            if (junction.kind == TransitionKind::HardCut) {
                const auto offset = std::clamp(junction.cut_us - previous_anchor, Time{0}, overlap);
                junction.cut_us = b->start + offset;
            } else if (junction.anchor == JunctionAnchor::FullOverlap) {
                junction.in_us = b->start;
                junction.out_us = a->end();
            } else {
                junction.in_us = b->start + std::clamp(junction.in_us - previous_anchor, Time{0}, overlap);
                junction.out_us = b->start + std::clamp(junction.out_us - previous_anchor, Time{0}, overlap);
                if (junction.out_us <= junction.in_us) {
                    junction.kind = TransitionKind::HardCut;
                    junction.cut_us = b->start + std::clamp(midpoint - previous_anchor, Time{0}, overlap);
                }
            }
            junction.incoming_anchor_us = b->start;
            junction.duration = overlap;
            junction.track = a->track;
            if (!junction.id) {
                junction.id = nextId();
                mutableClip(a->id)->fade_out = {};
                mutableClip(b->id)->fade_in = {};
            }
            retained.push_back(std::move(junction));
        }
        transitions_ = std::move(retained);
        const Clip* previous = nullptr;
        const Clip* penultimate = nullptr;
        for (const auto& item : clips_) {
            if (!previous || previous->track != item.track) {
                penultimate = nullptr;
                previous = &item;
                continue;
            }
            if (previous->end() > item.start) {
                if (previous->start >= item.start || previous->end() >= item.end() ||
                    (penultimate && penultimate->end() > item.start)) {
                    fail(EditErrorCode::UnsupportedOverlap, {previous->id, item.id});
                    return false;
                }
                const auto count = std::count_if(transitions_.begin(), transitions_.end(), [&](const auto& junction) {
                    return junction.outgoing == previous->id && junction.incoming == item.id;
                });
                if (count > 1) {
                    fail(EditErrorCode::InvalidTarget, {previous->id, item.id});
                    return false;
                }
                if (count == 0) {
                    Transition junction{previous->id, item.id, TransitionKind::Crossfade, previous->end() - item.start};
                    junction.id = nextId();
                    junction.track = item.track;
                    junction.in_us = item.start;
                    junction.out_us = previous->end();
                    junction.incoming_anchor_us = item.start;
                    transitions_.push_back(std::move(junction));
                    mutableClip(previous->id)->fade_out = {};
                    mutableClip(item.id)->fade_in = {};
                }
            }
            penultimate = previous;
            previous = &item;
        }
        return !last_error_;
    }
    static std::vector<Span> unite(std::vector<Span> intervals) {
        std::sort(intervals.begin(), intervals.end(), [](const auto& a, const auto& b) { return a.in_us < b.in_us; });
        std::vector<Span> result;
        for (const auto interval : intervals) {
            if (result.empty() || interval.in_us > result.back().out_us)
                result.push_back(interval);
            else
                result.back().out_us = std::max(result.back().out_us, interval.out_us);
        }
        return result;
    }
    static Time mapRemoval(Time time, const std::vector<Span>& intervals) {
        Time removed = 0;
        for (const auto interval : intervals) {
            if (time < interval.in_us)
                break;
            removed += std::min(time, interval.out_us) - interval.in_us;
            if (time < interval.out_us)
                break;
        }
        return time - removed;
    }
    void mapRange(const std::vector<Span>& intervals) {
        result_range_.in_us = mapRemoval(result_range_.in_us, intervals);
        if (result_range_.out_mode == ResultOutMode::Fixed)
            result_range_.out_us = mapRemoval(result_range_.out_us, intervals);
    }
    void removeTargets(const std::vector<Id>& requested, bool expand, bool ripple) {
        const auto ids = targets(requested, expand);
        if (last_error_ || ids.empty())
            return;
        std::vector<Span> intervals;
        for (const auto& clip : clips_)
            if (contains(ids, clip.id))
                intervals.push_back({clip.start, clip.end()});
        intervals = unite(std::move(intervals));
        if (ripple)
            for (const auto& retained : clips_)
                if (!contains(ids, retained.id))
                    for (const auto interval : intervals)
                        if (retained.start < interval.out_us && retained.end() > interval.in_us) {
                            fail(EditErrorCode::RippleConflict, {retained.id});
                            return;
                        }
        std::erase_if(clips_, [&ids](const auto& clip) { return contains(ids, clip.id); });
        std::erase_if(transitions_,
                      [&ids](const auto& t) { return contains(ids, t.outgoing) || contains(ids, t.incoming); });
        if (ripple) {
            for (auto& clip : clips_)
                clip.start = mapRemoval(clip.start, intervals);
            for (auto& junction : transitions_) {
                junction.in_us = mapRemoval(junction.in_us, intervals);
                junction.out_us = mapRemoval(junction.out_us, intervals);
                junction.cut_us = mapRemoval(junction.cut_us, intervals);
            }
            mapRange(intervals);
        }
        if (contains(ids, selection_))
            selection_ = 0;
    }
    void execute(const LiftClips& command) {
        removeTargets(command.targets, command.expand_linked, false);
    }
    void execute(const RippleRemoveClips& command) {
        removeTargets(command.targets, command.expand_linked, true);
    }
    void execute(const Split& command) {
        auto requested = command.targets;
        if (requested.empty())
            for (const auto& clip : clips_)
                if (clip.start < command.at_us && command.at_us < clip.end())
                    requested.push_back(clip.id);
        const auto ids = targets(requested, command.expand_linked);
        if (last_error_)
            return;
        for (const auto& junction : transitions_) {
            const auto* incoming = clip(junction.incoming);
            const auto* outgoing = clip(junction.outgoing);
            if (incoming && outgoing && incoming->start < command.at_us && command.at_us < outgoing->end() &&
                (contains(ids, outgoing->id) || contains(ids, incoming->id))) {
                fail(EditErrorCode::TransitionBoundaryConflict, {junction.id});
                return;
            }
        }
        std::vector<Clip> tails;
        std::map<Id, Id> groups;
        std::map<Id, Id> tail_ids;
        for (auto& clip : clips_) {
            if (!contains(ids, clip.id) || command.at_us <= clip.start || command.at_us >= clip.end())
                continue;
            Clip tail = clip;
            tail.id = nextId();
            if (clip.group) {
                auto& tail_group = groups[clip.group];
                if (!tail_group)
                    tail_group = nextId();
                tail.group = tail_group;
            }
            tail.start = command.at_us;
            tail.source_in += command.at_us - clip.start;
            clip.source_out = tail.source_in;
            tail_ids[clip.id] = tail.id;
            tails.push_back(std::move(tail));
        }
        for (auto& junction : transitions_)
            if (tail_ids.contains(junction.outgoing))
                junction.outgoing = tail_ids.at(junction.outgoing);
        clips_.insert(clips_.end(), tails.begin(), tails.end());
    }
    void execute(const Move& command) {
        const auto ids = targets(command.targets, command.expand_linked);
        if (last_error_ || ids.empty())
            return;
        std::map<Id, Id> new_tracks;
        for (auto track : command.new_tracks) {
            const auto temporary = track.id;
            track.id = nextId();
            if (!track.id)
                return;
            new_tracks[temporary] = track.id;
            tracks_.push_back(std::move(track));
        }
        for (auto& clip : clips_) {
            if (!contains(ids, clip.id))
                continue;
            Time start = 0;
            if (!addTime(clip.start, command.delta_us, start) || start < 0) {
                fail(EditErrorCode::InvalidTime, {clip.id});
                return;
            }
            clip.start = start;
            for (const auto& [clip_id, requested_track] : command.target_tracks)
                if (clip.id == clip_id) {
                    const auto target_id =
                        new_tracks.contains(requested_track) ? new_tracks.at(requested_track) : requested_track;
                    const auto* current = mutableTrack(clip.track);
                    const auto* destination = mutableTrack(target_id);
                    if (!current || !destination || current->type != destination->type) {
                        fail(EditErrorCode::WrongTrackKind, {clip.id, target_id});
                        return;
                    }
                    clip.track = target_id;
                }
        }
    }
    void execute(const SetResultRange& command) {
        const auto total = duration();
        if (command.range.in_us < 0 || command.range.in_us > total ||
            (command.range.out_mode == ResultOutMode::Fixed &&
             (command.range.out_us < command.range.in_us || command.range.out_us > total))) {
            fail(EditErrorCode::InvalidTime);
            return;
        }
        result_range_ = command.range;
        if (result_range_.out_mode == ResultOutMode::Fixed && result_range_.out_us == total)
            result_range_.out_mode = ResultOutMode::FollowTimelineEnd;
    }
    void execute(const AddAssetGroup& command) {
        const auto* source = asset(command.asset_id);
        if (!source || source->duration <= 0 || command.placement_us < 0) {
            fail(EditErrorCode::InvalidTarget, {command.asset_id});
            return;
        }
        const auto group = nextId();
        auto find_track = [&](TrackType kind, Id requested, std::set<Id>& occupied) -> Id {
            if (requested) {
                const auto* track = mutableTrack(requested);
                if (!track || track->type != kind) {
                    fail(EditErrorCode::WrongTrackKind, {requested});
                    return 0;
                }
                occupied.insert(requested);
                return requested;
            }
            for (const auto& track : tracks_)
                if (track.type == kind && !occupied.contains(track.id)) {
                    occupied.insert(track.id);
                    return track.id;
                }
            const Id id = nextId();
            tracks_.push_back({id, kind});
            occupied.insert(id);
            return id;
        };
        std::set<Id> occupied;
        if (source->source_descriptor) {
            const auto& descriptor = *source->source_descriptor;
            if (command.input_mix_selection &&
                command.input_mix_selection->asset_identity_fingerprint != descriptor.asset_identity_fingerprint) {
                fail(EditErrorCode::SourceChanged, {source->id});
                return;
            }
            std::set<engine::AudioTrackId> identities;
            std::set<uint32_t> indices;
            for (const auto& stream : descriptor.streams) {
                if (stream.ref.stream_identity == engine::AudioTrackId{} ||
                    !identities.insert(stream.ref.stream_identity).second ||
                    !indices.insert(stream.ref.container_index).second || stream.source_span.in_us < 0 ||
                    stream.source_span.out_us <= stream.source_span.in_us) {
                    fail(EditErrorCode::SourceChanged, {source->id});
                    return;
                }
                if (stream.ref.kind == engine::TimelineStreamKind::Audio &&
                    (stream.codec.channels == 0 || stream.codec.channels > 2)) {
                    fail(EditErrorCode::UnsupportedAudioLayout, {source->id});
                    return;
                }
            }
            if (command.input_mix_selection) {
                std::set<engine::AudioTrackId> selected;
                for (const auto identity : command.input_mix_selection->selected_stream_identities) {
                    const auto found =
                        std::find_if(descriptor.streams.begin(), descriptor.streams.end(), [&](const auto& stream) {
                            return stream.ref.kind == engine::TimelineStreamKind::Audio &&
                                   stream.ref.stream_identity == identity;
                        });
                    if (found == descriptor.streams.end() || !selected.insert(identity).second) {
                        fail(EditErrorCode::SourceChanged, {source->id});
                        return;
                    }
                }
            }
            size_t audio = 0;
            for (const auto& stream : descriptor.streams) {
                const bool video = stream.ref.kind == engine::TimelineStreamKind::Video;
                if (video && !command.include_video)
                    continue;
                const Id requested =
                    video ? command.video_track_id
                          : (audio < command.audio_track_ids.size() ? command.audio_track_ids[audio] : 0);
                if (!video)
                    ++audio;
                const Id track = find_track(video ? TrackType::Video : TrackType::Audio, requested, occupied);
                if (auto* destination = mutableTrack(track); destination && destination->name.empty())
                    destination->name = stream.name;
                Time start = 0;
                if (!addTime(command.placement_us, stream.timeline_offset_us, start)) {
                    fail(EditErrorCode::InvalidTime);
                    return;
                }
                Clip clip{
                    nextId(), source->id, track, group, start, stream.source_span.in_us, stream.source_span.out_us};
                clip.stream = stream.ref;
                clip.stream->asset_id = source->id;
                if (!video && command.input_mix_selection)
                    clip.enabled = std::find(command.input_mix_selection->selected_stream_identities.begin(),
                                             command.input_mix_selection->selected_stream_identities.end(),
                                             stream.ref.stream_identity) !=
                                   command.input_mix_selection->selected_stream_identities.end();
                clip.original_enabled = clip.enabled;
                if (!selection_ || video)
                    selection_ = clip.id;
                clips_.push_back(std::move(clip));
                if (video && !output_fps_.valid()) {
                    output_fps_ = stream.fps.reduced();
                    output_width_ = source->width;
                    output_height_ = source->height;
                }
            }
        } else {
            for (const auto type : {TrackType::Video, TrackType::Audio}) {
                if ((type == TrackType::Video && !command.include_video) ||
                    (type == TrackType::Audio && !source->audio))
                    continue;
                const auto requested = type == TrackType::Video
                                           ? command.video_track_id
                                           : (command.audio_track_ids.empty() ? 0 : command.audio_track_ids.front());
                const auto track = find_track(type, requested, occupied);
                const auto id = nextId();
                clips_.push_back({id, source->id, track, group, command.placement_us, 0, source->duration});
                if (type == TrackType::Video)
                    selection_ = id;
            }
            if (command.include_video && !output_fps_.valid() && source->fps_num > 0 && source->fps_den > 0) {
                output_fps_ =
                    RationalFps{static_cast<uint32_t>(source->fps_num), static_cast<uint32_t>(source->fps_den)}
                        .reduced();
                output_width_ = source->width;
                output_height_ = source->height;
            }
        }
    }
    void execute(const SetTrackEnabled& command) {
        if (auto* track = mutableTrack(command.id))
            track->edit_enabled = command.enabled;
        else
            fail(EditErrorCode::InvalidTarget, {command.id});
    }
    bool checkRange(Span span) {
        if (span.in_us < 0 || span.out_us <= span.in_us || span.out_us > duration()) {
            fail(EditErrorCode::InvalidTime);
            return false;
        }
        for (const auto& junction : transitions_) {
            const auto* incoming = clip(junction.incoming);
            const auto* outgoing = clip(junction.outgoing);
            if (!incoming || !outgoing)
                continue;
            for (const auto boundary : {span.in_us, span.out_us})
                if (incoming->start < boundary && boundary < outgoing->end()) {
                    fail(EditErrorCode::TransitionBoundaryConflict, {junction.id});
                    return false;
                }
        }
        return true;
    }
    void eraseRange(Span span, bool ripple) {
        if (!checkRange(span))
            return;
        std::vector<Clip> result;
        std::map<Id, Id> tail_groups;
        for (const auto& clip : clips_) {
            if (clip.end() <= span.in_us || clip.start >= span.out_us) {
                result.push_back(clip);
                continue;
            }
            const bool left = clip.start < span.in_us;
            const bool right = clip.end() > span.out_us;
            if (left) {
                auto head = clip;
                head.source_out = head.source_in + span.in_us - head.start;
                head.fade_out.duration_us = std::min(head.fade_out.duration_us, head.duration());
                result.push_back(std::move(head));
            }
            if (right) {
                auto tail = clip;
                if (left) {
                    tail.id = nextId();
                    if (tail.group) {
                        auto& group = tail_groups[tail.group];
                        if (!group)
                            group = nextId();
                        tail.group = group;
                    }
                }
                tail.source_in += span.out_us - tail.start;
                tail.start = span.out_us;
                tail.fade_in.duration_us = std::min(tail.fade_in.duration_us, tail.duration());
                result.push_back(std::move(tail));
            }
        }
        std::erase_if(transitions_, [&](const auto& junction) {
            const auto* incoming = clip(junction.incoming);
            const auto* outgoing = clip(junction.outgoing);
            return !incoming || !outgoing || (span.in_us <= incoming->start && span.out_us >= outgoing->end());
        });
        clips_ = std::move(result);
        if (ripple) {
            for (auto& clip : clips_)
                clip.start = mapRemoval(clip.start, {span});
            for (auto& junction : transitions_) {
                junction.in_us = mapRemoval(junction.in_us, {span});
                junction.out_us = mapRemoval(junction.out_us, {span});
                junction.cut_us = mapRemoval(junction.cut_us, {span});
            }
            mapRange({span});
        }
    }
    void execute(const LiftRange& command) {
        eraseRange(command.span, false);
    }
    void execute(const RippleRemoveRange& command) {
        eraseRange(command.span, true);
    }
    void execute(const KeepRange& command) {
        if (!checkRange(command.span))
            return;
        std::vector<Clip> result;
        for (const auto& clip : clips_) {
            const auto begin = std::max(clip.start, command.span.in_us);
            const auto end = std::min(clip.end(), command.span.out_us);
            if (begin >= end)
                continue;
            auto retained = clip;
            retained.source_in += begin - clip.start;
            retained.source_out = retained.source_in + end - begin;
            retained.start = begin - command.span.in_us;
            retained.fade_in.duration_us = std::min(retained.fade_in.duration_us, retained.duration());
            retained.fade_out.duration_us = std::min(retained.fade_out.duration_us, retained.duration());
            result.push_back(std::move(retained));
        }
        std::erase_if(transitions_, [&](const auto& junction) {
            const auto* incoming = clip(junction.incoming);
            const auto* outgoing = clip(junction.outgoing);
            return !incoming || !outgoing || incoming->start < command.span.in_us ||
                   outgoing->end() > command.span.out_us;
        });
        for (auto& junction : transitions_) {
            junction.in_us -= command.span.in_us;
            junction.out_us -= command.span.in_us;
            junction.cut_us = std::clamp(junction.cut_us, command.span.in_us, command.span.out_us) - command.span.in_us;
        }
        clips_ = std::move(result);
        result_range_.in_us =
            std::clamp(result_range_.in_us, command.span.in_us, command.span.out_us) - command.span.in_us;
        if (result_range_.out_mode == ResultOutMode::Fixed)
            result_range_.out_us =
                std::clamp(result_range_.out_us, command.span.in_us, command.span.out_us) - command.span.in_us;
    }
    void execute(const Trim& command) {
        auto requested = command.targets;
        if (requested.empty())
            requested.push_back(command.lead_id);
        const auto ids = targets(requested, command.expand_linked);
        const auto* lead = clip(command.lead_id);
        if (last_error_ || !lead || !contains(ids, command.lead_id)) {
            fail(EditErrorCode::InvalidTarget, {command.lead_id});
            return;
        }
        Time lower = (std::numeric_limits<Time>::min)();
        Time upper = (std::numeric_limits<Time>::max)();
        Time suffix = 0;
        for (const auto id : ids) {
            const auto* clip = this->clip(id);
            const auto* source = asset(clip->asset);
            Time source_begin = 0;
            Time source_end = source->duration;
            if (clip->stream && source->source_descriptor)
                for (const auto& stream : source->source_descriptor->streams)
                    if (stream.ref.stream_identity == clip->stream->stream_identity) {
                        source_begin = stream.source_span.in_us;
                        source_end = stream.source_span.out_us;
                    }
            Time minimum = 1;
            const auto* track = mutableTrack(clip->track);
            if (track->type == TrackType::Video && output_fps_.valid()) {
                const auto next = output_fps_.nextFrame(clip->start);
                if (next)
                    minimum = *next - clip->start;
            } else if (track->type == TrackType::Audio && clip->stream) {
                minimum = 21;
            }
            if (command.edge == TrimEdge::Left) {
                lower = std::max(lower, source_begin - clip->source_in);
                if (command.mode == TrimMode::Fixed)
                    lower = std::max(lower, -clip->start);
                upper = std::min(upper, clip->duration() - minimum);
            } else {
                lower = std::max(lower, minimum - clip->duration());
                upper = std::min(upper, source_end - clip->source_out);
            }
            suffix = std::max(suffix, clip->end());
        }
        if (lower > upper) {
            fail(EditErrorCode::SourceBounds, ids);
            return;
        }
        const Time delta = std::clamp(command.delta_us, lower, upper);
        if (delta == 0)
            return;
        const Time shift = command.edge == TrimEdge::Left ? -delta : delta;
        if (command.mode == TrimMode::Ripple)
            for (const auto& clip : clips_)
                if (!contains(ids, clip.id) && clip.start < suffix && clip.end() > suffix) {
                    fail(EditErrorCode::RippleConflict, {clip.id});
                    return;
                }
        for (auto& clip : clips_) {
            if (contains(ids, clip.id)) {
                if (command.edge == TrimEdge::Left) {
                    clip.source_in += delta;
                    if (command.mode == TrimMode::Fixed)
                        clip.start += delta;
                } else {
                    clip.source_out += delta;
                }
                if (!fitFades(clip, command.edge == TrimEdge::Left ? FadeEdge::In : FadeEdge::Out))
                    return;
            } else if (command.mode == TrimMode::Ripple && clip.start >= suffix) {
                if (!addTime(clip.start, shift, clip.start)) {
                    fail(EditErrorCode::InvalidTime, {clip.id});
                    return;
                }
            }
        }
        if (command.mode == TrimMode::Ripple) {
            if (shift < 0) {
                mapRange({{suffix + shift, suffix}});
            } else {
                if (result_range_.in_us >= suffix && !addTime(result_range_.in_us, shift, result_range_.in_us))
                    fail(EditErrorCode::InvalidTime);
                if (result_range_.out_mode == ResultOutMode::Fixed && result_range_.out_us >= suffix &&
                    !addTime(result_range_.out_us, shift, result_range_.out_us))
                    fail(EditErrorCode::InvalidTime);
            }
        }
    }
    void execute(const SetClipEnabled& command) {
        if (auto* clip = mutableClip(command.id))
            clip->enabled = command.enabled;
        else
            fail(EditErrorCode::InvalidTarget, {command.id});
    }
    void execute(const SetGain& command) {
        if (!std::isfinite(command.gain_db)) {
            fail(EditErrorCode::NonfiniteValue, {command.id});
            return;
        }
        if (command.target == GainTarget::Track) {
            auto* track = mutableTrack(command.id);
            if (!track || track->type != TrackType::Audio)
                fail(EditErrorCode::WrongTrackKind, {command.id});
            else
                track->gain_db = command.gain_db;
        } else {
            auto* clip = mutableClip(command.id);
            const auto* track = clip ? mutableTrack(clip->track) : nullptr;
            if (!clip || !track || track->type != TrackType::Audio)
                fail(EditErrorCode::WrongTrackKind, {command.id});
            else
                clip->gain_db = command.gain_db;
        }
    }
    void execute(const SetClipOpacity& command) {
        auto* clip = mutableClip(command.id);
        const auto* track = clip ? mutableTrack(clip->track) : nullptr;
        if (!std::isfinite(command.opacity) || command.opacity < 0 || command.opacity > 1)
            fail(EditErrorCode::NonfiniteValue, {command.id});
        else if (!clip || !track || track->type != TrackType::Video)
            fail(EditErrorCode::WrongTrackKind, {command.id});
        else
            clip->opacity = command.opacity;
    }
    void execute(const CreateTrack& command) {
        Track track{nextId(), command.kind, command.name};
        tracks_.insert(tracks_.begin() + std::min(command.index, tracks_.size()), std::move(track));
    }
    void execute(const ReorderTrack& command) {
        auto* track = mutableTrack(command.track_id);
        if (!track) {
            fail(EditErrorCode::InvalidTarget, {command.track_id});
            return;
        }
        auto copy = *track;
        std::erase_if(tracks_, [&](const auto& item) { return item.id == command.track_id; });
        tracks_.insert(tracks_.begin() + std::min(command.index, tracks_.size()), std::move(copy));
    }
    void execute(const DeleteTrack& command) {
        const auto* track = mutableTrack(command.track_id);
        if (!track) {
            fail(EditErrorCode::InvalidTarget, {command.track_id});
            return;
        }
        std::vector<Id> removed;
        for (const auto& clip : clips_)
            if (clip.track == command.track_id)
                removed.push_back(clip.id);
        if (!removed.empty() && command.confirmed_contents_revision != revision_) {
            fail(EditErrorCode::StaleRevision, {command.track_id});
            return;
        }
        removeTargets(removed, false, false);
        std::erase_if(tracks_, [&](const auto& item) { return item.id == command.track_id; });
        if (std::none_of(tracks_.begin(), tracks_.end(),
                         [](const auto& item) { return item.type == TrackType::Video; }))
            tracks_.insert(tracks_.begin(), {nextId(), TrackType::Video});
    }
    void execute(const Unlink& command) {
        const auto ids = targets(command.targets, false);
        for (auto& clip : clips_)
            if (contains(ids, clip.id))
                clip.group = 0;
    }
    void execute(const Relink& command) {
        const auto ids = targets(command.targets, false);
        if (last_error_ || ids.size() < 2)
            return;
        const auto group = nextId();
        for (auto& clip : clips_)
            if (contains(ids, clip.id))
                clip.group = group;
    }
    void execute(const SetFade& command) {
        auto* clip = mutableClip(command.clip_id);
        if (!clip) {
            fail(EditErrorCode::InvalidTarget, {command.clip_id});
            return;
        }
        if (command.fade.duration_us < 0 || !engine::EvaluateCurve(command.fade.curve, 0)) {
            fail(EditErrorCode::InvalidCurve, {command.clip_id});
            return;
        }
        if (command.fade.duration_us > 0 &&
            std::any_of(transitions_.begin(), transitions_.end(), [&](const auto& junction) {
                return command.edge == FadeEdge::Out ? junction.outgoing == clip->id : junction.incoming == clip->id;
            })) {
            fail(EditErrorCode::TransitionBoundaryConflict, {command.clip_id});
            return;
        }
        auto fade = command.fade;
        const auto& opposite = command.edge == FadeEdge::In ? clip->fade_out : clip->fade_in;
        fade.duration_us = std::min(fade.duration_us, clip->duration() - opposite.duration_us);
        (command.edge == FadeEdge::In ? clip->fade_in : clip->fade_out) = fade;
    }
    template <typename T> void execute(const T&) {
        fail(EditErrorCode::UnsupportedProfile);
    }
    struct Command {
        std::vector<Clip> before;
        std::vector<Clip> after;
        std::vector<Transition> transitions_before;
        std::vector<Transition> transitions_after;
        std::vector<Track> tracks_before;
        std::vector<Track> tracks_after;
        std::vector<Id> order_before;
        std::vector<Id> order_after;
        ResultRange range_before;
        ResultRange range_after;
        RationalFps fps_before;
        RationalFps fps_after;
        int width_before = 0;
        int width_after = 0;
        int height_before = 0;
        int height_after = 0;
        size_t bytes = 0;
    };
    Id nextId() {
        if (next_id_ == (std::numeric_limits<Id>::max)()) {
            fail(EditErrorCode::ResourceLimitExceeded);
            return 0;
        }
        return ++next_id_;
    }
    template <typename T> static std::vector<T> difference(const std::vector<T>& left, const std::vector<T>& right) {
        std::vector<T> result;
        for (const auto& value : left)
            if (std::find(right.begin(), right.end(), value) == right.end())
                result.push_back(value);
        return result;
    }
    template <typename T>
    static void patchEntities(std::vector<T>& values, const std::vector<T>& removed, const std::vector<T>& inserted) {
        std::erase_if(values, [&](const auto& value) {
            return std::any_of(removed.begin(), removed.end(), [&](const auto& old) { return value.id == old.id; });
        });
        values.insert(values.end(), inserted.begin(), inserted.end());
    }
    void patchTracks(const std::vector<Track>& removed, const std::vector<Track>& inserted,
                     const std::vector<Id>& order) {
        patchEntities(tracks_, removed, inserted);
        std::sort(tracks_.begin(), tracks_.end(), [&](const auto& left, const auto& right) {
            return std::find(order.begin(), order.end(), left.id) < std::find(order.begin(), order.end(), right.id);
        });
    }
    bool validate() {
        std::set<Id> track_ids;
        std::set<Id> clip_ids;
        for (const auto& track : tracks_)
            if (!track.id || !track_ids.insert(track.id).second || !std::isfinite(track.gain_db)) {
                fail(EditErrorCode::InvalidTarget, {track.id});
                return false;
            }
        order(clips_);
        for (const auto& clip : clips_) {
            const auto* source = asset(clip.asset);
            const auto* track = mutableTrack(clip.track);
            Time end = 0;
            if (!clip.id || !clip_ids.insert(clip.id).second || !source || !track) {
                fail(EditErrorCode::InvalidTarget, {clip.id});
                return false;
            }
            Time source_begin = 0;
            Time source_end = source->duration;
            if (clip.stream) {
                if (!source->source_descriptor || clip.stream->asset_id != clip.asset) {
                    fail(EditErrorCode::SourceChanged, {clip.id});
                    return false;
                }
                const auto& streams = source->source_descriptor->streams;
                const auto stream = std::find_if(streams.begin(), streams.end(), [&](const auto& candidate) {
                    return candidate.ref.stream_identity == clip.stream->stream_identity &&
                           candidate.ref.container_index == clip.stream->container_index &&
                           candidate.ref.kind == clip.stream->kind;
                });
                if (stream == streams.end()) {
                    fail(EditErrorCode::SourceChanged, {clip.id});
                    return false;
                }
                source_begin = stream->source_span.in_us;
                source_end = stream->source_span.out_us;
            }
            if (clip.source_in < source_begin || clip.source_out <= clip.source_in || clip.source_out > source_end ||
                clip.start < 0 || !addTime(clip.start, clip.source_out - clip.source_in, end)) {
                fail(EditErrorCode::SourceBounds, {clip.id});
                return false;
            }
            if (clip.stream &&
                ((clip.stream->kind == engine::TimelineStreamKind::Audio) != (track->type == TrackType::Audio))) {
                fail(EditErrorCode::WrongTrackKind, {clip.id});
                return false;
            }
            if (!std::isfinite(clip.gain_db) || !std::isfinite(clip.opacity) || clip.opacity < 0 || clip.opacity > 1) {
                fail(EditErrorCode::NonfiniteValue, {clip.id});
                return false;
            }
        }
        if (!normalizeJunctions())
            return false;
        for (auto& clip : clips_)
            if (!fitFades(clip))
                return false;
        const auto total = duration();
        result_range_.in_us = std::clamp(result_range_.in_us, Time{0}, total);
        result_range_.out_us = result_range_.out_mode == ResultOutMode::FollowTimelineEnd
                                   ? 0
                                   : std::clamp(result_range_.out_us, result_range_.in_us, total);
        return !last_error_;
    }
    static size_t commandBytes(const Command& command) {
        size_t bytes =
            sizeof(Command) + (command.before.capacity() + command.after.capacity()) * sizeof(Clip) +
            (command.transitions_before.capacity() + command.transitions_after.capacity()) * sizeof(Transition) +
            (command.tracks_before.capacity() + command.tracks_after.capacity()) * sizeof(Track) +
            (command.order_before.capacity() + command.order_after.capacity()) * sizeof(Id);
        for (const auto& track : command.tracks_before)
            bytes += track.name.capacity();
        for (const auto& track : command.tracks_after)
            bytes += track.name.capacity();
        return bytes;
    }
    bool publish(Workspace& candidate) {
        if (!candidate.validate()) {
            last_error_ = candidate.last_error_;
            return false;
        }
        if (candidate.clips_ == clips_ && candidate.transitions_ == transitions_ && candidate.tracks_ == tracks_ &&
            candidate.result_range_ == result_range_ && candidate.output_fps_ == output_fps_ &&
            candidate.output_width_ == output_width_ && candidate.output_height_ == output_height_)
            return false;
        if (!candidate_mode_) {
            if (revision_ == (std::numeric_limits<Revision>::max)()) {
                fail(EditErrorCode::ResourceLimitExceeded);
                return false;
            }
            Command command;
            command.before = difference(clips_, candidate.clips_);
            command.after = difference(candidate.clips_, clips_);
            command.transitions_before = difference(transitions_, candidate.transitions_);
            command.transitions_after = difference(candidate.transitions_, transitions_);
            command.tracks_before = difference(tracks_, candidate.tracks_);
            command.tracks_after = difference(candidate.tracks_, tracks_);
            for (const auto& track : tracks_)
                command.order_before.push_back(track.id);
            for (const auto& track : candidate.tracks_)
                command.order_after.push_back(track.id);
            command.range_before = result_range_;
            command.range_after = candidate.result_range_;
            command.fps_before = output_fps_;
            command.fps_after = candidate.output_fps_;
            command.width_before = output_width_;
            command.width_after = candidate.output_width_;
            command.height_before = output_height_;
            command.height_after = candidate.output_height_;
            command.bytes = commandBytes(command);
            if (command.bytes > kHistoryBudget) {
                fail(EditErrorCode::ResourceLimitExceeded);
                return false;
            }
            while (history_.size() > cursor_) {
                history_bytes_ -= history_.back().bytes;
                history_.pop_back();
            }
            while (!history_.empty() && history_bytes_ > kHistoryBudget - command.bytes) {
                history_bytes_ -= history_.front().bytes;
                history_.erase(history_.begin());
                --cursor_;
            }
            history_bytes_ += command.bytes;
            history_.push_back(std::move(command));
            ++cursor_;
            ++revision_;
        }
        clips_ = std::move(candidate.clips_);
        tracks_ = std::move(candidate.tracks_);
        transitions_ = std::move(candidate.transitions_);
        result_range_ = candidate.result_range_;
        output_fps_ = candidate.output_fps_;
        output_width_ = candidate.output_width_;
        output_height_ = candidate.output_height_;
        next_id_ = candidate.next_id_;
        selection_ = candidate.selection_;
        seek(playhead_);
        snapshot_cache_.reset();
        return true;
    }
    static bool linked(const Clip& a, const Clip& b) {
        return a.id == b.id || (a.group && a.group == b.group);
    }
    static void order(std::vector<Clip>& clips) {
        std::sort(clips.begin(), clips.end(), [](const auto& a, const auto& b) {
            if (a.track != b.track)
                return a.track < b.track;
            if (a.start != b.start)
                return a.start < b.start;
            return a.id < b.id;
        });
    }
    bool commit(std::vector<Clip> after, Id selection) {
        return commit(std::move(after), selection, transitions_);
    }
    bool commit(std::vector<Clip> after, Id selection, std::vector<Transition> transitions) {
        auto candidate = workingCopy();
        candidate.clips_ = std::move(after);
        candidate.selection_ = selection;
        candidate.transitions_ = std::move(transitions);
        return publish(candidate);
    }
    void apply(const std::vector<Clip>& remove, const std::vector<Clip>& add) {
        std::erase_if(clips_, [&remove](const auto& c) {
            return std::any_of(remove.begin(), remove.end(), [&c](const auto& r) { return r.id == c.id; });
        });
        clips_.insert(clips_.end(), add.begin(), add.end());
        order(clips_);
    }
    static constexpr size_t kHistoryBudget = 64 * 1024 * 1024;
    Revision revision_ = 0;
    size_t history_bytes_ = 0;
    ResultRange result_range_;
    RationalFps output_fps_;
    int output_width_ = 0;
    int output_height_ = 0;
    bool candidate_mode_ = false;
    std::optional<EditError> last_error_;
    mutable std::shared_ptr<const EditSnapshot> snapshot_cache_;
    Id next_id_ = 0;
    uint64_t monitoring_revision_ = 0;
    Id selection_ = 0;
    Time playhead_ = 0;
    std::vector<Asset> assets_;
    std::vector<Track> tracks_;
    std::vector<Clip> clips_;
    std::vector<Transition> transitions_;
    std::vector<Command> history_;
    size_t cursor_ = 0;
};

inline EditPreview PreviewEdit(const EditSnapshot& snapshot, const EditCommand& command) {
    Workspace candidate;
    candidate.next_id_ = snapshot.id_high_watermark;
    candidate.tracks_ = snapshot.tracks;
    candidate.clips_ = snapshot.clips;
    candidate.transitions_ = snapshot.junctions;
    candidate.result_range_ = snapshot.result_range;
    candidate.output_fps_ = snapshot.output_fps;
    candidate.output_width_ = snapshot.output_width;
    candidate.output_height_ = snapshot.output_height;
    candidate.revision_ = snapshot.revision;
    candidate.candidate_mode_ = true;
    for (const auto& asset : snapshot.assets)
        candidate.assets_.push_back(*asset);
    std::visit([&](const auto& value) { candidate.execute(value); }, command);
    const auto valid = !candidate.last_error_ && candidate.validate();
    return {snapshot.revision,
            valid,
            candidate.last_error_,
            std::move(candidate.tracks_),
            std::move(candidate.clips_),
            std::move(candidate.transitions_),
            0};
}

} // namespace exosnap::edit
