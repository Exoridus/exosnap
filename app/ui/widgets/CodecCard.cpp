#include "CodecCard.h"

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

CodecCard::CodecCard(const QString& name, const QString& tag, const QString& description, QWidget* parent)
    : QWidget(parent) {
    setObjectName("codecCard");
    setProperty("selected", false);
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMinimumHeight(116);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(6);

    auto* top_row = new QHBoxLayout();
    top_row->setContentsMargins(0, 0, 0, 0);
    top_row->setSpacing(8);

    name_label_ = new QLabel(name, this);
    name_label_->setProperty("labelRole", "codecCardName");

    tag_label_ = new QLabel(tag, this);
    tag_label_->setProperty("labelRole", "codecCardTag");
    tag_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    top_row->addWidget(name_label_);
    top_row->addStretch(1);
    top_row->addWidget(tag_label_);

    description_label_ = new QLabel(description, this);
    description_label_->setProperty("labelRole", "codecCardDesc");
    description_label_->setWordWrap(true);

    root->addLayout(top_row);
    root->addWidget(description_label_);
}

void CodecCard::setSelected(bool selected) {
    if (selected_ == selected)
        return;

    selected_ = selected;
    setProperty("selected", selected_);
    restyle(this);
}

bool CodecCard::isSelected() const noexcept {
    return selected_;
}

void CodecCard::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        emit clicked();
    QWidget::mousePressEvent(event);
}

} // namespace exosnap::ui::widgets
