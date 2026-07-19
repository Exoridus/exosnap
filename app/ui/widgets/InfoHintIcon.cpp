#include "InfoHintIcon.h"

#include <algorithm>

#include <QCursor>
#include <QEnterEvent>
#include <QFocusEvent>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QToolTip>

#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"

namespace exosnap::ui::widgets {

namespace {
constexpr int kIconSize = 15;             // logical px — matches the design's 15×15 info glyph
constexpr int kHoverPollIntervalMs = 120; // matches CompareHint::hover_timer_ / TransportDock's chevron poll cadence
constexpr int kHysteresisMarginPx = 6;    // px the cursor may sit outside the icon before the tooltip closes
constexpr int kTooltipGapPx = 4;          // px gap between the icon's bottom edge and the tooltip's top

// The hint may be rich text (bold lead line + <br> breaks, see SettingsHintText.h).
// Screen readers must not hear markup, so the accessible name uses a flattened
// plain-text form: <br> becomes a line break, tags are dropped.
QString plainForAccessibility(const QString& text) {
    if (!Qt::mightBeRichText(text))
        return text;
    return QTextDocumentFragment::fromHtml(text).toPlainText();
}
} // namespace

InfoHintIcon::InfoHintIcon(const QString& hint_text, QWidget* parent) : QToolButton(parent), hint_text_(hint_text) {
    // Flat, no border, no background — styled via QSS role like the AdvancedPage infoGlyph.
    setProperty("labelRole", "infoGlyph");
    setAutoRaise(true);
    setFocusPolicy(Qt::TabFocus);

    // Accessible: the button announces itself as "More information: <hint>" for
    // screen readers and satisfies the keyboard-reachable requirement. The hint may
    // carry rich-text markup; flatten it so no tags are read aloud.
    setAccessibleName(QStringLiteral("More information: ") + plainForAccessibility(hint_text_));

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
    // Anchor to the icon (just below its rect), NOT to QCursor::pos(). A
    // cursor-anchored tip materialised directly under the pointer: it landed in a
    // different spot for every approach direction, jittered as the cursor moved the
    // last pixels onto the glyph, and — depending on approach — Qt tore the freshly
    // shown tip down on the very next move of the same entering motion, so it
    // appeared to "only open when the cursor came from below". A fixed icon-anchored
    // point removes the direction dependence and the jitter; hintAnchor() resolves
    // the widget's own screen and clamps into it for the multi-monitor case.
    QToolTip::showText(hintAnchor(), hint_text_, this);
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
    // Keyboard trigger has no cursor to anchor on; use the same icon-anchored point
    // as the hover path so the tip appears in an identical, stable place either way.
    QToolTip::showText(hintAnchor(), hint_text_, this);
    hover_timer_->start();
}

QPoint InfoHintIcon::hintAnchor() const {
    // A fixed point just below the icon's bottom-left corner. Independent of the
    // cursor, so the tooltip appears in the same place from every approach direction
    // and never shifts with small cursor movements.
    QPoint anchor = mapToGlobal(QPoint(0, height() + kTooltipGapPx));
    // Multi-monitor hardening (mirrors CompareHint::repositionPopover): resolve the
    // widget's own screen and clamp the point into its available geometry. mapToGlobal
    // can be computed against a stale per-window DPI/screen association right after the
    // window is dragged to a differently scaled monitor; the clamp keeps the tip on the
    // icon's screen instead of the primary display's edge.
    if (QScreen* widget_screen = this->screen()) {
        const QRect avail = widget_screen->availableGeometry();
        anchor.setX(std::clamp(anchor.x(), avail.left(), avail.right()));
        anchor.setY(std::clamp(anchor.y(), avail.top(), avail.bottom()));
    }
    return anchor;
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
