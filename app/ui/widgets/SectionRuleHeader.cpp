#include "SectionRuleHeader.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>

namespace exosnap::ui::widgets {

SectionRuleHeader::SectionRuleHeader(const QString& title, QWidget* parent) : QWidget(parent) {
    setObjectName("sectionRuleHeader");

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    title_label_ = new QLabel(title, this);
    title_label_->setProperty("labelRole", "sectionRuleTitle");

    rule_line_ = new QFrame(this);
    rule_line_->setFrameShape(QFrame::HLine);
    rule_line_->setProperty("frameRole", "sectionRuleLine");
    rule_line_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    meta_label_ = new QLabel(this);
    meta_label_->setProperty("labelRole", "sectionRuleMeta");
    meta_label_->setVisible(false);

    layout->addWidget(title_label_);
    layout->addWidget(rule_line_, 1);
    layout->addWidget(meta_label_);
}

void SectionRuleHeader::setTitle(const QString& title) {
    title_label_->setText(title);
}

void SectionRuleHeader::setMeta(const QString& meta) {
    meta_label_->setText(meta);
    meta_label_->setVisible(!meta.trimmed().isEmpty());
}

void SectionRuleHeader::clearMeta() {
    meta_label_->clear();
    meta_label_->setVisible(false);
}

QString SectionRuleHeader::title() const {
    return title_label_->text();
}

QString SectionRuleHeader::meta() const {
    return meta_label_->text();
}

} // namespace exosnap::ui::widgets
