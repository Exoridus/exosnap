#include "ExportOverlay.h"

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>

namespace exosnap::ui::dialogs {

ExportOverlay::ExportOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("exportOverlay"));
    setVisible(false);
}

void ExportOverlay::openCard() {
    state_ = State::Options;
    setVisible(true);
}

void ExportOverlay::closeCard() {
    if (state_ == State::Running)
        return;
    setVisible(false);
}

bool ExportOverlay::isCardOpen() const noexcept {
    return isVisible();
}

ExportOverlay::State ExportOverlay::state() const noexcept {
    return state_;
}

QString ExportOverlay::containerKey() const {
    return QStringLiteral("mkv");
}

QString ExportOverlay::saveModeKey() const {
    return QStringLiteral("new");
}

void ExportOverlay::showRunning() {
    state_ = State::Running;
}

void ExportOverlay::setProgress(int /*percent*/) {
}

void ExportOverlay::showDone(const QString& /*output_path*/) {
    state_ = State::Done;
}

void ExportOverlay::showFailed(const QString& /*error_message*/) {
    state_ = State::Failed;
}

void ExportOverlay::applyThemeStyles() {
}

void ExportOverlay::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
}

void ExportOverlay::keyPressEvent(QKeyEvent* event) {
    QWidget::keyPressEvent(event);
}

void ExportOverlay::mousePressEvent(QMouseEvent* event) {
    QWidget::mousePressEvent(event);
}

bool ExportOverlay::eventFilter(QObject* watched, QEvent* event) {
    return QWidget::eventFilter(watched, event);
}

} // namespace exosnap::ui::dialogs
