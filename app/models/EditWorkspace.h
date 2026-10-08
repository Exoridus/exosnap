#pragma once
#include "RecordingMarker.h"
#include <exosnap/engine/edit_timeline.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace exosnap::edit {

using Id = uint64_t;
using Time = int64_t;
enum class TrackType { Video, Audio };
enum class AssetState { Available, Missing, Failed };
enum class TransitionKind { Crossfade };

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
    bool operator==(const Clip&) const = default;
};

struct Track {
    Id id = 0;
    TrackType type = TrackType::Video;
};

struct Transition {
    Id outgoing = 0;
    Id incoming = 0;
    TransitionKind kind = TransitionKind::Crossfade;
    Time duration = 0;
    bool operator==(const Transition&) const = default;
};

// Values use microseconds. Assets are immutable media references; history stores only clip deltas.
class Workspace {
  public:
    Workspace() : tracks_{{nextId(), TrackType::Video}, {nextId(), TrackType::Audio}} {
    }

    [[nodiscard]] const std::vector<Asset>& assets() const {
        return assets_;
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
        for (const auto& c : clips_) {
            const auto t =
                std::find_if(tracks_.begin(), tracks_.end(), [&c](const auto& track) { return track.id == c.track; });
            result.clips.push_back(
                {c.id, c.asset, c.group, t->type == TrackType::Audio, c.start, c.source_in, c.source_out});
        }
        for (const auto& t : transitions_)
            result.crossfades.push_back({t.outgoing, t.incoming, t.duration});
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
                c.start += old - duration;
        return commit(std::move(after), outgoing, std::move(transitions));
    }
    bool removeCrossfade(Id outgoing) {
        const auto found = std::find_if(transitions_.begin(), transitions_.end(),
                                        [outgoing](const auto& t) { return t.outgoing == outgoing; });
        if (found == transitions_.end())
            return false;
        const auto* incoming = clip(found->incoming);
        if (!incoming)
            return false;
        auto after = clips_;
        for (auto& c : after)
            if (c.start >= incoming->start)
                c.start += found->duration;
        auto transitions = transitions_;
        std::erase_if(transitions, [outgoing](const auto& t) { return t.outgoing == outgoing; });
        return commit(std::move(after), outgoing, std::move(transitions));
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
        const Id id = nextId();
        tracks_.push_back({id, type});
        return id;
    }
    Id addAsset(Asset asset) {
        for (auto& current : assets_)
            if (current.path == asset.path && current.state == asset.state &&
                (current.duration == asset.duration || current.duration <= 0)) {
                if (current.duration <= 0 && asset.duration > 0) {
                    asset.id = current.id;
                    current = std::move(asset);
                }
                return current.id;
            }
        asset.id = nextId();
        assets_.push_back(std::move(asset));
        return assets_.back().id;
    }
    bool insert(Id asset_id, Time start = -1) {
        return insertAssets({asset_id}, start);
    }
    bool insertAssets(const std::vector<Id>& ids, Time start = -1) {
        if (start < 0)
            start = duration();
        auto after = clips_;
        Id selected = 0;
        for (Id asset_id : ids) {
            const auto* source = asset(asset_id);
            if (!source || source->duration <= 0)
                return false;
            const Id group = nextId();
            for (TrackType type : {TrackType::Video, TrackType::Audio}) {
                if (type == TrackType::Audio && !source->audio)
                    continue;
                const auto track =
                    std::find_if(tracks_.begin(), tracks_.end(), [type](const auto& t) { return t.type == type; });
                if (track == tracks_.end())
                    continue;
                const Id id = nextId();
                if (!selected)
                    selected = id;
                after.push_back({id, asset_id, track->id, group, start, 0, source->duration});
            }
            start += source->duration;
        }
        return commit(std::move(after), selected);
    }
    bool move(Id id, Time start) {
        const auto* selected = clip(id);
        if (!selected || start < 0)
            return false;
        const Time delta = start - selected->start;
        auto after = clips_;
        for (auto& c : after)
            if (linked(c, *selected))
                c.start += delta;
        return commit(std::move(after), id);
    }
    bool trim(Id id, Time source_in, Time source_out) {
        const auto* selected = clip(id);
        if (!selected)
            return false;
        const Time left = source_in - selected->source_in;
        const Time right = source_out - selected->source_out;
        auto after = clips_;
        for (auto& c : after)
            if (linked(c, *selected)) {
                c.start += left;
                c.source_in += left;
                c.source_out += right;
            }
        return commit(std::move(after), id);
    }
    bool split(Id id, Time at) {
        const auto* selected = clip(id);
        if (!selected || at <= selected->start || at >= selected->end())
            return false;
        auto after = clips_;
        std::vector<Clip> tails;
        const Id tail_group = nextId();
        for (auto& c : after)
            if (linked(c, *selected)) {
                const Time cut = c.source_in + at - c.start;
                Clip tail = c;
                tail.id = nextId();
                tail.group = tail_group;
                tail.start = at;
                tail.source_in = cut;
                c.source_out = cut;
                tails.push_back(tail);
            }
        after.insert(after.end(), tails.begin(), tails.end());
        return commit(std::move(after), id);
    }
    bool remove(Id id, bool ripple) {
        const auto* selected = clip(id);
        if (!selected)
            return false;
        auto after = clips_;
        auto transitions = transitions_;
        for (const auto& transition : transitions_) {
            const auto* a = clip(transition.outgoing);
            const auto* b = clip(transition.incoming);
            if (!a || !b || (!linked(*selected, *a) && !linked(*selected, *b)))
                continue;
            const auto incoming = std::find_if(after.begin(), after.end(),
                                               [&transition](const auto& c) { return c.id == transition.incoming; });
            const Time boundary = incoming->start;
            for (auto& c : after)
                if (c.start >= boundary)
                    c.start += transition.duration;
            std::erase_if(transitions, [&transition](const auto& t) { return t.outgoing == transition.outgoing; });
        }
        const Clip removed = *std::find_if(after.begin(), after.end(), [id](const auto& c) { return c.id == id; });
        std::erase_if(after, [&removed](const auto& c) { return linked(c, removed); });
        if (ripple) {
            // Close this interval globally so linked tracks retain their alignment.
            for (auto& c : after) {
                if (c.start < removed.end() && c.end() > removed.start)
                    return false;
                if (c.start >= removed.end())
                    c.start -= removed.duration();
            }
        }
        return commit(std::move(after), 0, std::move(transitions));
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
        if (!canUndo())
            return false;
        const auto& command = history_[--cursor_];
        apply(command.after, command.before);
        transitions_ = command.transitions_before;
        selection_ = command.selection_before;
        seek(playhead_);
        return true;
    }
    bool redo() {
        if (!canRedo())
            return false;
        const auto& command = history_[cursor_++];
        apply(command.before, command.after);
        transitions_ = command.transitions_after;
        selection_ = command.selection_after;
        seek(playhead_);
        return true;
    }

