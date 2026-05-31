#include "CaptureTargetCard.h"

#include <QHBoxLayout>
#include <QKeyEvent>
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

constexpr int kThumbnailWidth = 160;
constexpr int kThumbnailHeight = 90;

} // namespace

CaptureTargetCard::CaptureTargetCard(QWidget* parent) : QFrame(parent) {
    setObjectName("captureTargetCard");
    setProperty("selected", false);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(10, 10, 14, 10);
    root->setSpacing(10);

    thumbnail_label_ = new QLabel(this);
    thumbnail_label_->setObjectName("captureCardThumbnail");
    thumbnail_label_->setFixedSize(kThumbnailWidth, kThumbnailHeight);
    thumbnail_label_->setAlignment(Qt::AlignCenter);
    thumbnail_label_->setProperty("labelRole", "captureCardThumbnail");
    thumbnail_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    thumbnail_label_->installEventFilter(this);
    setThumbnailPlaceholder();
    root->addWidget(thumbnail_label_);

    auto* text_col = new QVBoxLayout();
    text_col->setContentsMargins(0, 0, 0, 0);
    text_col->setSpacing(4);

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

    help_label_ = new QLabel(this);
    help_label_->setProperty("labelRole", "captureCardHelp");
    help_label_->setWordWrap(true);
    help_label_->setVisible(false);
    help_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    help_label_->installEventFilter(this);

    text_col->addLayout(top_row);
    text_col->addWidget(subtitle_label_);
    text_col->addWidget(help_label_);
    root->addLayout(text_col, 1);

    updateStatusLabel();
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
    status_text_ = status;
    updateStatusLabel();
}

QString CaptureTargetCard::statusText() const {
    return status_text_;
}

void CaptureTargetCard::setSelected(bool selected) {
    if (selected_ == selected)
        return;

    selected_ = selected;
    setProperty("selected", selected_);
    updateStatusLabel();
    restyle(this);
}

bool CaptureTargetCard::isSelected() const noexcept {
    return selected_;
}

void CaptureTargetCard::setThumbnail(const QPixmap& pixmap) {
    if (pixmap.isNull()) {
        setThumbnailPlaceholder();
        return;
    }
    has_thumbnail_ = true;
    thumbnail_label_->setPixmap(
        pixmap.scaled(kThumbnailWidth, kThumbnailHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    thumbnail_label_->setProperty("labelRole", "captureCardThumbnail");
    restyle(thumbnail_label_);
}

void CaptureTargetCard::setThumbnailPlaceholder() {
    has_thumbnail_ = false;
    thumbnail_label_->setPixmap({});
    thumbnail_label_->setText(QString{});
    thumbnail_label_->setProperty("labelRole", "captureCardThumbnailPlaceholder");
    restyle(thumbnail_label_);
}

bool CaptureTargetCard::hasThumbnail() const noexcept {
    return has_thumbnail_;
}

void CaptureTargetCard::setUnavailable(bool unavailable) {
    unavailable_ = unavailable;
    setEnabled(!unavailable);
    setProperty("unavailable", unavailable);
    restyle(this);
}

bool CaptureTargetCard::isUnavailable() const noexcept {
    return unavailable_;
}

void CaptureTargetCard::setHelpText(const QString& text) {
    help_label_->setText(text);
    help_label_->setVisible(!text.isEmpty());
}

void CaptureTargetCard::updateStatusLabel() {
    if (!status_label_) {
        return;
    }
    const QString badge = status_text_.trimmed();
    if (selected_) {
        status_label_->setText(badge.isEmpty() ? QStringLiteral("● ACTIVE") : QStringLiteral("● %1").arg(badge));
    } else {
        status_label_->setText(badge.isEmpty() ? QStringLiteral("○") : badge);
    }
}

bool CaptureTargetCard::eventFilter(QObject* watched, QEvent* event) {
    const bool is_child = watched == title_label_ || watched == status_label_ || watched == subtitle_label_ ||
                          watched == thumbnail_label_ || watched == help_label_;
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

void CaptureTargetCard::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Space) {
        emit clicked();
        event->accept();
        return;
    }

    QFrame::keyPressEvent(event);
}

void CaptureTargetCard::mousePressEvent(QMouseEvent* event) {
    if (unavailable_) {
        event->ignore();
        return;
    }
    click_armed_ = (event->button() == Qt::LeftButton);
    QFrame::mousePressEvent(event);
}

void CaptureTargetCard::mouseReleaseEvent(QMouseEvent* event) {
    if (unavailable_) {
        event->ignore();
        return;
    }
    if (click_armed_ && event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        emit clicked();
    }
    click_armed_ = false;
    QFrame::mouseReleaseEvent(event);
}

} // namespace exosnap::ui::widgets
