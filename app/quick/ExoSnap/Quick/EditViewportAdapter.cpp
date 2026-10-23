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

EditViewportAdapter::EditViewportAdapter(QObject* parent) : QObject(parent), wheel_timer_(this) {
    wheel_timer_.setSingleShot(true);
    wheel_timer_.setInterval(300);
    connect(&wheel_timer_, &QTimer::timeout, this, &EditViewportAdapter::endViewOperation);
}

qint64 EditViewportAdapter::navigationEndUs() const {
    if (!end_)
        return 10'000'000;
    const auto tail = std::max<qint64>(2'000'000, end_ / 50 + (end_ % 50 != 0));
    return end_ > (std::numeric_limits<qint64>::max)() - tail ? (std::numeric_limits<qint64>::max)() : end_ + tail;
}

qreal EditViewportAdapter::minimumScale() const {
    return width_ * 1000000.0 / static_cast<qreal>(navigationEndUs());
}

qreal EditViewportAdapter::maximumScale() const {
    return std::max(minimumScale(), kMaximumScale);
}

qreal EditViewportAdapter::boundedScale(qreal scale) const {
    return std::clamp(scale, minimumScale(), maximumScale());
}

void EditViewportAdapter::normalize() {
    if (!initialized_) {
        view_.start = 0;
        view_.scale = minimumScale();
    }
    view_.scale = std::min(view_.scale, maximumScale());
    const auto span = static_cast<long double>(width_) * 1000000.0L / view_.scale;
    const auto extent = navigationEndUs();
    const auto limit = RoundedTime(std::max(0.0L, static_cast<long double>(extent) - span), extent);
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
    const auto span = static_cast<long double>(width_) * 1000000.0L / view_.scale;
    width_ = value;
    if (initialized_)
        view_.scale = static_cast<qreal>(static_cast<long double>(width_) * 1000000.0L / span);
    normalize();
    emit viewChanged();
}

void EditViewportAdapter::setFitPaddingUs(qint64 value) {
    if (value <= 0 || fit_padding_ == value)
        return;
    fit_padding_ = value;
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

void EditViewportAdapter::fitInterval(qint64 from, qint64 to, bool padding) {
    const auto extent = navigationEndUs();
    from = std::clamp<qint64>(from, 0, extent);
    to = std::clamp<qint64>(to, 0, extent);
    if (from > to || (!padding && from == to))
        return;
    if (padding) {
        const auto span = to - from;
        const auto margin = span == 0 ? std::max<qint64>(1'000'000, fit_padding_)
                                      : std::max(fit_padding_, span / 50 + (span % 50 != 0));
        from = from < margin ? 0 : from - margin;
        to = extent - to < margin ? extent : to + margin;
    }
    if (from == to)
        return;
    finishWheelOperation();
    const auto before = view_;
    initialized_ = true;
    view_.start = from;
    view_.scale = boundedScale(width_ * 1000000.0 / static_cast<qreal>(to - from));
    normalize();
    publishView(before, true);
}

void EditViewportAdapter::setVisibleInterval(qint64 from, qint64 to) {
    fitInterval(from, to, false);
}

void EditViewportAdapter::fitOccupied(qint64 from, qint64 to) {
    fitInterval(from, to, true);
}

void EditViewportAdapter::fitSelection(qint64 from, qint64 to) {
    fitInterval(from, to, true);
}

void EditViewportAdapter::zoomAt(qreal scale, qreal anchor_x) {
    if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(anchor_x))
        return;
    anchor_x = std::clamp<qreal>(anchor_x, 0, width_);
    const auto anchor = timeAtX(anchor_x);
    const auto before = view_;
    initialized_ = true;
    view_.scale = boundedScale(scale);
    view_.start =
        RoundedTime(static_cast<long double>(anchor) - static_cast<long double>(anchor_x) * 1000000.0L / view_.scale,
                    navigationEndUs());
    normalize();
    publishView(before, true);
}

void EditViewportAdapter::zoomWheelAt(qreal scale, qreal anchor_x) {
    if (!operation_before_) {
        operation_before_ = interval(view_);
        wheel_operation_ = true;
    }
    zoomAt(scale, anchor_x);
    if (wheel_operation_)
        wheel_timer_.start();
}

void EditViewportAdapter::beginViewOperation() {
    finishWheelOperation();
    if (!operation_before_)
        operation_before_ = interval(view_);
}

void EditViewportAdapter::endViewOperation() {
    wheel_timer_.stop();
    if (!operation_before_)
        return;
    const auto before = *operation_before_;
    const auto zoomed = operation_zoomed_;
    operation_before_.reset();
    wheel_operation_ = false;
    operation_zoomed_ = false;
    if (zoomed && before != interval(view_)) {
        pushHistory(before);
        emit viewChanged();
    }
}

void EditViewportAdapter::finishWheelOperation() {
    if (wheel_operation_)
        endViewOperation();
}

void EditViewportAdapter::setPixelsPerSecond(qreal value) {
    zoomAt(value, 0);
}

