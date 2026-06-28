#pragma once

#include <QFrame>
#include <QString>

class QLabel;

namespace exosnap::ui::widgets {

// A single capture-pipeline step card for the Diagnostics pipeline view.
//
// The card shows the step name, a status pill and a short note. In this slice
// no live per-frame telemetry exists, so the card never renders fabricated
// numeric latency / queue-depth / throughput values:
//   * steps backed by a real capability probe show Ok / Unavailable
//     (an honest static availability check), and
//   * internal stages with no probe show Planned.
//
// The status vocabulary mirrors the Hybrid v3 design (OK / Hotspot / Over /
// Planned / Unavailable). Hotspot / Over are reserved for future live-timing
// data and are not produced by the current static checks.
class PipelineStepCard : public QFrame {
    Q_OBJECT
  public:
    enum class Status { Ok, Hotspot, Over, Planned, Unavailable };

    explicit PipelineStepCard(QWidget* parent = nullptr);

    void setStepName(const QString& name);
    const QString& stepName() const;

    void setStatus(Status status);
    Status status() const noexcept;

    void setNote(const QString& note);
    QString note() const;

    void setResourceTag(const QString& tag);
    QString resourceTag() const;

    void setSecondaryNumber(const QString& number);
    QString secondaryNumber() const;

    // The honest status pill label ("OK" / "Hotspot" / "Over" / "Planned" / "Unavailable").
    QString statusText() const;

  private:
    void applyStatus();

    QLabel* name_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* note_label_ = nullptr;
    QLabel* resource_label_ = nullptr;
    QLabel* number_label_ = nullptr;
    Status status_ = Status::Planned;
    QString step_name_;
};

} // namespace exosnap::ui::widgets
