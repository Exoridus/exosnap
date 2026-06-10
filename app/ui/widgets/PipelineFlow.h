#pragma once

#include <QStringList>
#include <QVector>
#include <QWidget>

#include "PipelineStepCard.h"

namespace exosnap::ui::widgets {

// Horizontal capture-pipeline flow for the Diagnostics page:
//
//   Source Capture -> Frame Queue -> Compositor -> Encoder -> Muxer -> Disk
//
// Cards are connected by arrow glyphs. The step order is fixed and canonical;
// callers update each step's status/note via setStepStatus(). The flow holds no
// telemetry of its own — it is a presentation surface for honest, per-stage
// readiness (real static checks where a probe exists; Planned otherwise).
class PipelineFlow : public QWidget {
    Q_OBJECT
  public:
    explicit PipelineFlow(QWidget* parent = nullptr);

    // Canonical capture-pipeline step order (left to right).
    static QStringList canonicalStepNames();

    int stepCount() const noexcept;
    PipelineStepCard* card(int index) const;

    void setStepStatus(int index, PipelineStepCard::Status status, const QString& note);

  private:
    QVector<PipelineStepCard*> cards_;
};

} // namespace exosnap::ui::widgets
