#pragma once

// UpdaterWindow -- the whole standalone updater window, rendered as a pure
// function of an UpdaterUiState. Its compact title bar carries the same ExoSnap
// mark + wordmark identity and window-control geometry as the main application,
// followed by a stable Updater label. Progress and failure state stay in the
// content where they are explained. Below it sit the version pills, the
// ProgressRing, a status line, the five-step checklist, one fixed-height state
// panel and one fixed action row. Their geometry does not change between
// working, warning, error and success states.

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <array>

#include "UpdaterController.h" // UpdaterUiState / TerminalVariant / StepStatus

class QCloseEvent;
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
    bool cancelConfirmationVisible() const;

  signals:
    void retryRequested();
    void closeRequested();
    void openExoSnapRequested();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

  private:
    void buildFooter(const UpdaterUiState& state);
    void emitForAction(const QString& action);
    void requestClose();
    void showCancelConfirmation();
    void confirmCancelAndClose();

    // Header
    QPushButton* minimize_button_ = nullptr;
    QPushButton* close_button_ = nullptr;
    // Mirrors the same "Install/Verify/Launch working" gate as the in-window
    // close X (see render()), but also refuses WM_CLOSE arriving through any
    // other path -- Alt+F4, the taskbar close, Windows logoff -- since the
    // custom close button being disabled does not stop the OS from sending
    // WM_CLOSE via those routes.
    bool close_blocked_ = false;
    bool cancel_confirmation_required_ = false;
    QWidget* cancel_overlay_ = nullptr;

    // Version transition
    QLabel* from_pill_ = nullptr;
    QLabel* to_pill_ = nullptr;

    // Ring + status
    ProgressRing* ring_ = nullptr;
    QWidget* status_row_ = nullptr;
    QWidget* status_icon_ = nullptr;
    QLabel* status_text_ = nullptr;

    // Steps + footer
    StepListWidget* steps_ = nullptr;
    QWidget* footer_container_ = nullptr;
    QVBoxLayout* footer_layout_ = nullptr;
    QList<QPushButton*> footer_buttons_;
};
