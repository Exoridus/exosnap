#include "CrashReportOverlay.h"

#include "../theme/ExoSnapPalette.h"

#include <QColor>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QShowEvent>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {
namespace {

// Backdrop tint — matches Hybrid modal dim (rgba(8,8,10, 0.62)).
constexpr int kBackdropAlpha = 158;

} // namespace

CrashReportOverlay::CrashReportOverlay(const CrashReportModel& model, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("crashReportOverlay"));
    setFocusPolicy(Qt::StrongFocus);
    setVisible(false);

    // CrashReportPanel is a plain QWidget — no native OS window is created, so no
    // separate OS chrome appears; it is embedded directly in the overlay.
    panel_ = new CrashReportPanel(model, this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(30, 30, 30, 30);
    root->setSpacing(0);
    root->addStretch(1);
    root->addWidget(panel_, 0, Qt::AlignHCenter);
    root->addStretch(1);

    // Forward the panel's action signals so the host can connect to the overlay.
    connect(panel_, &CrashReportPanel::sendReportRequested, this, &CrashReportOverlay::sendReportRequested);
    connect(panel_, &CrashReportPanel::restartRequested, this, &CrashReportOverlay::restartRequested);
    connect(panel_, &CrashReportPanel::reportOnGitHubRequested, this, &CrashReportOverlay::reportOnGitHubRequested);
    connect(panel_, &CrashReportPanel::openCrashFolderRequested, this, &CrashReportOverlay::openCrashFolderRequested);
    connect(panel_, &CrashReportPanel::autoSendToggled, this, &CrashReportOverlay::autoSendToggled);
    // The chrome close X / overflow "Don't send & close" decline AND dismiss the overlay.
    connect(panel_, &CrashReportPanel::dontSendRequested, this, &CrashReportOverlay::dontSendRequested);
    connect(panel_, &CrashReportPanel::dontSendRequested, this, &CrashReportOverlay::closeOverlay);

    if (parent != nullptr)
        parent->installEventFilter(this);
}

void CrashReportOverlay::openOverlay() {
    syncGeometryToParent();
    if (panel_ != nullptr)
        panel_->setVisible(true);
    setVisible(true);
    raise();
    setFocus(Qt::OtherFocusReason);
}

void CrashReportOverlay::closeOverlay() {
    if (isHidden())
        return;
    setVisible(false);
    emit closed();
}

bool CrashReportOverlay::isOpen() const noexcept {
    return !isHidden();
}

bool CrashReportOverlay::autoSendChecked() const {
    return panel_ != nullptr && panel_->autoSendChecked();
}

void CrashReportOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void CrashReportOverlay::mousePressEvent(QMouseEvent* event) {
    // The panel is an opaque child and consumes its own clicks; any press that
    // reaches the overlay is on the dimmed backdrop, so dismiss.
    if (panel_ == nullptr || !panel_->geometry().contains(event->pos())) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void CrashReportOverlay::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    QColor backdrop(theme::ExoSnapPalette::kBg0);
    backdrop.setAlpha(kBackdropAlpha);
    painter.fillRect(rect(), backdrop);
}

bool CrashReportOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show)) {
        syncGeometryToParent();
    }
    return QWidget::eventFilter(watched, event);
}

void CrashReportOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    syncGeometryToParent();
    raise();
    setFocus(Qt::OtherFocusReason);
}

void CrashReportOverlay::syncGeometryToParent() {
    if (QWidget* host = parentWidget())
        setGeometry(host->rect());
}

} // namespace exosnap::ui::dialogs
