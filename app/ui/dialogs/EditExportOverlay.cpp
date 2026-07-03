#include "EditExportOverlay.h"

#include "../../pages/EditExportPage.h"
#include "../theme/ExoSnapTheme.h"

#include <QColor>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QShowEvent>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {
namespace {

// Backdrop tint — matches SourcePickerOverlay's Hybrid modal dim (rgba(8,8,10, 0.62)).
constexpr int kBackdropAlpha = 158;
// The hosted page reads as a full editor surface, not a small centered dialog —
// a thin margin keeps a visible/clickable backdrop band for dismiss-by-click.
constexpr int kOverlayMargin = 20;

} // namespace

EditExportOverlay::EditExportOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName("editExportOverlay");
    setFocusPolicy(Qt::StrongFocus);
    setVisible(false);

    // EditExportPage is a plain QWidget — no native OS window created, so no
    // separate window chrome appears. It is embedded directly in the overlay and
    // built once; its own internals (phases, trim, output) are unchanged.
    page_ = new EditExportPage(this);
    page_->setObjectName(QStringLiteral("editExportOverlayPanel")); // additive QSS panel framing
    // Required for the QSS background/border on the plain-QWidget page to paint —
    // without it the dimmed Record page bleeds through the page body (same idiom
    // as SourcePickerPanel).
    page_->setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(kOverlayMargin, kOverlayMargin, kOverlayMargin, kOverlayMargin);
    root->setSpacing(0);
    root->addWidget(page_, 1);

    // Back / Done both emit backRequested regardless of phase (pre-existing,
    // unchanged EditExportPage behavior) — closing the overlay is the direct
    // equivalent of the former stack-swap-back, since Record is already the page
    // underneath.
    connect(page_, &EditExportPage::backRequested, this, &EditExportOverlay::closeOverlay);

    if (parent != nullptr)
        parent->installEventFilter(this);
}

void EditExportOverlay::openOverlay() {
    const bool was_hidden = isHidden();
    syncGeometryToParent();
    setVisible(true);
    raise();
    if (page_)
        page_->setFocus(Qt::OtherFocusReason);
    if (was_hidden)
        emit opened();
}

void EditExportOverlay::closeOverlay() {
    if (isHidden())
        return;
    setVisible(false);
    emit closed();
}

bool EditExportOverlay::isOpen() const noexcept {
    return !isHidden();
}

bool EditExportOverlay::isDismissBlocked() const noexcept {
    return page_ != nullptr && page_->phase() == exosnap::EditExportPage::Phase::Exporting;
}

void EditExportOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        if (!isDismissBlocked())
            closeOverlay();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void EditExportOverlay::mousePressEvent(QMouseEvent* event) {
    if (page_ == nullptr || !page_->geometry().contains(event->pos())) {
        if (!isDismissBlocked())
            closeOverlay();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void EditExportOverlay::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    QColor backdrop(QString::fromUtf8(theme::ActiveTheme().bg));
    backdrop.setAlpha(kBackdropAlpha);
    painter.fillRect(rect(), backdrop);
}

bool EditExportOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show)) {
        syncGeometryToParent();
    }
    return QWidget::eventFilter(watched, event);
}

void EditExportOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    syncGeometryToParent();
    raise();
    if (page_)
        page_->setFocus(Qt::OtherFocusReason);
}

void EditExportOverlay::syncGeometryToParent() {
    if (QWidget* host = parentWidget())
        setGeometry(host->rect());
}

} // namespace exosnap::ui::dialogs
