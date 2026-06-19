#include "SettingsCardExpander.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

namespace exosnap::ui::widgets {

SettingsCardExpander::SettingsCardExpander(int option_count, QWidget* parent) : QFrame(parent) {
    setFrameShape(QFrame::NoFrame);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Header: "Advanced · N options" toggle button.
    header_btn_ = new QPushButton(this);
    header_btn_->setObjectName(QStringLiteral("settingsCardExpanderHeader"));
    header_btn_->setCheckable(false);
    header_btn_->setAutoDefault(false);
    header_btn_->setDefault(false);
    header_btn_->setCursor(Qt::PointingHandCursor);
    header_btn_->setProperty("role", "ghost");
    header_btn_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    const QString count_str = option_count > 0 ? QStringLiteral("Advanced · %1 option%2")
                                                     .arg(option_count)
                                                     .arg(option_count == 1 ? QStringLiteral("") : QStringLiteral("s"))
                                               : QStringLiteral("Advanced");
    header_btn_->setText(count_str);
    outer->addWidget(header_btn_, 0, Qt::AlignLeft);

    // Content container (hidden by default).
    content_widget_ = new QWidget(this);
    content_widget_->setObjectName(QStringLiteral("settingsCardExpanderContent"));
    auto* content_layout = new QVBoxLayout(content_widget_);
    content_layout->setContentsMargins(0, 8, 0, 0);
    content_layout->setSpacing(8);
    content_widget_->setVisible(false);
    outer->addWidget(content_widget_);

    connect(header_btn_, &QPushButton::clicked, this, &SettingsCardExpander::toggle);
}

QWidget* SettingsCardExpander::contentWidget() const noexcept {
    return content_widget_;
}

void SettingsCardExpander::setExpanded(bool expanded) {
    if (expanded_ == expanded)
        return;
    expanded_ = expanded;
    content_widget_->setVisible(expanded_);
    emit expandedChanged(expanded_);
}

bool SettingsCardExpander::isExpanded() const noexcept {
    return expanded_;
}

void SettingsCardExpander::toggle() {
    setExpanded(!expanded_);
}

} // namespace exosnap::ui::widgets