  private:
    struct Command {
        std::vector<Clip> before;
        std::vector<Clip> after;
        std::vector<Transition> transitions_before;
        std::vector<Transition> transitions_after;
        Id selection_before = 0;
        Id selection_after = 0;
    };
    Id nextId() {
        return ++next_id_;
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
        order(after);
        const auto find = [&after](Id id) -> const Clip* {
            const auto it = std::find_if(after.begin(), after.end(), [id](const auto& c) { return c.id == id; });
            return it == after.end() ? nullptr : &*it;
        };
        for (const auto& t : transitions) {
            const auto* a = find(t.outgoing);
            const auto* b = find(t.incoming);
            if (!a || !b || a->track != b->track || a->start >= b->start || t.duration <= 0 ||
                a->end() - b->start != t.duration || a->end() >= b->end())
                return false;
            for (const auto& c : after)
                if (c.track == a->track && c.id != a->id && c.id != b->id && c.start < a->end() && c.end() > b->start)
                    return false;
        }
        const auto permitted = [&](const Clip& a, const Clip& b) {
            return std::any_of(transitions.begin(), transitions.end(), [&](const auto& t) {
                const auto* outgoing = find(t.outgoing);
                const auto* incoming = find(t.incoming);
                return outgoing && incoming && linked(a, *outgoing) && linked(b, *incoming) &&
                       a.start == outgoing->start && a.end() == outgoing->end() && b.start == incoming->start &&
                       b.end() == incoming->end();
            });
        };
        const Clip* previous = nullptr;
        for (const auto& c : after) {
            const auto* source = asset(c.asset);
            if (!source || c.start < 0 || c.source_in < 0 || c.duration() <= 0 || c.source_out > source->duration)
                return false;
            if (previous && previous->track == c.track && previous->end() > c.start && !permitted(*previous, c))
                return false;
            previous = &c;
        }
        if (after == clips_ && transitions == transitions_)
            return false;
        Command command;
        command.transitions_before = transitions_;
        command.transitions_after = transitions;
        command.selection_before = selection_;
        command.selection_after = selection;
        for (const auto& c : clips_)
            if (std::find_if(after.begin(), after.end(), [&c](const Clip& candidate) { return candidate == c; }) ==
                after.end())
                command.before.push_back(c);
        for (const auto& c : after)
            if (std::find_if(clips_.begin(), clips_.end(), [&c](const Clip& candidate) { return candidate == c; }) ==
                clips_.end())
                command.after.push_back(c);
        history_.resize(cursor_);
        history_.push_back(std::move(command));
        ++cursor_;
        clips_ = std::move(after);
        transitions_ = std::move(transitions);
        selection_ = selection;
        seek(playhead_);
        return true;
    }
    void apply(const std::vector<Clip>& remove, const std::vector<Clip>& add) {
        std::erase_if(clips_, [&remove](const auto& c) {
            return std::any_of(remove.begin(), remove.end(), [&c](const auto& r) { return r.id == c.id; });
        });
        clips_.insert(clips_.end(), add.begin(), add.end());
        order(clips_);
    }
    Id next_id_ = 0;
    Id selection_ = 0;
    Time playhead_ = 0;
    std::vector<Asset> assets_;
    std::vector<Track> tracks_;
    std::vector<Clip> clips_;
    std::vector<Transition> transitions_;
    std::vector<Command> history_;
    size_t cursor_ = 0;
};

} // namespace exosnap::edit
