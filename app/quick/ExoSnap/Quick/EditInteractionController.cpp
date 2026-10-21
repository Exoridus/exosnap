#include "EditInteractionController.h"
#include "EditTimelineModels.h"
#include <exosnap/engine/edit_types.h>

#include <QGuiApplication>
#include <QKeyEvent>
#include <QStyleHints>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

namespace exosnap::quick {

int EditInteractionController::clipPointerTarget(const QVariantMap& row, qreal width, qreal height, qreal header_height,
                                                 qreal x, qreal y, bool junction) const {
    const edit::ClipPointerGeometry geometry{width,
                                             height,
                                             header_height,
                                             !row.value(QStringLiteral("video")).toBool(),
                                             row.value(QStringLiteral("gainDb")).toDouble(),
                                             !row.value(QStringLiteral("fadeInOwned")).toBool(),
                                             !row.value(QStringLiteral("fadeOutOwned")).toBool(),
                                             junction};
    return static_cast<int>(edit::ResolveClipPointerTarget(geometry, x, y));
}

bool EditInteractionController::clipPointerThreshold(int target, qreal dx, qreal dy, qreal threshold) const {
    return edit::ClipPointerThresholdReached(static_cast<edit::ClipPointerTarget>(target), dx, dy, threshold);
}

qreal EditInteractionController::clipGainLineY(const QVariantMap& row, qreal height, qreal header_height) const {
    edit::ClipPointerGeometry geometry;
    geometry.height = height;
    geometry.header_height = header_height;
    geometry.gain_db = row.value(QStringLiteral("gainDb")).toDouble();
    return edit::ClipGainLineY(geometry);
}

QVariantMap EditInteractionController::clipPointerPolicy(int target, int modifiers, bool has_range) const {
    const bool control = modifiers == Qt::ControlModifier;
    const bool shift = modifiers == Qt::ShiftModifier;
    const bool alt = modifiers == Qt::AltModifier;
    const bool plain = modifiers == Qt::NoModifier;
    const bool header = target == PointerHeader;
    const bool content = target == PointerContent;
    const bool trim = target == PointerTrimStart || target == PointerTrimEnd;
    const bool fade = target == PointerFadeIn || target == PointerFadeOut;
    const bool gain = target == PointerGain;
    const bool valid = (header && (plain || control || shift || alt)) || (content && (plain || control || shift)) ||
                       (trim && (plain || alt)) || ((fade || gain) && (plain || control)) ||
                       (target == PointerJunction && plain);
    return {{QStringLiteral("valid"), valid},
            {QStringLiteral("toggle"), header && control},
            {QStringLiteral("extend"), shift},
            {QStringLiteral("insert"), header && control},
            {QStringLiteral("unlink"), (header || trim) && alt},
            {QStringLiteral("fine"), (fade || gain) && control},
            {QStringLiteral("range"), content && (shift || control)},
            {QStringLiteral("seek"), content && plain},
            {QStringLiteral("anchorAtPlayhead"), content && shift && !has_range},
            {QStringLiteral("preserveBounds"), content && control && has_range},
            {QStringLiteral("preserveSelection"), header},
            {QStringLiteral("error"),
             valid ? QString() : tr("This modifier combination is not available for this gesture.")}};
}

QString ResolveTimelineKey(int key, int modifiers, bool auto_repeat) {
    struct Binding {
        int key = 0;
        int modifiers = 0;
        const char* action = nullptr;
        bool repeat = false;
    };
    static constexpr Binding bindings[]{
        {Qt::Key_Escape, Qt::NoModifier, "cancel"},
        {Qt::Key_Right, Qt::NoModifier, "frame-next", true},
        {Qt::Key_Left, Qt::NoModifier, "frame-previous", true},
        {Qt::Key_Right, Qt::AltModifier, "sample-next", true},
        {Qt::Key_Left, Qt::AltModifier, "sample-previous", true},
        {Qt::Key_Right, Qt::ShiftModifier, "second-next", true},
        {Qt::Key_Left, Qt::ShiftModifier, "second-previous", true},
        {Qt::Key_Up, Qt::NoModifier, "boundary-previous", true},
        {Qt::Key_Down, Qt::NoModifier, "boundary-next", true},
        {Qt::Key_Up, Qt::AltModifier, "clip-previous", true},
        {Qt::Key_Down, Qt::AltModifier, "clip-next", true},
        {Qt::Key_Home, Qt::NoModifier, "timeline-start"},
        {Qt::Key_End, Qt::NoModifier, "timeline-end"},
        {Qt::Key_I, Qt::NoModifier, "range-in"},
        {Qt::Key_O, Qt::NoModifier, "range-out"},
        {Qt::Key_X, Qt::AltModifier, "clear-range"},
        {Qt::Key_L, Qt::ControlModifier, "loop"},
        {Qt::Key_A, Qt::ControlModifier, "select-all"},
        {Qt::Key_B, Qt::ControlModifier, "split"},
        {Qt::Key_S, Qt::NoModifier, "split"},
        {Qt::Key_Delete, Qt::NoModifier, "lift"},
        {Qt::Key_Delete, Qt::ShiftModifier, "ripple-remove"},
        {Qt::Key_Z, Qt::ControlModifier, "undo"},
        {Qt::Key_Y, Qt::ControlModifier, "redo"},
        {Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier, "redo"},
        {Qt::Key_C, Qt::ControlModifier, "copy"},
        {Qt::Key_X, Qt::ControlModifier, "cut"},
        {Qt::Key_V, Qt::ControlModifier, "paste"},
        {Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier, "insert-paste"},
        {Qt::Key_Comma, Qt::NoModifier, "nudge-previous", true},
        {Qt::Key_Period, Qt::NoModifier, "nudge-next", true},
        {Qt::Key_Comma, Qt::AltModifier, "sample-nudge-previous", true},
        {Qt::Key_Period, Qt::AltModifier, "sample-nudge-next", true},
        {Qt::Key_Comma, Qt::ControlModifier, "trim-start", true},
        {Qt::Key_Period, Qt::ControlModifier, "trim-end", true},
        {Qt::Key_Comma, Qt::ControlModifier | Qt::ShiftModifier, "extend-start", true},
        {Qt::Key_Period, Qt::ControlModifier | Qt::ShiftModifier, "extend-end", true},
        {Qt::Key_Plus, Qt::ControlModifier, "zoom-in", true},
        {Qt::Key_Equal, Qt::ControlModifier, "zoom-in", true},
        {Qt::Key_Minus, Qt::ControlModifier, "zoom-out", true},
        {Qt::Key_Z, Qt::ShiftModifier, "fit-timeline"},
        {Qt::Key_Z, Qt::NoModifier, "fit-selection"},
        {Qt::Key_Backspace, Qt::NoModifier, "previous-view"},
        {Qt::Key_M, Qt::NoModifier, "mute-track"},
        {Qt::Key_M, Qt::ControlModifier, "media-dock"},
        {Qt::Key_D, Qt::ControlModifier, "inspector-dock"},
        {Qt::Key_E, Qt::ControlModifier, "export"},
        {Qt::Key_Menu, Qt::NoModifier, "context-menu"},
        {Qt::Key_F10, Qt::ShiftModifier, "context-menu"},
        {Qt::Key_Question, Qt::NoModifier, "shortcuts"},
        {Qt::Key_Question, Qt::ShiftModifier, "shortcuts"},
    };
    for (const auto& binding : bindings)
        if (key == binding.key && modifiers == binding.modifiers && (!auto_repeat || binding.repeat))
            return QString::fromLatin1(binding.action);
    return {};
}

QString EditInteractionController::keyAction(int key, int modifiers, bool auto_repeat) const {
    return eligible() ? ResolveTimelineKey(key, modifiers, auto_repeat) : QString{};
}

QVariantMap ResolveTimelineWheel(int modifiers, QPointF pixels, QPointF angles, bool space_reserved) {
    QVariantMap action{{"pan", 0.0}, {"scroll", 0.0}, {"zoom", 0.0}, {"height", 0.0}};
    if (!std::isfinite(pixels.x()) || !std::isfinite(pixels.y()) || !std::isfinite(angles.x()) ||
        !std::isfinite(angles.y()))
        return action;
    if (modifiers != Qt::NoModifier && modifiers != Qt::ShiftModifier && modifiers != Qt::ControlModifier &&
        modifiers != Qt::AltModifier)
        return action;
    const bool pixel_input = !pixels.isNull();
    const auto delta = pixel_input ? pixels : angles;
    const qreal scalar = delta.y() != 0 ? delta.y() : delta.x();
    const qreal pan_unit = pixel_input ? 1.0 : 1.0 / 3;
    if (space_reserved || modifiers == Qt::ShiftModifier) {
        action["pan"] = -scalar * pan_unit;
    } else if (modifiers == Qt::ControlModifier) {
        action["zoom"] = scalar / 120;
    } else if (modifiers == Qt::AltModifier) {
        action["height"] = scalar / 120 * 8;
    } else {
        action["pan"] = -delta.x() * pan_unit;
        action["scroll"] = -delta.y() * pan_unit;
    }
    return action;
}

QVariantMap EditInteractionController::handleWheel(int modifiers, QPointF pixels, QPointF angles, int phase) {
    const bool reserved = held_ && modifiers == Qt::NoModifier && eligible();
    if (reserved) {
        consumed_ = true;
        emit gestureChanged();
    } else if (held_) {
        cancel();
    }
    auto action = ResolveTimelineWheel(modifiers, pixels, angles, reserved);
    action.insert(QStringLiteral("finished"), phase == Qt::ScrollEnd);
    return action;
}

EditInteractionController::EditInteractionController(QObject* parent) : QObject(parent) {
}

qulonglong EditInteractionController::dragTrackId(const QVariantList& rows, qulonglong previous_id,
                                                  qreal pointer_y) const {
    std::vector<edit::TrackRowBounds> bounds;
    std::vector<qulonglong> ids;
    std::unordered_set<qulonglong> seen;
    int previous = -1;
    for (const auto& value : rows) {
        const auto row = value.toMap();
        bool id_valid = false, top_valid = false, height_valid = false;
        const auto id = row.value(QStringLiteral("id")).toULongLong(&id_valid);
        const auto top = row.value(QStringLiteral("top")).toDouble(&top_valid);
        const auto height = row.value(QStringLiteral("height")).toDouble(&height_valid);
        if (!id_valid || !id || !top_valid || !height_valid || !seen.insert(id).second)
            return 0;
        if (id == previous_id)
            previous = static_cast<int>(ids.size());
        ids.push_back(id);
        bounds.push_back({top, top + height});
    }
    const auto index = edit::ResolveDragTrack(previous, pointer_y, bounds);
    return index >= 0 ? ids[static_cast<std::size_t>(index)] : 0;
}

QPointF EditInteractionController::edgeAutoScroll(QPointF pointer, QSizeF extent) const {
    return {edit::EdgeAutoScrollVelocity(pointer.x(), extent.width()),
            edit::EdgeAutoScrollVelocity(pointer.y(), extent.height())};
}

QVariantList EditInteractionController::fadeEnvelopePoints(int kind, qreal bias, bool outgoing, qreal from,
                                                           qreal to) const {
    if (!std::isfinite(from) || !std::isfinite(to) || from < 0 || to > 1 || from >= to)
        return {};
    const engine::CurveSpec curve{static_cast<engine::CurveKind>(kind), bias};
    QVariantList points;
    points.reserve(33);
    for (int index = 0; index <= 32; ++index) {
        const auto progress = from + (to - from) * index / 32;
        const auto value = engine::EvaluateCurve(curve, outgoing ? 1 - progress : progress);
        if (!value)
            return {};
        points.push_back(*value);
    }
    return points;
}

qulonglong EditInteractionController::junctionAt(const QVariantList& rows, qulonglong clip_id, qint64 at) const {
    qulonglong selected = 0;
    qulonglong selected_junction = (std::numeric_limits<qulonglong>::max)();
    for (const auto& value : rows) {
        const auto row = value.toMap();
        const auto outgoing = row.value(QStringLiteral("outgoing")).toULongLong();
        const auto incoming = row.value(QStringLiteral("incoming")).toULongLong();
        const auto id = row.value(QStringLiteral("id")).toULongLong();
        const auto start = row.value(QStringLiteral("overlapStartUs")).toLongLong();
        const auto end = row.value(QStringLiteral("overlapEndUs")).toLongLong();
        if (id && outgoing && incoming && (outgoing == clip_id || incoming == clip_id) && start >= 0 && end > start &&
            at >= start && at < end && id < selected_junction) {
            selected = outgoing;
            selected_junction = id;
        }
    }
    return selected;
}

qulonglong EditInteractionController::modelJunctionAt(EditTimelineClipModel* model, qulonglong clip_id,
                                                      qint64 at) const {
    QVariantList rows;
    if (model)
        for (int index = 0; index < model->rowCount(); ++index)
            rows.push_back(model->data(model->index(index), EditTimelineClipModel::RowRole));
    return junctionAt(rows, clip_id, at);
}

int EditInteractionController::junctionHandleAt(qreal x, qreal start, qreal end, int last, qreal hit) const {
    const auto target = edit::ResolveRangePointerTarget(
        x, start, end, hit, true, last == 1 ? edit::RangePointerTarget::End : edit::RangePointerTarget::Start);
    return target == edit::RangePointerTarget::Start ? 0 : target == edit::RangePointerTarget::End ? 1 : -1;
}

bool EditInteractionController::junctionPointerEligible(const QVariantMap& row, qreal scale, qint64 at, qreal y,
                                                        qreal height, qreal header) const {
    const auto a_start = row.value(QStringLiteral("outgoingStartUs")).toLongLong();
    const auto a_end = row.value(QStringLiteral("outgoingEndUs")).toLongLong();
    const auto b_start = row.value(QStringLiteral("incomingStartUs")).toLongLong();
    const auto b_end = row.value(QStringLiteral("incomingEndUs")).toLongLong();
    if (!std::isfinite(scale) || scale <= 0 || a_start < 0 || b_start <= a_start || a_end <= b_start ||
        b_end <= a_end || at < b_start || at >= a_end)
        return false;
    const bool audio = !row.value(QStringLiteral("video")).toBool();
    const edit::ClipPointerGeometry outgoing{(a_end - a_start) * scale, height, header, audio, 0, true, false, true};
    const edit::ClipPointerGeometry incoming{(b_end - b_start) * scale, height, header, audio, 0, false, true, true};
    return edit::ResolveClipPointerTarget(outgoing, (at - a_start) * scale, y) == edit::ClipPointerTarget::Junction &&
           edit::ResolveClipPointerTarget(incoming, (at - b_start) * scale, y) == edit::ClipPointerTarget::Junction;
}

qulonglong EditInteractionController::resolveNarrowClip(EditTimelineClipModel* model, qulonglong track, qint64 at,
                                                        qreal scale) {
    if (!model || !track || at < 0 || !std::isfinite(scale) || scale <= 0) {
        narrow_candidates_.clear();
        narrow_last_ = narrow_track_ = 0;
        narrow_time_ = 0;
        narrow_scale_ = 0;
        return 0;
    }
    QList<qulonglong> candidates;
    qreal best = 4;
    for (int index = 0; index < model->rowCount(); ++index) {
        const auto row = model->data(model->index(index), EditTimelineClipModel::RowRole).toMap();
        const auto id = row.value(QStringLiteral("id")).toULongLong();
        const auto start = row.value(QStringLiteral("startUs")).toLongLong();
        const auto end = row.value(QStringLiteral("endUs")).toLongLong();
        if (!id || row.value(QStringLiteral("trackId")).toULongLong() != track || start < 0 || end <= start)
            continue;
        const auto distance = static_cast<qreal>(at < start ? start - at : at > end ? at - end : 0) * scale;
        if (!std::isfinite(distance) || distance > 4)
            continue;
        if (distance < best - 1e-9) {
            best = distance;
            candidates.clear();
        }
        if (std::abs(distance - best) <= 1e-9)
            candidates.push_back(id);
    }
    std::sort(candidates.begin(), candidates.end());
    const auto displacement = static_cast<qreal>(at >= narrow_time_ ? at - narrow_time_ : narrow_time_ - at) * scale;
    const bool repeat = !candidates.isEmpty() && candidates == narrow_candidates_ && track == narrow_track_ &&
                        scale == narrow_scale_ && displacement <= 0.5;
    const auto previous = repeat ? candidates.indexOf(narrow_last_) : -1;
    narrow_last_ = candidates.isEmpty() ? 0 : candidates[(previous + 1) % candidates.size()];
    narrow_candidates_ = std::move(candidates);
    narrow_track_ = track;
    narrow_time_ = at;
    narrow_scale_ = scale;
    return narrow_last_;
}

EditInteractionController::~EditInteractionController() {
    if (scope_)
        disconnect(scope_, nullptr, this, nullptr);
    if (window_) {
        window_->removeEventFilter(this);
        disconnect(window_, nullptr, this, nullptr);
    }
}

void EditInteractionController::setScopeItem(QQuickItem* item) {
    if (scope_ == item)
        return;
    cancel();
    if (scope_)
        disconnect(scope_, nullptr, this, nullptr);
    scope_ = item;
    if (scope_) {
        connect(scope_, &QQuickItem::windowChanged, this, &EditInteractionController::bindWindow);
        connect(scope_, &QQuickItem::visibleChanged, this, [this] {
            if (!eligible())
                cancel();
        });
        connect(scope_, &QQuickItem::enabledChanged, this, [this] {
            if (!eligible())
                cancel();
        });
        connect(scope_, &QObject::destroyed, this, &EditInteractionController::bindWindow);
    }
    bindWindow();
    emit scopeItemChanged();
}

void EditInteractionController::bindWindow() {
    cancel();
    if (window_) {
        window_->removeEventFilter(this);
        disconnect(window_, nullptr, this, nullptr);
    }
    window_ = scope_ ? scope_->window() : nullptr;
    if (window_) {
        window_->installEventFilter(this);
        connect(window_, &QQuickWindow::activeFocusItemChanged, this, &EditInteractionController::cancel);
    }
}

bool EditInteractionController::eligible() const {
    if (blocked_ || !scope_ || !window_ || !scope_->isVisible() || !scope_->isEnabled())
        return false;
    const auto* focus = window_->activeFocusItem();
    return focus && !focus->flags().testFlag(QQuickItem::ItemAcceptsInputMethod) && !focus->inherits("QQuickControl") &&
           focus->metaObject()->indexOfSignal("clicked()") < 0 && (focus == scope_ || scope_->isAncestorOf(focus));
}

void EditInteractionController::setBlocked(bool value) {
    if (blocked_ == value)
        return;
    blocked_ = value;
    if (value)
        cancel();
    emit blockedChanged();
}

bool EditInteractionController::armSpace(int modifiers, bool auto_repeat) {
    if (modifiers == Qt::ShiftModifier) {
        if (!auto_repeat && eligible() && QGuiApplication::mouseButtons() == Qt::NoButton) {
            cancel();
            emit rangeOnceRequested();
            return true;
        }
        return false;
    }
    if (auto_repeat)
        return held_;
    if (modifiers != Qt::NoModifier || !eligible() || QGuiApplication::mouseButtons() != Qt::NoButton)
        return false;
    if (!held_) {
        held_ = true;
        consumed_ = false;
        emit gestureChanged();
    }
    return true;
}

void EditInteractionController::beginPan(QPointF position, bool emit_pixels) {
    if (!held_ || !eligible() || !std::isfinite(position.x()) || !std::isfinite(position.y()))
        return;
    consumed_ = true;
    captured_ = true;
    panning_ = false;
    emit_pan_pixels_ = emit_pixels;
    press_ = previous_ = position;
    emit gestureChanged();
}

void EditInteractionController::updatePan(QPointF position) {
    if (!held_ || !captured_ || !std::isfinite(position.x()) || !std::isfinite(position.y()))
        return;
    if (!panning_ && std::hypot(position.x() - press_.x(), position.y() - press_.y()) <
                         QGuiApplication::styleHints()->startDragDistance())
        return;
    if (!panning_) {
        panning_ = true;
        emit gestureChanged();
    }
    const auto delta = previous_.x() - position.x();
    // Advance even at a viewport bound so reversing direction responds immediately.
    previous_ = position;
    if (emit_pan_pixels_)
        emit panRequested(delta);
}

void EditInteractionController::endPan() {
    captured_ = false;
    panning_ = false;
    emit gestureChanged();
}

void EditInteractionController::cancel() {
    if (!held_)
        return;
    held_ = false;
    consumed_ = true;
    // Keep the pointer grab until release so cancellation cannot leak a clip click.
    emit gestureChanged();
}

bool EditInteractionController::eventFilter(QObject* watched, QEvent* event) {
    if (watched != window_ || !held_)
        return false;
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)
        consumed_ = true;
    else if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide)
        cancel();
    else if (event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() != Qt::Key_Space) {
            cancel();
            return key->key() == Qt::Key_Escape;
        }
    } else if (event->type() == QEvent::KeyRelease) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Space && !key->isAutoRepeat()) {
            const bool toggle = !consumed_ && eligible();
            held_ = false;
            emit gestureChanged();
            if (toggle)
                emit togglePlayRequested();
            return true;
        }
    }
    return false;
}

} // namespace exosnap::quick
