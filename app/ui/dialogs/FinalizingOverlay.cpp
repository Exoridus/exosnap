#include "FinalizingOverlay.h"

#include "../theme/ExoSnapMetrics.h"
#include "../theme/ExoSnapTheme.h"

#include <QColor>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>

namespace exosnap::ui::dialogs {
namespace {

using M = theme::ExoSnapMetrics;

// Backdrop tint — matches EditExportOverlay/SourcePickerOverlay's dim.
constexpr int kBackdropAlpha = 158;
constexpr int kCardWidth = 240;

} // namespace

FinalizingOverlay::FinalizingOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("finalizingOverlay"));
    setVisible(false);
    // No dismiss action of any kind (no Escape/click-to-close handlers) — it
    // blocks interaction with the page underneath (the default, non-transparent
    // mouse behavior) and only disappears once the caller calls hideOverlay().

    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("finalizingOverlayCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setFixedWidth(kCardWidth);

    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(M::kSpaceLg, M::kSpaceLg, M::kSpaceLg, M::kSpaceLg);
    card_layout->setSpacing(M::kSpaceMd);

    label_ = new QLabel(QStringLiteral("Finishing…"), card);
    label_->setAlignment(Qt::AlignCenter);

    bar_ = new QProgressBar(card);
    bar_->setObjectName(QStringLiteral("finalizingOverlayBar"));
    bar_->setFixedHeight(6);
    bar_->setTextVisible(false);
    bar_->setRange(0, 0); // indeterminate by default (Stopping has no progress data)

    card_layout->addWidget(label_);
    card_layout->addWidget(bar_);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addStretch(1);
    auto* row = new QHBoxLayout();
    row->addStretch(1);
    row->addWidget(card);
    row->addStretch(1);
    root->addLayout(row);
    root->addStretch(1);

    if (parent != nullptr)
        parent->installEventFilter(this);

    // Runs once immediately (initial styling), then again on every later theme switch.
    theme::OnThemeChanged(this, [this]() { applyThemeStyles(); });
}

void FinalizingOverlay::showFinalizing() {
    label_->setText(QStringLiteral("Finishing…"));
    bar_->setRange(0, 0); // indeterminate
    syncGeometryToParent();
    setVisible(true);
    raise();
}

void FinalizingOverlay::showSaving(int percent) {
    const int clamped = std::clamp(percent, 0, 100);
    label_->setText(QStringLiteral("Saving… %1%").arg(clamped));
    if (bar_->maximum() == 0)
        bar_->setRange(0, 100); // switch from indeterminate on the first known fraction
    bar_->setValue(clamped);
    syncGeometryToParent();
    setVisible(true);
    raise();
}

void FinalizingOverlay::hideOverlay() {
    setVisible(false);
}

void FinalizingOverlay::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    QColor backdrop(QString::fromUtf8(theme::ActiveTheme().bg));
    backdrop.setAlpha(kBackdropAlpha);
    painter.fillRect(rect(), backdrop);
}

bool FinalizingOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show)) {
        syncGeometryToParent();
    }
    return QWidget::eventFilter(watched, event);
}

void FinalizingOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    syncGeometryToParent();
    raise();
}

void FinalizingOverlay::syncGeometryToParent() {
    if (QWidget* host = parentWidget())
        setGeometry(host->rect());
}

void FinalizingOverlay::applyThemeStyles() {
    const theme::ExoTheme& t = theme::ActiveTheme();
    QWidget* card = findChild<QFrame*>(QStringLiteral("finalizingOverlayCard"));
    if (card) {
        card->setStyleSheet(QStringLiteral("QFrame#finalizingOverlayCard {"
                                           "background:%1;"
                                           "border: 1px solid %2;"
                                           "border-radius: %3px;"
                                           "}")
                                .arg(t.surf, t.line)
                                .arg(M::kRadiusLg));
    }
    label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-weight:600; font-size:13px; }").arg(t.ink));
    bar_->setStyleSheet(QStringLiteral("QProgressBar { background:%1; border-radius:3px; border:none; }"
                                       "QProgressBar::chunk { background:%2; border-radius:3px; }")
                            .arg(t.raise, t.ac));
}

} // namespace exosnap::ui::dialogs
