#include "CaptureTargetCard.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

namespace exosnap::ui::widgets {
namespace {

void restyle(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

constexpr int kThumbnailWidth = 304;
constexpr int kThumbnailHeight = 171;

} // namespace

CaptureTargetCard::CaptureTargetCard(QWidget* parent) : QFrame(parent) {
    setObjectName("captureTargetCard");
    setProperty("selected", false);
    setProperty("captureCardUnavailable", false);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    thumbnail_surface_ = new QFrame(this);
    thumbnail_surface_->setObjectName("captureCardThumbnailSurface");
    thumbnail_surface_->setFixedSize(kThumbnailWidth, kThumbnailHeight);
    auto* thumbnail_stack = new QGridLayout(thumbnail_surface_);
    thumbnail_stack->setContentsMargins(0, 0, 0, 0);
    thumbnail_stack->setSpacing(0);

    thumbnail_label_ = new QLabel(thumbnail_surface_);
    thumbnail_label_->setObjectName("captureCardThumbnail");
    thumbnail_label_->setFixedSize(kThumbnailWidth, kThumbnailHeight);
    thumbnail_label_->setAlignment(Qt::AlignCenter);
    thumbnail_label_->setProperty("labelRole", "captureCardThumbnail");
    thumbnail_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    thumbnail_label_->installEventFilter(this);
    thumbnail_stack->addWidget(thumbnail_label_, 0, 0);

    thumbnail_state_label_ = new QLabel(thumbnail_surface_);
    thumbnail_state_label_->setProperty("labelRole", "captureCardThumbnailState");
    thumbnail_state_label_->setAlignment(Qt::AlignCenter);
    thumbnail_state_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    thumbnail_state_label_->setWordWrap(true);
    thumbnail_state_label_->setContentsMargins(10, 0, 10, 0);
    thumbnail_state_label_->installEventFilter(this);
    thumbnail_stack->addWidget(thumbnail_state_label_, 0, 0);
    root->addWidget(thumbnail_surface_, 0, Qt::AlignCenter);

    auto* text_col = new QVBoxLayout();
    text_col->setContentsMargins(0, 0, 0, 0);
    text_col->setSpacing(4);

    auto* top_row = new QHBoxLayout();
    top_row->setContentsMargins(0, 0, 0, 0);
    top_row->setSpacing(8);

    title_label_ = new QLabel(this);
    title_label_->setProperty("labelRole", "captureCardTitle");
    title_text_ = QStringLiteral("Monitor");
    title_label_->setText(title_text_);
    title_label_->setWordWrap(false);
    title_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    title_label_->installEventFilter(this);

    selected_chip_label_ = new QLabel(this);
    selected_chip_label_->setProperty("labelRole", "captureCardSelectedChip");
    selected_chip_label_->setText(QStringLiteral("Selected"));
    selected_chip_label_->setVisible(false);
    selected_chip_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    selected_chip_label_->installEventFilter(this);

    status_label_ = new QLabel(this);
    status_label_->setProperty("labelRole", "captureCardStatus");
    status_label_->setText(QStringLiteral("Screen"));
    status_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    status_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    status_label_->installEventFilter(this);

    top_row->addWidget(title_label_);
    top_row->addStretch(1);
    top_row->addWidget(selected_chip_label_);
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
    root->addLayout(text_col);

    setThumbnailPlaceholder();

    updateStatusLabel();
    updateTitleLabel();
}

void CaptureTargetCard::setTitle(const QString& title) {
    title_text_ = title;
    updateTitleLabel();
    setToolTip(title_text_);
}

QString CaptureTargetCard::title() const {
    return title_text_;
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
    if (selected_chip_label_) {
        selected_chip_label_->setVisible(selected_);
    }
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
    setThumbnailState(ThumbnailState::Ready, {});
}

void CaptureTargetCard::setThumbnailPlaceholder() {
    setThumbnailLoadingText(QStringLiteral("Loading preview..."));
}

void CaptureTargetCard::setThumbnailLoadingText(const QString& text) {
    setThumbnailState(ThumbnailState::Loading, text);
}

void CaptureTargetCard::setThumbnailFailureText(const QString& text) {
    setThumbnailState(ThumbnailState::Failed, text);
}

void CaptureTargetCard::setThumbnailUnavailableText(const QString& text) {
    setThumbnailState(ThumbnailState::Unavailable, text);
}

void CaptureTargetCard::setThumbnailState(ThumbnailState state, const QString& text) {
    has_thumbnail_ = false;
    if (state != ThumbnailState::Ready) {
        thumbnail_label_->setPixmap({});
    }

    if (!thumbnail_surface_ || !thumbnail_state_label_) {
        return;
    }

    switch (state) {
    case ThumbnailState::Loading:
        thumbnail_surface_->setProperty("thumbnailState", "loading");
        thumbnail_state_label_->setText(text.trimmed().isEmpty() ? QStringLiteral("Loading preview...") : text);
        thumbnail_state_label_->setVisible(true);
        break;
    case ThumbnailState::Ready:
        thumbnail_surface_->setProperty("thumbnailState", "ready");
        thumbnail_state_label_->clear();
        thumbnail_state_label_->setVisible(false);
        has_thumbnail_ = true;
        break;
    case ThumbnailState::Failed:
        thumbnail_surface_->setProperty("thumbnailState", "failed");
        thumbnail_state_label_->setText(text.trimmed().isEmpty() ? QStringLiteral("Preview unavailable") : text);
        thumbnail_state_label_->setVisible(true);
        break;
    case ThumbnailState::Unavailable:
        thumbnail_surface_->setProperty("thumbnailState", "unavailable");
        thumbnail_state_label_->setText(text.trimmed().isEmpty() ? QStringLiteral("Source unavailable") : text);
        thumbnail_state_label_->setVisible(true);
        break;
    }

    restyle(thumbnail_label_);
    restyle(thumbnail_surface_);
    restyle(thumbnail_state_label_);
}

bool CaptureTargetCard::hasThumbnail() const noexcept {
    return has_thumbnail_;
}

void CaptureTargetCard::setUnavailable(bool unavailable) {
    unavailable_ = unavailable;
    setEnabled(!unavailable);
    setProperty("unavailable", unavailable);
    setProperty("captureCardUnavailable", unavailable);
    if (unavailable) {
        setCursor(Qt::ArrowCursor);
    } else {
        setCursor(Qt::PointingHandCursor);
    }
    restyle(this);
}

bool CaptureTargetCard::isUnavailable() const noexcept {
    return unavailable_;
}

void CaptureTargetCard::setHelpText(const QString& text) {
    help_label_->setText(text);
    help_label_->setVisible(!text.isEmpty());
}

void CaptureTargetCard::updateTitleLabel() {
    if (!title_label_) {
        return;
    }
    const int available_width = std::max(48, title_label_->width());
    const QString elided = title_label_->fontMetrics().elidedText(title_text_, Qt::ElideRight, available_width);
    title_label_->setText(elided);
}

void CaptureTargetCard::updateStatusLabel() {
    if (!status_label_) {
        return;
    }
    const QString badge = status_text_.trimmed();
    if (selected_) {
        status_label_->setText(badge.isEmpty() ? QStringLiteral("ACTIVE") : badge);
    } else {
        status_label_->setText(badge.isEmpty() ? QStringLiteral("Source") : badge);
    }
}

bool CaptureTargetCard::eventFilter(QObject* watched, QEvent* event) {
    const bool is_child = watched == title_label_ || watched == status_label_ || watched == subtitle_label_ ||
                          watched == thumbnail_label_ || watched == help_label_ || watched == thumbnail_state_label_ ||
                          watched == selected_chip_label_;
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

void CaptureTargetCard::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    updateTitleLabel();
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
