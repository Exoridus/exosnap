#pragma once
#include "RecordingMarker.h"

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
        const Clip removed = *selected;
        auto after = clips_;
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
        return commit(std::move(after), 0);
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
        selection_ = command.selection_before;
        seek(playhead_);
        return true;
    }
    bool redo() {
        if (!canRedo())
            return false;
        const auto& command = history_[cursor_++];
        apply(command.before, command.after);
        selection_ = command.selection_after;
        seek(playhead_);
        return true;
    }

  private:
    struct Command {
        std::vector<Clip> before;
        std::vector<Clip> after;
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
        order(after);
        const Clip* previous = nullptr;
        for (const auto& c : after) {
            const auto* source = asset(c.asset);
            if (!source || c.start < 0 || c.source_in < 0 || c.duration() <= 0 || c.source_out > source->duration)
                return false;
            if (previous && previous->track == c.track && previous->end() > c.start)
                return false;
            previous = &c;
        }
        if (after == clips_)
            return false;
        Command command;
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
