#include "EditExportOverlay.h"

#include "../../pages/EditExportPage.h"
#include "../theme/ExoSnapTheme.h"

#include <QColor>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRect>
#include <QShowEvent>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {
namespace {

// Backdrop is fully opaque here (unlike SourcePickerOverlay's semi-transparent
// Hybrid modal dim). The overlay no longer covers the window chrome, so the
// original bleed-through argument is spent — but a translucent backdrop would
// show the Record page through the margin band, which is not wanted either.
constexpr int kBackdropAlpha = 255;
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
    // built once.
    page_ = new EditExportPage(this);
    page_->setObjectName(QStringLiteral("editExportOverlayPanel")); // additive QSS panel framing
    // Required for the QSS background/border on the plain-QWidget page to paint —
    // without it the overlay's backdrop shows through the page body instead of the
    // panel framing (same idiom as SourcePickerPanel).
    page_->setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(kOverlayMargin, kOverlayMargin, kOverlayMargin, kOverlayMargin);
    root->setSpacing(0);
    root->addWidget(page_, 1);

    // The back arrow runs the page's own discard guard before it emits, so by
    // the time this fires the close is already agreed. Closing is the direct
    // equivalent of the former stack-swap-back, since Record is the page
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

bool EditExportOverlay::requestCloseOverlay() {
    if (isHidden())
        return true;
    if (page_ != nullptr && !page_->confirmDiscardEdits())
        return false;
    closeOverlay();
    return true;
}

bool EditExportOverlay::isOpen() const noexcept {
    return !isHidden();
}

void EditExportOverlay::setTopInset(int pixels) {
    if (pixels == top_inset_)
        return;
    top_inset_ = pixels;
    syncGeometryToParent();
}

bool EditExportOverlay::isDismissBlocked() const noexcept {
    return page_ != nullptr && page_->isExportRunning();
}

void EditExportOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        if (!isDismissBlocked())
            requestCloseOverlay();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void EditExportOverlay::mousePressEvent(QMouseEvent* event) {
    if (page_ == nullptr || !page_->geometry().contains(event->pos())) {
        if (!isDismissBlocked())
            requestCloseOverlay();
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
    if (QWidget* host = parentWidget()) {
        QRect area = host->rect();
        area.setTop(area.top() + top_inset_);
        setGeometry(area);
    }
}

} // namespace exosnap::ui::dialogs
