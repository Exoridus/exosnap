#include "CaptureTargetCard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>
#include <QVBoxLayout>

namespace exosnap::ui::widgets {
namespace {

void restyle(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

} // namespace

CaptureTargetCard::CaptureTargetCard(QWidget* parent) : QFrame(parent) {
    setObjectName("captureTargetCard");
    setProperty("selected", false);
    setCursor(Qt::PointingHandCursor);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(4);

    auto* top_row = new QHBoxLayout();
    top_row->setContentsMargins(0, 0, 0, 0);
    top_row->setSpacing(8);

    title_label_ = new QLabel(this);
    title_label_->setProperty("labelRole", "captureCardTitle");
    title_label_->setText("Monitor");
    title_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    title_label_->installEventFilter(this);

    status_label_ = new QLabel(this);
    status_label_->setProperty("labelRole", "captureCardStatus");
    status_label_->setText("○");
    status_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    status_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    status_label_->installEventFilter(this);

    top_row->addWidget(title_label_);
    top_row->addStretch(1);
    top_row->addWidget(status_label_);

    subtitle_label_ = new QLabel(this);
    subtitle_label_->setProperty("labelRole", "captureCardSubtitle");
    subtitle_label_->setText("DISPLAY1 · 2560×1440 · DDA");
    subtitle_label_->setWordWrap(true);
    subtitle_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    subtitle_label_->installEventFilter(this);

    root->addLayout(top_row);
    root->addWidget(subtitle_label_);
}

void CaptureTargetCard::setTitle(const QString& title) {
    title_label_->setText(title);
}

QString CaptureTargetCard::title() const {
    return title_label_->text();
}

void CaptureTargetCard::setSubtitle(const QString& subtitle) {
    subtitle_label_->setText(subtitle);
}

QString CaptureTargetCard::subtitle() const {
    return subtitle_label_->text();
}

void CaptureTargetCard::setStatusText(const QString& status) {
    status_label_->setText(status);
}

QString CaptureTargetCard::statusText() const {
    return status_label_->text();
}

void CaptureTargetCard::setSelected(bool selected) {
    if (selected_ == selected)
        return;

    selected_ = selected;
    setProperty("selected", selected_);
    status_label_->setText(selected_ ? QStringLiteral("● ACTIVE") : QStringLiteral("○"));
    restyle(this);
}

bool CaptureTargetCard::isSelected() const noexcept {
    return selected_;
}

bool CaptureTargetCard::eventFilter(QObject* watched, QEvent* event) {
    const bool is_child = watched == title_label_ || watched == status_label_ || watched == subtitle_label_;
    if (is_child) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            click_armed_ = (mouse->button() == Qt::LeftButton);
        } else if (event->type() == QEvent::MouseButtonRelease) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (click_armed_ && mouse->button() == Qt::LeftButton) {
                emit clicked();
            }
            click_armed_ = false;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void CaptureTargetCard::mousePressEvent(QMouseEvent* event) {
    click_armed_ = (event->button() == Qt::LeftButton);
    QFrame::mousePressEvent(event);
}

void CaptureTargetCard::mouseReleaseEvent(QMouseEvent* event) {
    if (click_armed_ && event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        emit clicked();
    }
    click_armed_ = false;
    QFrame::mouseReleaseEvent(event);
}

} // namespace exosnap::ui::widgets
