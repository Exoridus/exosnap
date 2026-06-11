#include "RegionSelectionOverlay.h"

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>

#include "RegionGeometry.h"

#include <algorithm>

namespace exosnap::ui::widgets {

RegionSelectionOverlay::RegionSelectionOverlay(QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);

    confirm_button_ = new QPushButton(QStringLiteral("Confirm"), this);
    confirm_button_->setObjectName(QStringLiteral("regionOverlayConfirmButton"));
    confirm_button_->setProperty("role", QStringLiteral("primary"));
    confirm_button_->setVisible(false);
    confirm_button_->setCursor(Qt::PointingHandCursor);
    connect(confirm_button_, &QPushButton::clicked, this, &RegionSelectionOverlay::confirmSelection);

    cancel_button_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancel_button_->setObjectName(QStringLiteral("regionOverlayCancelButton"));
    cancel_button_->setProperty("role", QStringLiteral("ghost"));
    cancel_button_->setVisible(false);
    cancel_button_->setCursor(Qt::PointingHandCursor);
    connect(cancel_button_, &QPushButton::clicked, this, &RegionSelectionOverlay::cancelSelection);
}

void RegionSelectionOverlay::activateForSelection(QRect monitor_virtual_screen, QRect initial_region_virtual) {
    dragging_ = false;
    drag_start_ = {};
    drag_current_ = {};
    drag_last_ = {};
    drag_origin_rect_ = {};
    active_handle_ = RegionResizeHandle::None;
    setOverlayInteraction(InteractionMode::None);

    // Compute the union of all screen geometries (virtual desktop).
    QRect virtualRect;
    const auto screens = QGuiApplication::screens();
    for (const auto* screen : screens) {
        virtualRect = virtualRect.united(screen->geometry());
    }
    if (virtualRect.isEmpty()) {
        // Fallback: primary screen
        if (QGuiApplication::primaryScreen()) {
            virtualRect = QGuiApplication::primaryScreen()->geometry();
        }
    }
    if (virtualRect.isEmpty()) {
        cancelSelection();
        return;
    }

    if (!monitor_virtual_screen.isValid() || monitor_virtual_screen.isEmpty()) {
        monitor_virtual_screen = virtualRect;
    }

    setGeometry(virtualRect);
    monitor_rect_ = toLocal(monitor_virtual_screen).intersected(rect());
    if (!monitor_rect_.isValid() || monitor_rect_.width() < kMinDimension || monitor_rect_.height() < kMinDimension) {
        monitor_rect_ = rect();
    }

    initial_region_ = toLocal(initial_region_virtual).intersected(monitor_rect_);
    editing_existing_ = initial_region_.isValid() && initial_region_.width() >= kMinDimension &&
                        initial_region_.height() >= kMinDimension;
    selection_rect_ = editing_existing_ ? ClampRegionToMonitor(initial_region_, monitor_rect_, kMinDimension) : QRect();

    if (confirm_button_)
        confirm_button_->setVisible(selection_rect_.isValid());
    if (cancel_button_)
        cancel_button_->setVisible(true);
    updateActionGeometry();

    show();
    raise();
    activateWindow();
    setFocus();
    grabKeyboard();
    updateCursorForPosition(mapFromGlobal(QCursor::pos()));
    update();
}

QRect RegionSelectionOverlay::selectionRectLocal() const {
    return dragging_ && interaction_mode_ == InteractionMode::Selecting ? QRect(drag_start_, drag_current_).normalized()
                                                                        : selection_rect_;
}

QRect RegionSelectionOverlay::monitorRectLocal() const {
    return monitor_rect_.isValid() ? monitor_rect_ : rect();
}

QRect RegionSelectionOverlay::toLocal(const QRect& virtual_screen_rect) const {
    return virtual_screen_rect.translated(-geometry().topLeft());
}

QRect RegionSelectionOverlay::toVirtual(const QRect& local_rect) const {
    return local_rect.translated(geometry().topLeft());
}

RegionResizeHandle RegionSelectionOverlay::hitTestHandle(const QPoint& pos) const {
    if (!selection_rect_.isValid()) {
        return RegionResizeHandle::None;
    }
    constexpr int kHandleHit = 14;
    auto nearPoint = [pos](const QPoint& p) {
        return QRect(p.x() - kHandleHit / 2, p.y() - kHandleHit / 2, kHandleHit, kHandleHit).contains(pos);
    };
    if (nearPoint(selection_rect_.topLeft()))
        return RegionResizeHandle::TopLeft;
    if (nearPoint(QPoint(selection_rect_.right(), selection_rect_.top())))
        return RegionResizeHandle::TopRight;
    if (nearPoint(QPoint(selection_rect_.left(), selection_rect_.bottom())))
        return RegionResizeHandle::BottomLeft;
    if (nearPoint(selection_rect_.bottomRight()))
        return RegionResizeHandle::BottomRight;
    return RegionResizeHandle::None;
}

