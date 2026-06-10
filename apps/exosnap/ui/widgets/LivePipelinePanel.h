#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

#include <recorder_core/pipeline_diagnostics.h>

class QLabel;
class QVBoxLayout;

namespace exosnap::ui::widgets {

// Live recording-pipeline telemetry panel for the Diagnostics page. Renders the
// canonical RecordingDiagnosticsSnapshot in compact sections. Unavailable metrics are
// shown as "Unavailable"/"—" — never as a fabricated zero. Idle/Initializing states show
// neutral values rather than stale active-looking numbers.
//
// Each value label carries a stable objectName (the row key) so tests and the visual
// manifest can read rendered state via findChild.
class LivePipelinePanel : public QWidget {
    Q_OBJECT
  public:
    explicit LivePipelinePanel(QWidget* parent = nullptr);

    // Update from a live snapshot of any lifecycle.
    void applySnapshot(const recorder_core::RecordingDiagnosticsSnapshot& snapshot);

    // Neutral "no active recording" state.
    void setIdle();

  private:
    QWidget* addSection(QVBoxLayout* parent_layout, const QString& title);
    void addRow(QWidget* section, const QString& key, const QString& caption);
    void setValue(const QString& key, const QString& text);
    void setNeutral(const QString& lifecycle_text);

    QLabel* status_pill_ = nullptr;
    QLabel* lifecycle_label_ = nullptr;
    QHash<QString, QLabel*> values_;
};

} // namespace exosnap::ui::widgets
