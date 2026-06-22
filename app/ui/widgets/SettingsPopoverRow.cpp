#include "SettingsPopoverRow.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QSize>
#include <QToolButton>

#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"
#include "InfoHintIcon.h"

namespace exosnap::ui::widgets {

namespace {
constexpr int kCogIconSize = 16; // logical px
} // namespace

SettingsPopoverRow::SettingsPopoverRow(const QString& label, QWidget* parent) : QWidget(parent) {
    auto* hl = new QHBoxLayout(this);
    hl->setContentsMargins(0, 12, 0, 12);
    hl->setSpacing(8);

    auto* lbl = new QLabel(label, this);
    lbl->setProperty("labelRole", "settingsRowLabel");
    hl->addWidget(lbl, 0);

    // Info-hint icon placeholder — inserted after label if setInfoHint() is called.
    // We use a stretch here initially; info hint is inserted before it.
    hl->addStretch(1);

    // Muted status text (optional).
    status_label_ = new QLabel(this);
    status_label_->setProperty("labelRole", "muted");
    status_label_->setVisible(false);
    hl->addWidget(status_label_, 0);

    // ⚙ cogwheel button.
    cog_btn_ = new QToolButton(this);
    cog_btn_->setObjectName(QStringLiteral("settingsPopoverCog"));
    cog_btn_->setAutoRaise(true);
    cog_btn_->setFixedSize(28, 28);
    cog_btn_->setIconSize(QSize(kCogIconSize, kCogIconSize));
    cog_btn_->setToolTip(QStringLiteral("Advanced options"));
    const auto& t = ui::theme::ActiveTheme();
    cog_btn_->setIcon(ui::theme::lucideIcon(QStringLiteral("settings"), QString::fromUtf8(t.mut), kCogIconSize));
    hl->addWidget(cog_btn_, 0);

    // Popover panel — Qt::Popup gets free outside-click-dismiss and Escape handling
    // without the QMenu overhead of needing QWidgetAction.
    // Parent is `this` for memory management; Qt::Popup makes it a top-level transient.
    popover_panel_ = new QWidget(this, Qt::Popup | Qt::FramelessWindowHint);
    popover_panel_->setObjectName(QStringLiteral("settingsPopoverPanel"));
    popover_panel_->setAttribute(Qt::WA_TranslucentBackground, false);
    popover_panel_->setMinimumWidth(320);

    auto* panel_outer = new QVBoxLayout(popover_panel_);
    panel_outer->setContentsMargins(0, 0, 0, 0);
    panel_outer->setSpacing(0);

    auto* panel_inner = new QWidget(popover_panel_);
    panel_inner->setObjectName(QStringLiteral("settingsPopoverPanelInner"));
    panel_outer->addWidget(panel_inner);

    popover_content_layout_ = new QVBoxLayout(panel_inner);
    popover_content_layout_->setContentsMargins(16, 12, 16, 12);
    popover_content_layout_->setSpacing(0);

    connect(cog_btn_, &QToolButton::clicked, this, &SettingsPopoverRow::onCogClicked);
}

void SettingsPopoverRow::setInfoHint(const QString& hint) {
    auto* hl = qobject_cast<QHBoxLayout*>(layout());
    if (!hl)
        return;
    // Insert info icon at index 1 (right after the row label at index 0).
    auto* hint_icon = new InfoHintIcon(hint, this);
    hl->insertWidget(1, hint_icon, 0);
}

void SettingsPopoverRow::setPrimaryControl(QWidget* w) {
    auto* hl = qobject_cast<QHBoxLayout*>(layout());
    if (!hl || !w)
        return;
    w->setParent(this);
    // Insert just before the cog button (second-to-last item).
    const int cog_idx = hl->indexOf(cog_btn_);
    hl->insertWidget(cog_idx, w, 0);
}

void SettingsPopoverRow::setStatusText(const QString& s) {
    if (!status_label_)
        return;
    status_label_->setText(s);
    status_label_->setVisible(!s.isEmpty());
}

QVBoxLayout* SettingsPopoverRow::popoverContentLayout() const {
    return popover_content_layout_;
}

void SettingsPopoverRow::onCogClicked() {
    if (!popover_panel_)
        return;

    if (popover_panel_->isVisible()) {
        popover_panel_->hide();
        return;
    }

    // Anchor panel below the cog button (screen-edge-safe: map to global).
    const QPoint global_bottom_left = cog_btn_->mapToGlobal(QPoint(0, cog_btn_->height()));
    popover_panel_->adjustSize();

    // Try below the button; if off-screen push it up. Clamp against the screen the
    // cog button actually lives on — NOT primaryScreen() — or on a multi-monitor
    // desktop the popover gets shoved onto the primary display.
    const QScreen* cog_screen = cog_btn_->screen();
    if (cog_screen == nullptr)
        cog_screen = QGuiApplication::screenAt(global_bottom_left);
    if (cog_screen == nullptr)
        cog_screen = QGuiApplication::primaryScreen();
    const QRect screen = cog_screen->availableGeometry();
    QPoint pos = global_bottom_left;
    if (pos.y() + popover_panel_->height() > screen.bottom()) {
        pos.setY(cog_btn_->mapToGlobal(QPoint(0, 0)).y() - popover_panel_->height());
    }
    // Clamp horizontal.
    if (pos.x() + popover_panel_->width() > screen.right()) {
        pos.setX(screen.right() - popover_panel_->width());
    }

    popover_panel_->move(pos);
    popover_panel_->show();
    popover_panel_->raise();
}

} // namespace exosnap::ui::widgets
