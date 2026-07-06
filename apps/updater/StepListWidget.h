#pragma once

// StepListWidget -- the five-row swap checklist inside a rounded card. Each row
// shows a status glyph (check / spinner / queued dot / cross), a label and a
// right-aligned mono tag (done | working | queued | failed). The failed-row
// tint follows the window's terminal variant (amber vs red), set via
// setFailColor() before setSteps().

#include <QColor>
#include <QWidget>
#include <array>

#include "UpdaterController.h" // StepStatus

class StepRow; // internal row widget (defined in the .cpp)

class StepListWidget : public QWidget {
    Q_OBJECT
  public:
    explicit StepListWidget(QWidget* parent = nullptr);

    void setSteps(const std::array<StepStatus, 5>& steps);
    void setFailColor(const QColor& color);

    // The five fixed canon labels, top to bottom.
    static const std::array<QString, 5>& labels();

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    std::array<StepRow*, 5> rows_{};
    QColor fail_color_;
};
