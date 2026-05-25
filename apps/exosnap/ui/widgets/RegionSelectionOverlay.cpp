#include "RegionSelectionOverlay.h"

#include <QApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QScreen>
#include <QShowEvent>

namespace exosnap::ui::widgets {

RegionSelectionOverlay::RegionSelectionOverlay(QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);
}

void RegionSelectionOverlay::activateForSelection() {
    dragging_ = false;
    drag_start_ = {};
    drag_current_ = {};

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
    setGeometry(virtualRect);
    show();
    raise();
    activateWindow();
    setFocus();
    grabKeyboard();
}

QRect RegionSelectionOverlay::selectionRectLocal() const {
    return QRect(drag_start_, drag_current_).normalized();
}

void RegionSelectionOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
}

void RegionSelectionOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);

    // Semi-transparent dark overlay
    p.fillRect(rect(), QColor(0, 0, 0, 140));

    if (dragging_) {
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
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("Drag to select capture region\nEsc to cancel"));
    }
}

void RegionSelectionOverlay::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        drag_start_ = event->pos();
        drag_current_ = event->pos();
        dragging_ = true;
        update();
    }
}

void RegionSelectionOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_) {
        drag_current_ = event->pos();
        update();
    }
}

void RegionSelectionOverlay::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !dragging_)
        return;

    dragging_ = false;
    const QRect sel = selectionRectLocal();

    if (sel.width() < kMinDimension || sel.height() < kMinDimension) {
        // Too small: reset and let user try again
        drag_start_ = {};
        drag_current_ = {};
        update();
        return;
    }

    // Convert widget-local coords to virtual screen coords
    const QRect virtualRect = sel.translated(geometry().topLeft());
    releaseKeyboard();
    hide();
    emit regionSelected(virtualRect);
}

void RegionSelectionOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        dragging_ = false;
        releaseKeyboard();
        hide();
        emit regionCancelled();
    }
}

} // namespace exosnap::ui::widgets
