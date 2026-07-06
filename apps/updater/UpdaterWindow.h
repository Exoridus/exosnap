#pragma once

// UpdaterWindow -- the whole standalone updater window, rendered as a pure
// function of an UpdaterUiState. A dedicated (frameless-look) title bar carries
// the brand + a boxed UPDATER tag + a close X that refuses to interrupt a swap;
// below it sit the version pills, the ProgressRing, a status line, the
// five-step checklist and a state-dependent footer (reassurance while working,
// a tinted result card with 1-2 actions on a terminal variant).

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <array>

#include "UpdaterController.h" // UpdaterUiState / TerminalVariant / StepStatus

class QLabel;
class QPushButton;
class QVBoxLayout;
class ProgressRing;
class StepListWidget;

class UpdaterWindow : public QWidget {
    Q_OBJECT
  public:
    explicit UpdaterWindow(QWidget* parent = nullptr);

    // Single entry point: the window is a pure function of the state struct.
    void render(const UpdaterUiState& state);

    // The five fixed canon step labels (top to bottom).
    static std::array<QString, 5> stepLabels();

    // Test / introspection seams.
    bool closeEnabled() const;
    QStringList footerButtonLabels() const;

  signals:
    void retryRequested();
    void closeRequested();
    void openCurrentRequested();
    void openNewRequested();

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    void buildFooter(const UpdaterUiState& state);
    void emitForAction(const QString& action);

    // Header
    QPushButton* close_button_ = nullptr;

    // Version transition
    QLabel* from_pill_ = nullptr;
    QLabel* to_pill_ = nullptr;

    // Ring + status
    ProgressRing* ring_ = nullptr;
    QWidget* status_icon_ = nullptr;
    QLabel* status_text_ = nullptr;

    // Steps + footer
    StepListWidget* steps_ = nullptr;
    QWidget* footer_container_ = nullptr;
    QVBoxLayout* footer_layout_ = nullptr;
    QList<QPushButton*> footer_buttons_;
};
