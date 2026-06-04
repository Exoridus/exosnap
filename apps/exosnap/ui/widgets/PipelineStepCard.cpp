#include "PipelineStepCard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QStyle>
#include <QVBoxLayout>

namespace exosnap::ui::widgets {
namespace {

const char* StatusKey(PipelineStepCard::Status status) {
    switch (status) {
    case PipelineStepCard::Status::Ok:
        return "ok";
    case PipelineStepCard::Status::Hotspot:
        return "hotspot";
    case PipelineStepCard::Status::Over:
        return "over";
    case PipelineStepCard::Status::Planned:
        return "planned";
    case PipelineStepCard::Status::Unavailable:
        return "unavailable";
    }
    return "planned";
}

QString StatusLabel(PipelineStepCard::Status status) {
    switch (status) {
    case PipelineStepCard::Status::Ok:
        return QStringLiteral("OK");
    case PipelineStepCard::Status::Hotspot:
        return QStringLiteral("Hotspot");
    case PipelineStepCard::Status::Over:
        return QStringLiteral("Over");
    case PipelineStepCard::Status::Planned:
        return QStringLiteral("Planned");
    case PipelineStepCard::Status::Unavailable:
        return QStringLiteral("Unavailable");
    }
    return QStringLiteral("Planned");
}

void Restyle(QWidget* widget) {
    if (!widget)
        return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

} // namespace

PipelineStepCard::PipelineStepCard(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("pipelineStepCard"));
    setProperty("pipelineStatus", QString::fromLatin1(StatusKey(status_)));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(13, 12, 13, 12);
    root->setSpacing(9);

    name_label_ = new QLabel(this);
    name_label_->setProperty("labelRole", "pipelineStepName");
    name_label_->setWordWrap(true);
    root->addWidget(name_label_);

    note_label_ = new QLabel(this);
    note_label_->setProperty("labelRole", "pipelineStepNote");
    note_label_->setWordWrap(true);
    root->addWidget(note_label_, 1);

    status_label_ = new QLabel(StatusLabel(status_), this);
    status_label_->setObjectName(QStringLiteral("pipelineStepStatus"));
    status_label_->setProperty("labelRole", "pipelineStepStatus");
    status_label_->setProperty("pipelineStatus", QString::fromLatin1(StatusKey(status_)));
    status_label_->setAlignment(Qt::AlignCenter);
    status_label_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    auto* pill_row = new QHBoxLayout();
    pill_row->setContentsMargins(0, 0, 0, 0);
    pill_row->setSpacing(0);
    pill_row->addWidget(status_label_, 0, Qt::AlignLeft);
    pill_row->addStretch(1);
    root->addLayout(pill_row);
}

void PipelineStepCard::setStepName(const QString& name) {
    step_name_ = name;
    name_label_->setText(name);
    setAccessibleName(name);
}

QString PipelineStepCard::stepName() const {
    return step_name_;
}

void PipelineStepCard::setStatus(Status status) {
    status_ = status;
    applyStatus();
}

PipelineStepCard::Status PipelineStepCard::status() const noexcept {
    return status_;
}

void PipelineStepCard::setNote(const QString& note) {
    if (note_label_)
        note_label_->setText(note);
}

QString PipelineStepCard::note() const {
    return note_label_ ? note_label_->text() : QString{};
}

QString PipelineStepCard::statusText() const {
    return status_label_ ? status_label_->text() : QString{};
}

void PipelineStepCard::applyStatus() {
    const QString key = QString::fromLatin1(StatusKey(status_));
    setProperty("pipelineStatus", key);
    if (status_label_) {
        status_label_->setText(StatusLabel(status_));
        status_label_->setProperty("pipelineStatus", key);
        Restyle(status_label_);
    }
    Restyle(this);
}

} // namespace exosnap::ui::widgets
