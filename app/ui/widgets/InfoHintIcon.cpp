#include "InfoHintIcon.h"

#include <QEnterEvent>
#include <QFocusEvent>
#include <QSize>
#include <QToolTip>

#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"

namespace exosnap::ui::widgets {

namespace {
constexpr int kIconSize = 15; // logical px — matches the design's 15×15 info glyph
} // namespace

InfoHintIcon::InfoHintIcon(const QString& hint_text, QWidget* parent) : QToolButton(parent), hint_text_(hint_text) {
    // Flat, no border, no background — styled via QSS role like the AdvancedPage infoGlyph.
    setProperty("labelRole", "infoGlyph");
    setAutoRaise(true);
    setFocusPolicy(Qt::TabFocus);

    // Accessible: the button announces itself as "More information: <hint>" for
    // screen readers and satisfies the keyboard-reachable requirement.
    setAccessibleName(QStringLiteral("More information: ") + hint_text_);

    // Qt's native QToolTip shows on hover and, because the widget is focusable, a
    // QToolTip is also accessible via keyboard shortcuts on most platforms.
    setToolTip(hint_text_);

    // Fixed size: 18×18 px touch target (design: 18px container, 15px glyph).
    setFixedSize(18, 18);
    setIconSize(QSize(kIconSize, kIconSize));

    updateIcon(false);
}

const QString& InfoHintIcon::hintText() const {
    return hint_text_;
}

void InfoHintIcon::enterEvent(QEnterEvent* event) {
    QToolButton::enterEvent(event);
    updateIcon(true);
    // Hover tooltip is shown by Qt natively via setToolTip() — its internal
    // positioning resolves the correct monitor under the cursor. The previous
    // explicit QToolTip::showText(mapToGlobal(rect().center()), …) here computed a
    // global point that, on a multi-monitor / mixed-DPI desktop, resolved to the
    // wrong screen and popped the tooltip up on the edge of the primary display.
}

void InfoHintIcon::leaveEvent(QEvent* event) {
    QToolButton::leaveEvent(event);
    updateIcon(false);
}

void InfoHintIcon::focusInEvent(QFocusEvent* event) {
    QToolButton::focusInEvent(event);
    updateIcon(true);
    QToolTip::showText(mapToGlobal(rect().center()), hint_text_, this);
}

void InfoHintIcon::focusOutEvent(QFocusEvent* event) {
    QToolButton::focusOutEvent(event);
    updateIcon(false);
}

void InfoHintIcon::updateIcon(bool highlighted) {
    const qreal dpr = devicePixelRatioF();
    const auto& t = ui::theme::ActiveTheme();
    const QString color = highlighted ? QString::fromUtf8(t.ac) : QString::fromUtf8(t.dim);
    setIcon(ui::theme::lucideIcon(QStringLiteral("info"), color, kIconSize, dpr));
}

} // namespace exosnap::ui::widgets