bool RegionSelectionOverlay::hitTestMove(const QPoint& pos) const {
    return selection_rect_.isValid() && selection_rect_.contains(pos);
}

void RegionSelectionOverlay::setOverlayInteraction(InteractionMode mode) {
    if (interaction_mode_ == mode) {
        return;
    }
    interaction_mode_ = mode;
    emit interactionModeChanged(mode);
}

void RegionSelectionOverlay::updateCursorForPosition(const QPoint& pos) {
    if (dragging_) {
        return;
    }
    switch (hitTestHandle(pos)) {
    case RegionResizeHandle::TopLeft:
    case RegionResizeHandle::BottomRight:
        setCursor(Qt::SizeFDiagCursor);
        return;
    case RegionResizeHandle::TopRight:
    case RegionResizeHandle::BottomLeft:
        setCursor(Qt::SizeBDiagCursor);
        return;
    case RegionResizeHandle::None:
        break;
    }
    setCursor(hitTestMove(pos) ? Qt::SizeAllCursor : Qt::CrossCursor);
}

void RegionSelectionOverlay::updateActionGeometry() {
    if (!confirm_button_ || !cancel_button_) {
        return;
    }

    constexpr int kGap = 8;
    confirm_button_->adjustSize();
    cancel_button_->adjustSize();
    const int total_width = confirm_button_->width() + cancel_button_->width() + kGap;
    QPoint pos;
    if (selection_rect_.isValid()) {
        pos = selection_rect_.bottomRight() + QPoint(10, 10);
        if (pos.x() + total_width > rect().right())
            pos.setX(selection_rect_.right() - total_width);
        if (pos.y() + confirm_button_->height() > rect().bottom())
            pos.setY(selection_rect_.top() - confirm_button_->height() - 10);
    } else {
        pos = monitorRectLocal().topLeft() + QPoint(16, 16);
    }
    pos.setX(std::clamp(pos.x(), rect().left() + 8, rect().right() - total_width - 8));
    pos.setY(std::clamp(pos.y(), rect().top() + 8, rect().bottom() - confirm_button_->height() - 8));
    cancel_button_->move(pos);
    confirm_button_->move(pos.x() + cancel_button_->width() + kGap, pos.y());
}

void RegionSelectionOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);

    // Semi-transparent dark overlay
    p.fillRect(rect(), QColor(0, 0, 0, 140));

    const QRect monitor = monitorRectLocal();
    p.setPen(QPen(QColor(255, 255, 255, 80), 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(monitor.adjusted(0, 0, -1, -1));

    if (selectionRectLocal().isValid()) {
        const QRect sel = selectionRectLocal();

        // Punch through the selection to show the content beneath
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.fillRect(sel, Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);

        // Selection border
        p.setPen(QPen(QColor(64, 160, 255), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(sel.adjusted(0, 0, -1, -1));

        // Corner handles
        const int hs = 6;
        p.fillRect(QRect(sel.left(), sel.top(), hs, hs), QColor(64, 160, 255));
        p.fillRect(QRect(sel.right() - hs + 1, sel.top(), hs, hs), QColor(64, 160, 255));
        p.fillRect(QRect(sel.left(), sel.bottom() - hs + 1, hs, hs), QColor(64, 160, 255));
        p.fillRect(QRect(sel.right() - hs + 1, sel.bottom() - hs + 1, hs, hs), QColor(64, 160, 255));

        // Dimensions label
        const QString sizeText = QString("%1 × %2").arg(sel.width()).arg(sel.height());
        QFont f = p.font();
        f.setPointSize(9);
        f.setBold(true);
        p.setFont(f);

        const QFontMetrics fm(f);
        const int tw = fm.horizontalAdvance(sizeText) + 10;
        const int th = fm.height() + 6;
        QPoint labelPos = sel.bottomRight() + QPoint(6, 6);
        // Clamp so the label stays on screen
        if (labelPos.x() + tw > rect().right())
            labelPos.setX(sel.right() - tw - 4);
        if (labelPos.y() + th > rect().bottom())
            labelPos.setY(sel.top() - th - 4);

        p.fillRect(QRect(labelPos, QSize(tw, th)), QColor(0, 0, 0, 180));
        p.setPen(Qt::white);
        p.drawText(QRect(labelPos, QSize(tw, th)), Qt::AlignCenter, sizeText);

        // Rejection hint when too small
        if (sel.width() < kMinDimension || sel.height() < kMinDimension) {
            p.setPen(QColor(255, 100, 100));
            QFont hf = p.font();
            hf.setBold(false);
            p.setFont(hf);
            p.drawText(sel.center() + QPoint(-80, 16), QStringLiteral("Too small — keep dragging"));
        }
    } else {
        // Instruction when not dragging
        p.setPen(Qt::white);
        QFont f = p.font();
        f.setPointSize(11);
        p.setFont(f);
        p.drawText(monitor, Qt::AlignCenter, QStringLiteral("Drag to select capture region\nEsc to cancel"));
    }
}

void RegionSelectionOverlay::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    if (confirm_button_ && confirm_button_->geometry().contains(event->pos()))
        return;
    if (cancel_button_ && cancel_button_->geometry().contains(event->pos()))
        return;

    const QRect monitor = monitorRectLocal();
    const QPoint pos(std::clamp(event->pos().x(), monitor.left(), monitor.right()),
                     std::clamp(event->pos().y(), monitor.top(), monitor.bottom()));
    active_handle_ = hitTestHandle(pos);
    drag_last_ = pos;
    drag_origin_rect_ = selection_rect_;
    dragging_ = true;

    if (active_handle_ != RegionResizeHandle::None) {
        switch (active_handle_) {
        case RegionResizeHandle::TopLeft:
            setOverlayInteraction(InteractionMode::ResizingTopLeft);
            break;
        case RegionResizeHandle::TopRight:
            setOverlayInteraction(InteractionMode::ResizingTopRight);
            break;
        case RegionResizeHandle::BottomLeft:
            setOverlayInteraction(InteractionMode::ResizingBottomLeft);
            break;
        case RegionResizeHandle::BottomRight:
            setOverlayInteraction(InteractionMode::ResizingBottomRight);
            break;
        case RegionResizeHandle::None:
            break;
        }
    } else if (hitTestMove(pos)) {
        setOverlayInteraction(InteractionMode::Moving);
    } else {
        drag_start_ = pos;
        drag_current_ = pos;
        selection_rect_ = {};
        setOverlayInteraction(InteractionMode::Selecting);
    }
    update();
}

void RegionSelectionOverlay::mouseMoveEvent(QMouseEvent* event) {
    const QRect monitor = monitorRectLocal();
    const QPoint pos(std::clamp(event->pos().x(), monitor.left(), monitor.right()),
                     std::clamp(event->pos().y(), monitor.top(), monitor.bottom()));

    if (!dragging_) {
        updateCursorForPosition(pos);
        return;
    }

    if (interaction_mode_ == InteractionMode::Moving) {
        selection_rect_ = MoveRegionWithinMonitor(drag_origin_rect_, pos - drag_last_, monitor);
    } else if (active_handle_ != RegionResizeHandle::None) {
        selection_rect_ = ResizeRegionFromCorner(drag_origin_rect_, active_handle_, pos, monitor, kMinDimension);
    } else {
        drag_current_ = pos;
        QRect candidate = QRect(drag_start_, drag_current_).normalized().intersected(monitor);
        selection_rect_ = candidate;
    }
    if (confirm_button_) {
        confirm_button_->setVisible(selection_rect_.isValid() && selection_rect_.width() >= kMinDimension &&
                                    selection_rect_.height() >= kMinDimension);
    }
    updateActionGeometry();
    update();
}

void RegionSelectionOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !dragging_)
        return;

    dragging_ = false;
    const QRect sel = selectionRectLocal();

    if (sel.width() < kMinDimension || sel.height() < kMinDimension) {
        if (editing_existing_ && initial_region_.isValid()) {
            selection_rect_ = initial_region_;
            setOverlayInteraction(InteractionMode::None);
            updateActionGeometry();
            update();
            return;
        }
        cancelSelection();
        return;
    }

    selection_rect_ = ClampRegionToMonitor(sel, monitorRectLocal(), kMinDimension);
    setOverlayInteraction(InteractionMode::None);
    if (confirm_button_)
        confirm_button_->setVisible(true);
    if (cancel_button_)
        cancel_button_->setVisible(true);
    updateActionGeometry();
    updateCursorForPosition(event->pos());
    update();
}

void RegionSelectionOverlay::confirmSelection() {
    if (!selection_rect_.isValid() || selection_rect_.width() < kMinDimension ||
        selection_rect_.height() < kMinDimension) {
        return;
    }
    const QRect virtualRect = toVirtual(selection_rect_);
    releaseMouse();
    releaseKeyboard();
    hide();
    setOverlayInteraction(InteractionMode::None);
    emit regionSelected(virtualRect);
}

void RegionSelectionOverlay::cancelSelection() {
    dragging_ = false;
    releaseMouse();
    releaseKeyboard();
    hide();
    setOverlayInteraction(InteractionMode::None);
    emit regionCancelled();
}

void RegionSelectionOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        cancelSelection();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        confirmSelection();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void RegionSelectionOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    updateActionGeometry();
}

} // namespace exosnap::ui::widgets
