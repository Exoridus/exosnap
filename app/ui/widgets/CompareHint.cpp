#include "CompareHint.h"

#include <QApplication>
#include <QEnterEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSize>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "../../models/SettingsCompareData.h"
#include "../theme/ExoSnapPalette.h"
#include "../theme/LucideIcon.h"

namespace exosnap::ui::widgets {

namespace {

constexpr int kGlyphSize = 15; // logical px — same as InfoHintIcon
constexpr int kPopoverWidth = 312;

// Build a small colored "badge" label (mono, uppercase).
QLabel* makeBadge(const QString& text, const QString& color, const QString& bg, const QString& border,
                  QWidget* parent = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral("QLabel { font-family: 'IBM Plex Mono', monospace; font-size: 8px;"
                                        " letter-spacing: 0.04em; text-transform: uppercase;"
                                        " color: %1; background: %2; border: 1px solid %3;"
                                        " border-radius: 4px; padding: 2px 5px; }")
                             .arg(color, bg, border));
    return label;
}

} // namespace

// ── Construction ─────────────────────────────────────────────────────────────

CompareHint::CompareHint(const QString& compare_key, const QString& current_value, QWidget* parent)
    : QToolButton(parent), compare_key_(compare_key), current_value_(current_value) {
    using P = ui::theme::ExoSnapPalette;

    // Flat "compareGlyph" role — same treatment as InfoHintIcon's "infoGlyph".
    setProperty("labelRole", "compareGlyph");
    setAutoRaise(true);
    setFocusPolicy(Qt::TabFocus);
    setFixedSize(18, 18);
    setIconSize(QSize(kGlyphSize, kGlyphSize));
    setCheckable(false);

    // Accessible name uses the data title if available, else the key.
    const auto* d = ui::compare::compareData(compare_key_);
    const QString title = d ? d->title : compare_key_;
    setAccessibleName(QStringLiteral("Compare options: ") + title);

    updateIcon(false);

    // If no data: widget is a no-op hint icon; skip popover.
    if (!d)
        return;

    // Click toggles pin state.
    connect(this, &QToolButton::clicked, this, &CompareHint::onClicked);
}

// ── Public API ───────────────────────────────────────────────────────────────

void CompareHint::setCurrentValue(const QString& value) {
    if (current_value_ == value)
        return;
    current_value_ = value;
    // If the popover is visible, rebuild the option rows in place.
    if (popover_ && popover_->isVisible())
        rebuildRows();
}

QString CompareHint::compareKey() const {
    return compare_key_;
}

QString CompareHint::currentValue() const {
    return current_value_;
}

// ── Icon state ───────────────────────────────────────────────────────────────

void CompareHint::updateIcon(bool highlighted) {
    const qreal dpr = devicePixelRatioF();
    const QString color = highlighted ? QString::fromLatin1(ui::theme::ExoSnapPalette::kAccent)
                                      : QString::fromLatin1(ui::theme::ExoSnapPalette::kText3);
    setIcon(ui::theme::lucideIcon(QStringLiteral("info"), color, kGlyphSize, dpr));
}

// ── Hover / focus events ──────────────────────────────────────────────────────

void CompareHint::enterEvent(QEnterEvent* event) {
    QToolButton::enterEvent(event);
    popover_hovered_ = true;
    updateIcon(true);
    if (ui::compare::compareData(compare_key_))
        showPopover();
}

void CompareHint::leaveEvent(QEvent* event) {
    QToolButton::leaveEvent(event);
    popover_hovered_ = false;
    updateIcon(popover_pinned_ || (popover_ && popover_->isVisible()));
    if (!popover_pinned_)
        hidePopover();
}

void CompareHint::focusInEvent(QFocusEvent* event) {
    QToolButton::focusInEvent(event);
    updateIcon(true);
    if (ui::compare::compareData(compare_key_))
        showPopover();
}

