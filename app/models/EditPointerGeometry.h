#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

namespace exosnap::edit {

enum class ClipPointerTarget { None, Header, Content, TrimStart, TrimEnd, FadeIn, FadeOut, Gain, Junction };
enum class RangePointerTarget { None, Create, Start, End, Body };

struct TrackRowBounds {
    double top = 0;
    double bottom = 0;
};

inline RangePointerTarget ResolveRangePointerTarget(double x, double start, double end, double hit = 6,
                                                    bool active = true,
                                                    RangePointerTarget last_edge = RangePointerTarget::None) {
    if (!std::isfinite(x) || !std::isfinite(hit) || hit < 0)
        return RangePointerTarget::None;
    if (!active)
        return RangePointerTarget::Create;
    if (!std::isfinite(start) || !std::isfinite(end) || end < start || !std::isfinite(end - start))
        return RangePointerTarget::None;
    const auto from_start = std::abs(x - start);
    const auto from_end = std::abs(x - end);
    if (from_start <= hit || from_end <= hit) {
        if (from_start == from_end)
            return last_edge == RangePointerTarget::End ? RangePointerTarget::End : RangePointerTarget::Start;
        return from_start < from_end ? RangePointerTarget::Start : RangePointerTarget::End;
    }
    if (end - start > hit * 2 && x > start + hit && x < end - hit)
        return RangePointerTarget::Body;
    return RangePointerTarget::Create;
}

inline double EdgeAutoScrollVelocity(double pointer, double extent, double edge = 24, double cap = 600) {
    if (!std::isfinite(pointer) || !std::isfinite(extent) || !std::isfinite(edge) || !std::isfinite(cap) ||
        extent <= 0 || edge <= 0 || cap < 0)
        return 0;
    const auto from_start = pointer;
    const auto from_end = extent - pointer;
    if (from_start == from_end)
        return 0;
    const auto distance = std::min(from_start, from_end);
    const auto speed = std::clamp((edge - distance) / edge, 0.0, 1.0) * cap;
    return from_start < from_end ? -speed : speed;
}

inline int ResolveDragTrack(int previous, double pointer, std::span<const TrackRowBounds> rows, double hysteresis = 4) {
    if (previous < 0 || static_cast<std::size_t>(previous) >= rows.size() ||
        rows.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) || !std::isfinite(pointer) ||
        !std::isfinite(hysteresis) || hysteresis < 0)
        return -1;
    for (std::size_t index = 0; index < rows.size(); ++index)
        if (!std::isfinite(rows[index].top) || !std::isfinite(rows[index].bottom) ||
            rows[index].bottom <= rows[index].top || (index > 0 && rows[index].top < rows[index - 1].bottom))
            return -1;
    const auto midpoint = [&rows](int index) { return rows[index].top / 2 + rows[index].bottom / 2; };
    auto target = previous;
    while (static_cast<std::size_t>(target + 1) < rows.size() && pointer - midpoint(target + 1) >= hysteresis)
        ++target;
    while (target > 0 && midpoint(target - 1) - pointer >= hysteresis)
        --target;
    return target;
}

struct ClipPointerGeometry {
    double width = 0;
    double height = 0;
    double header_height = 0;
    bool audio = false;
    double gain_db = 0;
    bool fade_in_available = true;
    bool fade_out_available = true;
    bool junction_content = false;
};

inline double ClipGainLineY(const ClipPointerGeometry& clip) {
    const auto body = clip.height - clip.header_height;
    const auto fraction = clip.gain_db >= 0 ? 0.3 * (1 - clip.gain_db / 24) : 0.3 + 0.7 * (-clip.gain_db / 60);
    return clip.header_height + body * fraction;
}

inline ClipPointerTarget ResolveClipPointerTarget(const ClipPointerGeometry& clip, double x, double y) {
    if (!std::isfinite(clip.width) || !std::isfinite(clip.height) || !std::isfinite(x) || !std::isfinite(y) ||
        clip.width <= 0 || clip.height <= 0 || x < 0 || x >= clip.width || y < 0 || y >= clip.height)
        return ClipPointerTarget::None;
    if (clip.width < 24)
        return ClipPointerTarget::Header;
    if (clip.width >= 40 && clip.height >= 44 && y < 14) {
        if (clip.fade_in_available && x < 14)
            return ClipPointerTarget::FadeIn;
        if (clip.fade_out_available && x >= clip.width - 14)
            return ClipPointerTarget::FadeOut;
    }
    const auto edge = clip.width < 40 ? 4 : 6;
    if (x < edge)
        return ClipPointerTarget::TrimStart;
    if (x >= clip.width - edge)
        return ClipPointerTarget::TrimEnd;
    if (clip.height < 44 || y < clip.header_height)
        return ClipPointerTarget::Header;
    if (clip.junction_content)
        return ClipPointerTarget::Junction;
    if (clip.audio && clip.height - clip.header_height >= 26 && std::isfinite(clip.gain_db) &&
        std::abs(y - ClipGainLineY(clip)) <= 3)
        return ClipPointerTarget::Gain;
    return ClipPointerTarget::Content;
}

inline bool ClipPointerThresholdReached(ClipPointerTarget target, double dx, double dy, double threshold) {
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(threshold) || threshold < 0)
        return false;
    switch (target) {
    case ClipPointerTarget::TrimStart:
    case ClipPointerTarget::TrimEnd:
    case ClipPointerTarget::FadeIn:
    case ClipPointerTarget::FadeOut:
        return std::abs(dx) >= threshold;
    case ClipPointerTarget::Gain:
        return std::abs(dy) >= threshold;
    case ClipPointerTarget::Header:
    case ClipPointerTarget::Content:
        return std::hypot(dx, dy) >= threshold;
    default:
        return false;
    }
}

} // namespace exosnap::edit
