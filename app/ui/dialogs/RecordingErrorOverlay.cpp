#include "RecordingErrorOverlay.h"

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

// Backdrop tint — matches the crash/modal dim (rgba(bg, 0.62)).
constexpr int kBackdropAlpha = 158;

} // namespace

RecordingErrorOverlay::RecordingErrorOverlay(const RecordingErrorModel& model, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("recordingErrorOverlay"));
    setFocusPolicy(Qt::StrongFocus);
    setVisible(false);

    panel_ = new RecordingErrorPanel(model, this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(30, 30, 30, 30);
    root->setSpacing(0);
    root->addStretch(1);
    root->addWidget(panel_, 0, Qt::AlignHCenter);
    root->addStretch(1);

    // Forward the panel's action signals so the host can connect to the overlay.
    connect(panel_, &RecordingErrorPanel::sendReportRequested, this, &RecordingErrorOverlay::sendReportRequested);
    connect(panel_, &RecordingErrorPanel::openLogsRequested, this, &RecordingErrorOverlay::openLogsRequested);
    // The Close button dismisses the overlay.
    connect(panel_, &RecordingErrorPanel::dismissRequested, this, &RecordingErrorOverlay::closeOverlay);

    if (parent != nullptr)
        parent->installEventFilter(this);
}

void RecordingErrorOverlay::openOverlay() {
    syncGeometryToParent();
    if (panel_ != nullptr)
        panel_->setVisible(true);
    setVisible(true);
    raise();
    setFocus(Qt::OtherFocusReason);
}

void RecordingErrorOverlay::closeOverlay() {
    if (isHidden())
        return;
    setVisible(false);
    emit closed();
}

bool RecordingErrorOverlay::isOpen() const noexcept {
    return !isHidden();
}

void RecordingErrorOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void RecordingErrorOverlay::mousePressEvent(QMouseEvent* event) {
    // The panel is an opaque child and consumes its own clicks; any press that
    // reaches the overlay is on the dimmed backdrop, so dismiss.
    if (panel_ == nullptr || !panel_->geometry().contains(event->pos())) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void RecordingErrorOverlay::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    QColor backdrop(QString::fromUtf8(theme::ActiveTheme().bg));
    backdrop.setAlpha(kBackdropAlpha);
    painter.fillRect(rect(), backdrop);
}

bool RecordingErrorOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show)) {
        syncGeometryToParent();
    }
    return QWidget::eventFilter(watched, event);
}

void RecordingErrorOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    syncGeometryToParent();
    raise();
    setFocus(Qt::OtherFocusReason);
}

void RecordingErrorOverlay::syncGeometryToParent() {
    if (QWidget* host = parentWidget())
        setGeometry(host->rect());
}

} // namespace exosnap::ui::dialogs
