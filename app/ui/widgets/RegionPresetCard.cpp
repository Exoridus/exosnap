#include "RegionPresetCard.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace exosnap::ui::widgets {
namespace {

constexpr int kPreviewBoxHeight = 66;
constexpr int kPreviewMaxWidth = 98;
constexpr int kPreviewMaxHeight = 52;

void restyle(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

} // namespace

RegionPresetCard::RegionPresetCard(QWidget* parent) : QFrame(parent) {
    setObjectName("regionPresetCard");
    setProperty("selected", false);
    setProperty("regionPresetPlanned", false);
    setProperty("regionPresetDraw", false);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(9);

    preview_box_ = new QFrame(this);
    preview_box_->setObjectName("regionPresetPreviewBox");
    preview_box_->setFixedHeight(kPreviewBoxHeight);
    preview_box_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto* preview_layout = new QHBoxLayout(preview_box_);
    preview_layout->setContentsMargins(0, 0, 0, 0);
    preview_layout->setSpacing(0);

    preview_shape_ = new QFrame(preview_box_);
    preview_shape_->setObjectName("regionPresetPreviewShape");
    preview_shape_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    preview_layout->addStretch(1);
    preview_layout->addWidget(preview_shape_, 0, Qt::AlignCenter);
    preview_layout->addStretch(1);

    // Accent check badge — floats over the top-right of the preview while
    // selected (repositioned in resizeEvent since the card width is dynamic).
    check_badge_ = new QLabel(QStringLiteral("✓"), preview_box_);
    check_badge_->setObjectName("regionPresetCheckBadge");
    check_badge_->setProperty("labelRole", "regionPresetCheckBadge");
    check_badge_->setAlignment(Qt::AlignCenter);
    check_badge_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    check_badge_->setFixedSize(20, 20);
    check_badge_->setVisible(false);

    auto* title_row = new QHBoxLayout();
    title_row->setContentsMargins(0, 0, 0, 0);
    title_row->setSpacing(8);

    title_label_ = new QLabel(this);
    title_label_->setProperty("labelRole", "regionPresetTitle");
    title_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    planned_badge_ = new QLabel(QStringLiteral("Planned"), this);
    planned_badge_->setObjectName("regionPresetPlannedBadge");
    planned_badge_->setProperty("labelRole", "regionPresetPlannedBadge");
    planned_badge_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    planned_badge_->setVisible(false);

    title_row->addWidget(title_label_, 1);
    title_row->addStretch(0);
    title_row->addWidget(planned_badge_, 0, Qt::AlignVCenter);

    detail_label_ = new QLabel(this);
    detail_label_->setProperty("labelRole", "regionPresetDetail");
    detail_label_->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    root->addWidget(preview_box_);
    root->addLayout(title_row);
    root->addWidget(detail_label_);

    updatePreviewGeometry();
}

void RegionPresetCard::setTitle(const QString& title) {
    title_text_ = title;
    title_label_->setText(title);
    setAccessibleName(title);
    setToolTip(title);
}

const QString& RegionPresetCard::title() const {
    return title_text_;
}

void RegionPresetCard::setDetail(const QString& detail) {
    detail_label_->setText(detail);
}

QString RegionPresetCard::detail() const {
    return detail_label_ ? detail_label_->text() : QString{};
}

void RegionPresetCard::setAspectRatio(double aspect) {
    aspect_ = aspect > 0.0 ? aspect : (16.0 / 9.0);
    updatePreviewGeometry();
}

void RegionPresetCard::setDrawVariant(bool draw) {
    draw_variant_ = draw;
    setProperty("regionPresetDraw", draw);
    if (preview_shape_) {
        restyle(preview_shape_);
    }
    restyle(this);
}

bool RegionPresetCard::isDrawVariant() const noexcept {
    return draw_variant_;
}

void RegionPresetCard::setPlanned(bool planned) {
    planned_ = planned;
    setProperty("regionPresetPlanned", planned);
    if (planned_badge_) {
        planned_badge_->setVisible(planned);
    }
    setCursor(planned ? Qt::ArrowCursor : Qt::PointingHandCursor);
    setFocusPolicy(planned ? Qt::NoFocus : Qt::StrongFocus);
    restyle(this);
}

bool RegionPresetCard::isPlanned() const noexcept {
    return planned_;
}

void RegionPresetCard::setSelected(bool selected) {
    if (selected_ == selected) {
        return;
    }
    selected_ = selected;
    setProperty("selected", selected_);
    if (preview_shape_) {
        restyle(preview_shape_);
    }
    updateCheckBadge();
    restyle(this);
}

bool RegionPresetCard::isSelected() const noexcept {
    return selected_;
}

void RegionPresetCard::updateCheckBadge() {
    if (!check_badge_ || !preview_box_) {
        return;
    }
    check_badge_->setVisible(selected_);
    if (!selected_) {
        return;
    }
    const int margin = 8;
    check_badge_->move(preview_box_->width() - check_badge_->width() - margin, margin);
    check_badge_->raise();
}

void RegionPresetCard::updatePreviewGeometry() {
    if (!preview_shape_) {
        return;
    }
    int width = kPreviewMaxWidth;
    int height = kPreviewMaxHeight;
    if (aspect_ >= 1.0) {
        width = kPreviewMaxWidth;
        height = static_cast<int>(std::lround(width / aspect_));
        if (height > kPreviewMaxHeight) {
            height = kPreviewMaxHeight;
            width = static_cast<int>(std::lround(height * aspect_));
        }
    } else {
        height = kPreviewMaxHeight;
        width = static_cast<int>(std::lround(height * aspect_));
        if (width > kPreviewMaxWidth) {
            width = kPreviewMaxWidth;
            height = static_cast<int>(std::lround(width / aspect_));
        }
    }
    preview_shape_->setFixedSize(std::max(14, width), std::max(14, height));
}

void RegionPresetCard::keyPressEvent(QKeyEvent* event) {
    if (!planned_ && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Space)) {
        emit clicked();
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

void RegionPresetCard::mousePressEvent(QMouseEvent* event) {
    if (planned_) {
        event->ignore();
        return;
    }
    click_armed_ = (event->button() == Qt::LeftButton);
    QFrame::mousePressEvent(event);
}

void RegionPresetCard::mouseReleaseEvent(QMouseEvent* event) {
    if (planned_) {
        event->ignore();
        return;
    }
    if (click_armed_ && event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        emit clicked();
    }
    click_armed_ = false;
    QFrame::mouseReleaseEvent(event);
}

void RegionPresetCard::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    updateCheckBadge();
}

} // namespace exosnap::ui::widgets
