#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace exosnap::edit {

struct PlaybackInterval {
    int64_t start_us = 0;
    int64_t end_us = 0;
    bool operator==(const PlaybackInterval&) const = default;
};

class TransportBounds {
  public:
    void setDuration(int64_t duration) {
        duration_ = std::max<int64_t>(0, duration);
        if (loop_)
            loop_ = clamp(*loop_);
        if (once_)
            once_ = clamp(*once_);
    }
    void setLoop(bool enabled, std::optional<PlaybackInterval> active_range = {}) {
        enabled_ = enabled;
        if (enabled)
            loop_ = active_range ? clamp(*active_range) : std::nullopt;
    }
    void commitRange(PlaybackInterval range) {
        if (enabled_)
            loop_ = clamp(range);
    }
    void clearRange() {
        loop_.reset();
    }
    bool beginOnce(PlaybackInterval range) {
        const auto candidate = clamp(range);
        if (!candidate)
            return false;
        once_ = candidate;
        return true;
    }
    void seek(int64_t position) {
        const auto current = interval();
        if (position < current.start_us || position >= current.end_us) {
            once_.reset();
            enabled_ = false;
        }
    }
    void completeOnce() {
        once_.reset();
    }
    bool loopEnabled() const {
        return enabled_;
    }
    bool onceActive() const {
        return once_.has_value();
    }
    bool repeating() const {
        return enabled_ && !once_;
    }
    std::optional<PlaybackInterval> loopRegion() const {
        return loop_;
    }
    std::optional<PlaybackInterval> onceBounds() const {
        return once_;
    }
    PlaybackInterval interval() const {
        return once_.value_or(enabled_ && loop_ ? *loop_ : PlaybackInterval{0, duration_});
    }

  private:
    std::optional<PlaybackInterval> clamp(PlaybackInterval range) const {
        range.start_us = std::clamp<int64_t>(range.start_us, 0, duration_);
        range.end_us = std::clamp<int64_t>(range.end_us, 0, duration_);
        return range.start_us < range.end_us ? std::optional(range) : std::nullopt;
    }
    int64_t duration_ = 0;
    bool enabled_ = false;
    std::optional<PlaybackInterval> loop_;
    std::optional<PlaybackInterval> once_;
};

} // namespace exosnap::edit
