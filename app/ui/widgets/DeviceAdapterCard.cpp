#include "DeviceAdapterCard.h"

#include "../theme/ExoSnapPalette.h"
#include "../theme/LucideIcon.h"

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

constexpr int kAvatarSize = 38;

} // namespace

DeviceAdapterCard::DeviceAdapterCard(QWidget* parent) : QFrame(parent) {
    setObjectName("deviceAdapterCard");
    setProperty("selected", false);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(13);

    avatar_icon_ = new QLabel(this);
    avatar_icon_->setObjectName("deviceAdapterCardAvatar");
    avatar_icon_->setFixedSize(kAvatarSize, kAvatarSize);
    avatar_icon_->setAlignment(Qt::AlignCenter);
    avatar_icon_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    root->addWidget(avatar_icon_, 0, Qt::AlignVCenter);

    auto* text_col = new QVBoxLayout();
    text_col->setContentsMargins(0, 0, 0, 0);
    text_col->setSpacing(3);

    auto* title_row = new QHBoxLayout();
    title_row->setContentsMargins(0, 0, 0, 0);
    title_row->setSpacing(8);

    title_label_ = new QLabel(this);
    title_label_->setProperty("labelRole", "deviceAdapterCardTitle");
    title_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    title_label_->setWordWrap(false);

    kind_badge_ = new QLabel(this);
    kind_badge_->setProperty("labelRole", "deviceAdapterCardKind");
    kind_badge_->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    title_row->addWidget(title_label_, 1);
    title_row->addWidget(kind_badge_, 0, Qt::AlignVCenter);

    backend_label_ = new QLabel(this);
    backend_label_->setProperty("labelRole", "deviceAdapterCardBackend");
    backend_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    text_col->addLayout(title_row);
    text_col->addWidget(backend_label_);
    root->addLayout(text_col, 1);

    active_badge_ = new QLabel(QStringLiteral("Active"), this);
    active_badge_->setProperty("labelRole", "deviceAdapterCardActiveBadge");
    active_badge_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    active_badge_->setVisible(false);
    root->addWidget(active_badge_, 0, Qt::AlignVCenter);

    updateAvatarTint();
}

void DeviceAdapterCard::setTitle(const QString& title) {
    title_text_ = title;
    title_label_->setText(title);
    setToolTip(title);
    setAccessibleName(title);
}

const QString& DeviceAdapterCard::title() const {
    return title_text_;
}

void DeviceAdapterCard::setKindBadge(const QString& text) {
    kind_badge_->setText(text);
    kind_badge_->setVisible(!text.isEmpty());
}

void DeviceAdapterCard::setBackendLine(const QString& text) {
    backend_label_->setText(text);
    backend_label_->setVisible(!text.isEmpty());
}

void DeviceAdapterCard::setSelected(bool selected) {
    if (selected_ == selected)
        return;
    selected_ = selected;
    setProperty("selected", selected_);
    updateAvatarTint();
    restyle(this);
}

bool DeviceAdapterCard::isSelected() const noexcept {
    return selected_;
}

void DeviceAdapterCard::setActive(bool active) {
    active_ = active;
    if (active_badge_)
        active_badge_->setVisible(active_);
}

bool DeviceAdapterCard::isActive() const noexcept {
    return active_;
}

void DeviceAdapterCard::updateAvatarTint() {
    if (!avatar_icon_)
        return;
    using Pal = ui::theme::ExoSnapPalette;
    const QString color = selected_ ? QString::fromUtf8(Pal::kAccent) : QString::fromUtf8(Pal::kText2);
    const qreal dpr = avatar_icon_->devicePixelRatioF();
    avatar_icon_->setPixmap(ui::theme::lucidePixmap(QStringLiteral("cpu"), color, 19, dpr));
}

void DeviceAdapterCard::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Space) {
        emit clicked();
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

void DeviceAdapterCard::mousePressEvent(QMouseEvent* event) {
    click_armed_ = (event->button() == Qt::LeftButton);
    QFrame::mousePressEvent(event);
}

void DeviceAdapterCard::mouseReleaseEvent(QMouseEvent* event) {
    if (click_armed_ && event->button() == Qt::LeftButton && rect().contains(event->pos()))
        emit clicked();
    click_armed_ = false;
    QFrame::mouseReleaseEvent(event);
}

} // namespace exosnap::ui::widgets