void CompareHint::focusOutEvent(QFocusEvent* event) {
    QToolButton::focusOutEvent(event);
    updateIcon(false);
    if (!popover_pinned_)
        hidePopover();
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void CompareHint::onClicked() {
    if (!ui::compare::compareData(compare_key_))
        return;
    popover_pinned_ = !popover_pinned_;
    if (popover_pinned_) {
        showPopover();
    } else {
        hidePopover();
        updateIcon(popover_hovered_);
    }
}

// ── Popover build / show / hide ───────────────────────────────────────────────

void CompareHint::buildPopover() {
    using P = ui::theme::ExoSnapPalette;
    const auto* d = ui::compare::compareData(compare_key_);
    if (!d)
        return;

    // Frameless popup window — Qt::Popup closes on outside click and Esc.
    popover_ = new QWidget(nullptr, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    popover_->setFixedWidth(kPopoverWidth);
    popover_->setAttribute(Qt::WA_TranslucentBackground, false);

    // Outer frame styling via setStyleSheet (inline, not via QSS role, because the
    // popover is a top-level window and does not inherit the app stylesheet).
    popover_->setStyleSheet(
        QStringLiteral("QWidget { background: %1; border: 1px solid %2; border-radius: 13px; }"
                       "QWidget#popoverInner { background: transparent; border: none; border-radius: 0; }")
            .arg(QLatin1String(P::kBg2), QLatin1String(P::kLine2)));

    // Drop shadow.
    auto* shadow = new QGraphicsDropShadowEffect(popover_);
    shadow->setBlurRadius(32);
    shadow->setOffset(0, 8);
    shadow->setColor(QColor(0, 0, 0, 120));
    popover_->setGraphicsEffect(shadow);

    auto* outerLayout = new QVBoxLayout(popover_);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    // ── Header ────────────────────────────────────────────────────────────────
    auto* header = new QWidget(popover_);
    header->setObjectName(QStringLiteral("popoverInner"));
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(14, 12, 14, 9);
    headerLayout->setSpacing(0);

    // Title row: title label + COMPARE badge
    auto* titleRow = new QWidget(header);
    titleRow->setObjectName(QStringLiteral("popoverInner"));
    auto* titleRowLayout = new QHBoxLayout(titleRow);
    titleRowLayout->setContentsMargins(0, 0, 0, 0);
    titleRowLayout->setSpacing(8);

    auto* titleLabel = new QLabel(d->title, titleRow);
    titleLabel->setStyleSheet(QStringLiteral("QLabel { font-size: 13px; font-weight: 600; color: %1;"
                                             " background: transparent; border: none; border-radius: 0; }")
                                  .arg(QLatin1String(P::kText0)));

    auto* compareBadge = makeBadge(QStringLiteral("COMPARE"), QLatin1String(P::kAccent), QLatin1String(P::kAccentDim),
                                   QLatin1String(P::kAccentBorderStrong), titleRow);

    titleRowLayout->addWidget(titleLabel);
    titleRowLayout->addStretch(1);
    titleRowLayout->addWidget(compareBadge);

    headerLayout->addWidget(titleRow);

    // Sub-heading
    if (!d->sub.isEmpty()) {
        auto* subLabel = new QLabel(d->sub, header);
        subLabel->setStyleSheet(QStringLiteral("QLabel { font-size: 11px; color: %1; background: transparent;"
                                               " border: none; border-radius: 0; margin-top: 3px; }")
                                    .arg(QLatin1String(P::kText3)));
        subLabel->setWordWrap(true);
        headerLayout->addWidget(subLabel);
    }

    outerLayout->addWidget(header);

    // Hairline separator
    auto* sep = new QFrame(popover_);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(
        QStringLiteral("QFrame { background: %1; border: none; border-radius: 0; min-height: 1px; max-height: 1px; }")
            .arg(QLatin1String(P::kLine1)));
    outerLayout->addWidget(sep);

    // ── Option list (container for rows, rebuilt on setCurrentValue) ──────────
    auto* rowsContainer = new QWidget(popover_);
    rowsContainer->setObjectName(QStringLiteral("compareHintRows"));
    rowsContainer->setProperty("popoverInner", true);
    auto* rowsLayout = new QVBoxLayout(rowsContainer);
    rowsLayout->setContentsMargins(0, 0, 0, 0);
    rowsLayout->setSpacing(0);
    outerLayout->addWidget(rowsContainer);

    popover_->adjustSize();

    // Rebuild fills the rows container.
    rebuildRows();
}

void CompareHint::rebuildRows() {
    using P = ui::theme::ExoSnapPalette;
    if (!popover_)
        return;

    auto* rowsContainer = popover_->findChild<QWidget*>(QStringLiteral("compareHintRows"));
    if (!rowsContainer)
        return;

    // Remove existing rows
    auto* oldLayout = rowsContainer->layout();
    if (oldLayout) {
        QLayoutItem* item = nullptr;
        while ((item = oldLayout->takeAt(0)) != nullptr) {
            if (item->widget())
                item->widget()->deleteLater();
            delete item;
        }
        delete oldLayout;
    }

    auto* rowsLayout = new QVBoxLayout(rowsContainer);
    rowsLayout->setContentsMargins(0, 0, 0, 0);
    rowsLayout->setSpacing(0);

    const auto* d = ui::compare::compareData(compare_key_);
    if (!d)
        return;

    for (const auto& opt : d->options) {
        const bool selected = (opt.value == current_value_);

        // Each row is a QToolButton for full hover/click without style hacks.
        auto* row = new QToolButton(rowsContainer);
        row->setAutoRaise(true);
        row->setFocusPolicy(Qt::TabFocus);
        row->setToolButtonStyle(Qt::ToolButtonIconOnly);
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        // Row background styling
        QString rowBg = selected ? QLatin1String(P::kAccentDim) : QStringLiteral("transparent");
        QString rowBorderLeft = selected ? QLatin1String(P::kAccent) : QStringLiteral("transparent");
        QString hoverBg = selected ? QLatin1String(P::kAccentDim) : QStringLiteral("rgba(255,255,255,0.04)");
        row->setStyleSheet(QStringLiteral("QToolButton { background: %1;"
                                          " border: none;"
                                          " border-left: 2px solid %2;"
                                          " border-radius: 0;"
                                          " padding: 0;"
                                          " text-align: left; }"
                                          "QToolButton:hover { background: %3; }")
                               .arg(rowBg, rowBorderLeft, hoverBg));

        // Inner layout: marker | content block
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(12, 9, 14, 9);
        rowLayout->setSpacing(10);

        // Marker: check icon or dim dot
        auto* markerWidget = new QWidget(row);
        markerWidget->setFixedWidth(15);
        markerWidget->setStyleSheet(QStringLiteral("QWidget { background: transparent; border: none; }"));
        auto* markerLayout = new QHBoxLayout(markerWidget);
        markerLayout->setContentsMargins(0, 0, 0, 0);
        markerLayout->setAlignment(Qt::AlignCenter);

        if (selected) {
            const qreal dpr = devicePixelRatioF();
            auto* checkBtn = new QLabel(markerWidget);
            checkBtn->setPixmap(ui::theme::lucidePixmap(QStringLiteral("check"), QLatin1String(P::kAccent), 14, dpr));
            checkBtn->setFixedSize(14, 14);
            checkBtn->setStyleSheet(QStringLiteral("QLabel { background: transparent; border: none; }"));
            markerLayout->addWidget(checkBtn);
        } else {
            auto* dot = new QWidget(markerWidget);
            dot->setFixedSize(6, 6);
            dot->setStyleSheet(QStringLiteral("QWidget { background: %1; border-radius: 3px; border: none; }")
                                   .arg(QLatin1String(P::kLine2)));
            markerLayout->addWidget(dot);
        }
        rowLayout->addWidget(markerWidget, 0, Qt::AlignTop);

        // Content block: name row + effect line
        auto* contentWidget = new QWidget(row);
        contentWidget->setStyleSheet(QStringLiteral("QWidget { background: transparent; border: none; }"));
        auto* contentLayout = new QVBoxLayout(contentWidget);
        contentLayout->setContentsMargins(0, 0, 0, 0);
        contentLayout->setSpacing(2);

        // Name row: name + optional recommended tag + optional tier tag
        auto* nameRow = new QWidget(contentWidget);
        nameRow->setStyleSheet(QStringLiteral("QWidget { background: transparent; border: none; }"));
        auto* nameRowLayout = new QHBoxLayout(nameRow);
        nameRowLayout->setContentsMargins(0, 0, 0, 0);
        nameRowLayout->setSpacing(7);

        auto* nameLabel = new QLabel(opt.value, nameRow);
        nameLabel->setStyleSheet(QStringLiteral("QLabel { font-size: 12px; font-weight: 600; color: %1;"
                                                " background: transparent; border: none; border-radius: 0; }")
                                     .arg(selected ? QLatin1String(P::kAccent) : QLatin1String(P::kText0)));

        nameRowLayout->addWidget(nameLabel);

        if (opt.recommended) {
            auto* recTag =
                makeBadge(QStringLiteral("RECOMMENDED"), QLatin1String(P::kOk),
                          QStringLiteral("rgba(132,203,162,0.12)"), QStringLiteral("rgba(132,203,162,0.30)"), nameRow);
            nameRowLayout->addWidget(recTag);
        }

        if (!opt.tier.isEmpty()) {
            auto* tierTag = new QLabel(opt.tier, nameRow);
            tierTag->setStyleSheet(
                QStringLiteral("QLabel { font-family: 'IBM Plex Mono', monospace; font-size: 8px;"
                               " color: %1; background: transparent; border: none; border-radius: 0; }")
                    .arg(QLatin1String(P::kText3)));
            nameRowLayout->addWidget(tierTag);
        }

        nameRowLayout->addStretch(1);
        contentLayout->addWidget(nameRow);

        // Effect line
        auto* effectLabel = new QLabel(opt.effect, contentWidget);
        effectLabel->setStyleSheet(QStringLiteral("QLabel { font-size: 11px; color: %1;"
                                                  " background: transparent; border: none; border-radius: 0; }")
                                       .arg(QLatin1String(P::kText2)));
        effectLabel->setWordWrap(true);
        contentLayout->addWidget(effectLabel);

        rowLayout->addWidget(contentWidget, 1);
        row->setLayout(rowLayout);

        // Capture the value by value for the lambda
        const QString optValue = opt.value;
        connect(row, &QToolButton::clicked, this, [this, optValue]() {
            setCurrentValue(optValue);
            emit optionSelected(optValue);
            popover_pinned_ = false;
            hidePopover();
        });

        rowsLayout->addWidget(row);
    }

    rowsContainer->adjustSize();
    if (popover_->isVisible()) {
        popover_->adjustSize();
        repositionPopover();
    }
}

void CompareHint::showPopover() {
    if (!popover_)
        buildPopover();
    if (!popover_)
        return;

    repositionPopover();
    popover_->show();
    popover_->raise();
    updateIcon(true);
}

void CompareHint::hidePopover() {
    if (popover_)
        popover_->hide();
    if (!popover_hovered_ && !popover_pinned_)
        updateIcon(false);
}

void CompareHint::repositionPopover() {
    if (!popover_)
        return;

    popover_->adjustSize();

    // Anchor: horizontally centered under the glyph button, with a 6px gap.
    const QPoint glyphGlobal = mapToGlobal(QPoint(0, 0));
    const int glyphCenterX = glyphGlobal.x() + width() / 2;
    const int popW = popover_->width();
    const int popH = popover_->height();

    int x = glyphCenterX - popW / 2;
    int y = glyphGlobal.y() + height() + 6;

    // Flip above if not enough room below (use primary screen geometry).
    QScreen* screen = QApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        if (y + popH > avail.bottom())
            y = glyphGlobal.y() - popH - 6;
        // Clamp horizontally.
        if (x < avail.left())
            x = avail.left() + 4;
        if (x + popW > avail.right())
            x = avail.right() - popW - 4;
    }

    popover_->move(x, y);
}

} // namespace exosnap::ui::widgets
