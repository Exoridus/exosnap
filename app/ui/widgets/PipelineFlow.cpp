#include "PipelineFlow.h"

#include <QHBoxLayout>
#include <QLabel>

namespace exosnap::ui::widgets {

QStringList PipelineFlow::canonicalStepNames() {
    return {QStringLiteral("Source Capture"), QStringLiteral("Frame Queue"), QStringLiteral("Compositor"),
            QStringLiteral("Encoder"),        QStringLiteral("Muxer"),       QStringLiteral("Disk")};
}

PipelineFlow::PipelineFlow(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("pipelineFlow"));

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    const QStringList names = canonicalStepNames();
    for (int i = 0; i < names.size(); ++i) {
        auto* step = new PipelineStepCard(this);
        step->setStepName(names.at(i));
        cards_.append(step);
        row->addWidget(step, 1);

        if (i + 1 < names.size()) {
            // Flow connector between steps (→).
            auto* arrow = new QLabel(QString::fromUtf8("\xE2\x86\x92"), this);
            arrow->setObjectName(QStringLiteral("pipelineArrow"));
            arrow->setProperty("labelRole", "pipelineArrow");
            arrow->setAlignment(Qt::AlignCenter);
            row->addWidget(arrow, 0, Qt::AlignVCenter);
        }
    }
}

int PipelineFlow::stepCount() const noexcept {
    return static_cast<int>(cards_.size());
}

PipelineStepCard* PipelineFlow::card(int index) const {
    return (index >= 0 && index < cards_.size()) ? cards_.at(index) : nullptr;
}

void PipelineFlow::setStepStatus(int index, PipelineStepCard::Status status, const QString& note) {
    if (auto* step = card(index)) {
        step->setStatus(status);
        step->setNote(note);
    }
}

void PipelineFlow::setStepLive(int index, PipelineStepCard::Status status, const QString& note,
                               const QString& resourceTag, const QString& secondaryNumber, const QString& tooltip) {
    if (auto* step = card(index)) {
        step->setStatus(status);
        step->setNote(note);
        step->setResourceTag(resourceTag);
        step->setSecondaryNumber(secondaryNumber);
        step->setToolTip(tooltip);
    }
}

} // namespace exosnap::ui::widgets
