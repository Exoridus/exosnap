#include "InfoHintIcon.h"

#include <QCursor>
#include <QEnterEvent>
#include <QFocusEvent>
#include <QRect>
#include <QSize>
#include <QTimer>
#include <QToolTip>

#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"

namespace exosnap::ui::widgets {

namespace {
constexpr int kIconSize = 15;             // logical px — matches the design's 15×15 info glyph
constexpr int kHoverPollIntervalMs = 120; // matches CompareHint::hover_timer_ / TransportDock's chevron poll cadence
constexpr int kHysteresisMarginPx = 6;    // px the cursor may sit outside the icon before the tooltip closes
} // namespace

InfoHintIcon::InfoHintIcon(const QString& hint_text, QWidget* parent) : QToolButton(parent), hint_text_(hint_text) {
    // Flat, no border, no background — styled via QSS role like the AdvancedPage infoGlyph.
    setProperty("labelRole", "infoGlyph");
    setAutoRaise(true);
    setFocusPolicy(Qt::TabFocus);

    // Accessible: the button announces itself as "More information: <hint>" for
    // screen readers and satisfies the keyboard-reachable requirement.
    setAccessibleName(QStringLiteral("More information: ") + hint_text_);

    // setToolTip() still carries the string for tooling that reads the property
    // directly (e.g. accessibility inspectors); actual display is driven explicitly
    // by enterEvent/focusInEvent below rather than Qt's automatic per-motion
    // QEvent::ToolTip dispatch — see the class comment for why.
    setToolTip(hint_text_);

    // Fixed size: 18×18 px touch target (design: 18px container, 15px glyph).
    setFixedSize(18, 18);
    setIconSize(QSize(kIconSize, kIconSize));

    updateIcon(false);

    // Polls the cursor while the tooltip is open instead of reacting to the bare
    // Enter/Leave pair: near the icon's edge, cursor jitter of a couple of physical
    // pixels crosses the widget boundary repeatedly, and a leave-driven hide would
    // flicker the tooltip open/closed on every crossing. The timer only closes it
    // once the cursor has settled truly outside a small hysteresis band around the
    // icon (mirrors TransportDock::openChevronMenu's chevron_leave_timer_ /
    // CompareHint's hover_timer_). Keyboard focus keeps the tooltip open regardless
    // of where the cursor is.
    hover_timer_ = new QTimer(this);
    hover_timer_->setSingleShot(false);
    hover_timer_->setInterval(kHoverPollIntervalMs);
    connect(hover_timer_, &QTimer::timeout, this, [this]() {
        if (hasFocus())
            return;
        const QRect band =
            QRect(mapToGlobal(QPoint(0, 0)), size())
                .adjusted(-kHysteresisMarginPx, -kHysteresisMarginPx, kHysteresisMarginPx, kHysteresisMarginPx);
        if (band.contains(QCursor::pos()))
            return;
        hovered_ = false;
        updateIcon(false);
        hideHintTooltip();
    });
}

const QString& InfoHintIcon::hintText() const {
    return hint_text_;
}

void InfoHintIcon::enterEvent(QEnterEvent* event) {
    QToolButton::enterEvent(event);
    hovered_ = true;
    updateIcon(true);
    // Anchor on the real OS cursor position rather than a widget-derived point
    // (mapToGlobal(rect().center())): QCursor::pos() is always correct regardless
    // of screen, whereas mapToGlobal() can be computed against a stale per-window
    // DPI/screen association right after the window is dragged to a differently
    // scaled monitor — the previous explicit call here used mapToGlobal and, on a
    // multi-monitor / mixed-DPI desktop, could resolve to the wrong screen and pop
    // the tooltip up on the edge of the primary display instead of under the icon.
    QToolTip::showText(QCursor::pos(), hint_text_, this);
    hover_timer_->start();
}

void InfoHintIcon::leaveEvent(QEvent* event) {
    QToolButton::leaveEvent(event);
    hovered_ = false;
    updateIcon(false);
    // No hide here: the poll timer (running while the tooltip is open) closes it
    // only once the cursor has truly left the hysteresis band — a bare leave-driven
    // hide is exactly what flickered the tooltip at the icon's edge.
}

void InfoHintIcon::focusInEvent(QFocusEvent* event) {
    QToolButton::focusInEvent(event);
    updateIcon(true);
    QToolTip::showText(mapToGlobal(rect().center()), hint_text_, this);
    hover_timer_->start();
}

void InfoHintIcon::focusOutEvent(QFocusEvent* event) {
    QToolButton::focusOutEvent(event);
    updateIcon(false);
    if (!hovered_)
        hideHintTooltip();
}

void InfoHintIcon::hideHintTooltip() {
    if (hover_timer_)
        hover_timer_->stop();
    QToolTip::hideText();
}

void InfoHintIcon::updateIcon(bool highlighted) {
    const qreal dpr = devicePixelRatioF();
    const auto& t = ui::theme::ActiveTheme();
    const QString color = highlighted ? QString::fromUtf8(t.ac) : QString::fromUtf8(t.dim);
    setIcon(ui::theme::lucideIcon(QStringLiteral("info"), color, kIconSize, dpr));
}

} // namespace exosnap::ui::widgets
