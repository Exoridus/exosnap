#include "EditViewportAdapter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace exosnap::quick {
namespace {
constexpr qreal kMaximumScale = 96000;

qint64 RoundedTime(long double value, qint64 maximum) {
    if (value <= 0)
        return 0;
    if (value >= static_cast<long double>(maximum))
        return maximum;
    const auto rounded = std::round(value);
    if (rounded >= static_cast<long double>(maximum))
        return maximum;
    return static_cast<qint64>(rounded);
}
} // namespace

EditViewportAdapter::EditViewportAdapter(QObject* parent) : QObject(parent) {
}

qreal EditViewportAdapter::minimumScale() const {
    return end_ > 0 ? width_ * 1000000.0 / static_cast<qreal>(end_) : 40;
}

qreal EditViewportAdapter::maximumScale() const {
    return std::max(minimumScale(), kMaximumScale);
}

qreal EditViewportAdapter::boundedScale(qreal scale) const {
    return std::clamp(scale, minimumScale(), maximumScale());
}

void EditViewportAdapter::normalize() {
    if (view_.fit == Fit::Timeline) {
        view_.start = 0;
        view_.scale = minimumScale();
    } else if (view_.fit == Fit::Interval) {
        view_.from = std::clamp<qint64>(view_.from, 0, end_);
        view_.to = std::clamp<qint64>(view_.to, view_.from, end_);
        view_.start = view_.from;
        view_.scale = width_ * 1000000.0 / static_cast<qreal>(std::max<qint64>(1, view_.to - view_.from));
    }
    view_.scale = boundedScale(view_.scale);
    const auto span = static_cast<long double>(width_) * 1000000.0L / view_.scale;
    const auto limit = RoundedTime(std::max(0.0L, static_cast<long double>(end_) - span), end_);
    view_.start = std::clamp<qint64>(view_.start, 0, limit);
}

qint64 EditViewportAdapter::endUs() const {
    return timeAtX(width_);
}

void EditViewportAdapter::setTimelineEndUs(qint64 value) {
    value = std::max<qint64>(0, value);
    if (end_ == value)
        return;
    end_ = value;
    normalize();
    emit viewChanged();
}

void EditViewportAdapter::setUsableWidth(qreal value) {
    if (!std::isfinite(value) || value < 1 || value > (std::numeric_limits<int>::max)() || width_ == value)
        return;
    width_ = value;
    normalize();
    emit viewChanged();
}

qint64 EditViewportAdapter::timeAtX(qreal x) const {
    return mapping().timeAtX(x);
}

qreal EditViewportAdapter::xAtTime(qint64 time) const {
    return mapping().xAtTime(time);
}

qint64 EditViewportMapping::timeAtX(qreal x) const {
    if (!std::isfinite(x))
        return origin_;
    return RoundedTime(static_cast<long double>(origin_) + static_cast<long double>(x) * 1000000.0L / scale_, end_);
}

qreal EditViewportMapping::xAtTime(qint64 time) const {
    const auto relative = static_cast<long double>(time) - static_cast<long double>(origin_);
    return static_cast<qreal>(relative * scale_ / 1000000.0L);
}

void EditViewportAdapter::fitInterval(qint64 from, qint64 to) {
    from = std::clamp<qint64>(from, 0, end_);
    to = std::clamp<qint64>(to, 0, end_);
    if (from >= to)
        return;
    const auto before = view_;
    view_.fit = Fit::Interval;
    view_.from = from;
    view_.to = to;
    normalize();
    publishView(before);
}

void EditViewportAdapter::setVisibleInterval(qint64 from, qint64 to) {
    fitInterval(from, to);
}

void EditViewportAdapter::fitOccupied(qint64 from, qint64 to) {
    fitInterval(from, to);
}

void EditViewportAdapter::fitSelection(qint64 from, qint64 to) {
    fitInterval(from, to);
}

void EditViewportAdapter::zoomAt(qreal scale, qreal anchor_x) {
    if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(anchor_x))
        return;
    anchor_x = std::clamp<qreal>(anchor_x, 0, width_);
    const auto anchor = timeAtX(anchor_x);
    const auto before = view_;
    view_.fit = Fit::Free;
    view_.scale = boundedScale(scale);
    view_.start = RoundedTime(
        static_cast<long double>(anchor) - static_cast<long double>(anchor_x) * 1000000.0L / view_.scale, end_);
    normalize();
    publishView(before);
}

void EditViewportAdapter::setPixelsPerSecond(qreal value) {
    zoomAt(value, 0);
}

void EditViewportAdapter::panPixels(qreal delta) {
    if (!std::isfinite(delta) || delta == 0)
        return;
    const auto before = view_;
    view_.fit = Fit::Free;
    view_.start = RoundedTime(
        static_cast<long double>(view_.start) + static_cast<long double>(delta) * 1000000.0L / view_.scale, end_);
    normalize();
    publishView(before);
}

void EditViewportAdapter::fitTimeline() {
    const auto before = view_;
    view_.fit = Fit::Timeline;
    normalize();
    publishView(before);
}

void EditViewportAdapter::publishView(const View& before) {
    if (view_.start == before.start && view_.scale == before.scale) {
        if (view_.fit == Fit::Free)
            view_ = before;
        return;
    }
    previous_ = before;
    emit viewChanged();
}

void EditViewportAdapter::restorePreviousView() {
    if (!previous_)
        return;
    std::swap(view_, *previous_);
    normalize();
    emit viewChanged();
}

} // namespace exosnap::quick
