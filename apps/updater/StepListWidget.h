#pragma once

// StepListWidget -- the five-row swap checklist inside a rounded card. Each row
// shows a status glyph (check / spinner / queued dot / cross), a label and a
// right-aligned mono tag (done | working | queued | failed). The failed-row
// tint follows the window's terminal variant (amber vs red), set via
// setFailColor() before setSteps().
//
// On the Green terminal variant a Failed step (the auto-relaunch) is not a
// real error -- the update itself succeeded, only the automatic relaunch
// didn't -- so callers pass failedIsManual=true to setSteps() and the row
// renders as a "manual" affordance (hollow ring, "manual" tag) instead of a
// cross/"failed". This is display-only; UpdaterController's StepStatus stays
// Failed either way.

#include <QColor>
#include <QWidget>
#include <array>

#include "UpdaterController.h" // StepStatus

class StepRow; // internal row widget (defined in the .cpp)

class StepListWidget : public QWidget {
    Q_OBJECT
  public:
    explicit StepListWidget(QWidget* parent = nullptr);

    void setSteps(const std::array<StepStatus, 5>& steps, bool failedIsManual = false);
    void setFailColor(const QColor& color);

    // The five fixed canon labels, top to bottom.
    static const std::array<QString, 5>& labels();

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    std::array<StepRow*, 5> rows_{};
    QColor fail_color_;
};