void EditViewportAdapter::panPixels(qreal delta) {
    if (!std::isfinite(delta) || delta == 0)
        return;
    const auto before = view_;
    initialized_ = true;
    view_.start =
        RoundedTime(static_cast<long double>(view_.start) + static_cast<long double>(delta) * 1000000.0L / view_.scale,
                    navigationEndUs());
    normalize();
    publishView(before, false);
}

void EditViewportAdapter::fitTimeline() {
    finishWheelOperation();
    const auto before = view_;
    initialized_ = true;
    view_.start = 0;
    view_.scale = minimumScale();
    normalize();
    publishView(before, true);
}

EditViewportAdapter::Interval EditViewportAdapter::interval(const View& view) const {
    return {view.start, RoundedTime(static_cast<long double>(view.start) +
                                        static_cast<long double>(width_) * 1000000.0L / view.scale,
                                    navigationEndUs())};
}

void EditViewportAdapter::pushHistory(Interval before) {
    if (history_.size() == 20)
        history_.erase(history_.begin());
    history_.push_back(before);
}

void EditViewportAdapter::publishView(const View& before, bool history) {
    if (view_.start == before.start && view_.scale == before.scale)
        return;
    if (history && !operation_before_)
        pushHistory(interval(before));
    if (history && operation_before_)
        operation_zoomed_ = true;
    emit viewChanged();
}

void EditViewportAdapter::restorePreviousView() {
    endViewOperation();
    if (history_.empty())
        return;
    const auto previous = history_.back();
    history_.pop_back();
    const auto extent = navigationEndUs();
    const auto span = std::clamp<qint64>(previous.to - previous.from, 1, extent);
    initialized_ = true;
    view_.start = std::clamp<qint64>(previous.from, 0, extent - span);
    view_.scale = boundedScale(width_ * 1000000.0 / static_cast<qreal>(span));
    normalize();
    emit viewChanged();
}

bool EditViewportAdapter::beginOverviewOperation(qreal x, qreal width, qreal edge_hit, bool body_only) {
    if (!std::isfinite(x) || !std::isfinite(width) || width <= 0 || !std::isfinite(edge_hit) || edge_hit < 0)
        return false;
    endOverviewOperation();
    beginViewOperation();
    const auto extent = static_cast<long double>(navigationEndUs());
    const auto left = static_cast<qreal>(view_.start / extent * width);
    const auto right = static_cast<qreal>(endUs() / extent * width);
    const auto left_distance = std::abs(x - left);
    const auto right_distance = std::abs(x - right);
    overview_target_ = 1;
    if (!body_only && (left_distance <= edge_hit || right_distance <= edge_hit)) {
        overview_target_ = left_distance == right_distance  ? overview_last_edge_
                           : left_distance < right_distance ? 2
                                                            : 3;
        overview_last_edge_ = overview_target_;
    } else if (!body_only && (x < left || x > right)) {
        centerOverviewAt(x, width);
    }
    overview_before_ = interval(view_);
    overview_scale_ = view_.scale;
    overview_press_fraction_ = x / width;
    emit viewChanged();
    return true;
}

void EditViewportAdapter::updateOverviewOperation(qreal x, qreal width) {
    if (!overview_target_ || !std::isfinite(x) || !std::isfinite(width) || width <= 0)
        return;
    const auto extent = navigationEndUs();
    const auto delta = (static_cast<long double>(x) / width - overview_press_fraction_) * extent;
    const auto before = view_;
    initialized_ = true;
    if (overview_target_ == 1) {
        view_.start = RoundedTime(overview_before_.from + delta, extent);
        view_.scale = overview_scale_;
    } else {
        const auto minimum_span =
            std::max<qint64>(1, static_cast<qint64>(std::ceil(width_ * 1000000.0 / maximumScale())));
        auto from = overview_before_.from;
        auto to = overview_before_.to;
        if (overview_target_ == 2)
            from = std::clamp<qint64>(RoundedTime(from + delta, extent), 0, std::max<qint64>(0, to - minimum_span));
        else
            to = std::clamp<qint64>(RoundedTime(to + delta, extent),
                                    extent - from < minimum_span ? extent : from + minimum_span, extent);
        if (to <= from)
            return;
        view_.start = from;
        view_.scale = boundedScale(width_ * 1000000.0 / static_cast<qreal>(to - from));
    }
    normalize();
    publishView(before, overview_target_ != 1);
}

void EditViewportAdapter::endOverviewOperation() {
    if (!overview_target_)
        return;
    overview_target_ = 0;
    endViewOperation();
    emit viewChanged();
}

void EditViewportAdapter::centerOverviewAt(qreal x, qreal width) {
    if (!std::isfinite(x) || !std::isfinite(width) || width <= 0)
        return;
    const auto before = view_;
    const auto center = static_cast<long double>(x) / width * navigationEndUs();
    const auto half_span = static_cast<long double>(width_) * 500000.0L / view_.scale;
    initialized_ = true;
    view_.start = RoundedTime(center - half_span, navigationEndUs());
    normalize();
    publishView(before, false);
}

} // namespace exosnap::quick
