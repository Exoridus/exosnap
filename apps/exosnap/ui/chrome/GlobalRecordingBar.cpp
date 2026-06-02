#include "GlobalRecordingBar.h"

#include "../widgets/StatusPill.h"

#include <QHBoxLayout>

namespace exosnap::ui::chrome {

GlobalRecordingBar::GlobalRecordingBar(QWidget* parent) : QWidget(parent) {
    setObjectName("globalRecordingBar");
    setFixedHeight(kHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(14, 6, 14, 6);
    root->setSpacing(0);

    status_pill_ = new ui::widgets::StatusPill(this);
    status_pill_->setObjectName(QStringLiteral("globalBarStatusChip"));
    status_pill_->setText(QStringLiteral("READY"));
    status_pill_->setTone(ui::widgets::StatusPill::Tone::Ready);
    status_pill_->setDotVisible(false);

    root->addWidget(status_pill_, 0, Qt::AlignVCenter);
    root->addStretch(1);

    refreshVisualState();
}

void GlobalRecordingBar::setStatusLabel(const QString& status_text) {
    const QString normalized = normalizeStatusLabel(status_text);
    if (status_label_ == normalized)
        return;

    status_label_ = normalized;
    refreshVisualState();
}

const QString& GlobalRecordingBar::statusLabel() const {
    return status_label_;
}

void GlobalRecordingBar::refreshVisualState() {
    refreshStatusChip();
}

void GlobalRecordingBar::refreshStatusChip() {
    if (status_label_ == QStringLiteral("REC")) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Recording);
        status_pill_->setDotVisible(true);
    } else if (status_label_ == QStringLiteral("READY")) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Ready);
        status_pill_->setDotVisible(false);
    } else if (status_label_ == QStringLiteral("BLOCKED") || status_label_ == QStringLiteral("ERROR")) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Blocked);
        status_pill_->setDotVisible(true);
    } else {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Warn);
        status_pill_->setDotVisible(true);
    }

    status_pill_->setText(status_label_);
    const QString tooltip = QStringLiteral("Current recording status: %1.").arg(status_label_);
    status_pill_->setToolTip(tooltip);
    status_pill_->setAccessibleName(QStringLiteral("Recording status: %1").arg(status_label_));
    status_pill_->setAccessibleDescription(tooltip);
}

QString GlobalRecordingBar::normalizeStatusLabel(const QString& status_text) {
    const QString normalized = status_text.trimmed().toUpper();
    if (normalized.contains(QStringLiteral("CHECK")))
        return QStringLiteral("CHECKING");
    if (normalized.contains(QStringLiteral("START")))
        return QStringLiteral("STARTING");
    if (normalized.contains(QStringLiteral("STOP")))
        return QStringLiteral("STOPPING");
    if (normalized.contains(QStringLiteral("PAUSED")))
        return QStringLiteral("PAUSED");
    if (normalized.contains(QStringLiteral("REC")))
        return QStringLiteral("REC");
    if (normalized.contains(QStringLiteral("BLOCK")))
        return QStringLiteral("BLOCKED");
    if (normalized.contains(QStringLiteral("ERROR")))
        return QStringLiteral("ERROR");
    return QStringLiteral("READY");
}

} // namespace exosnap::ui::chrome
