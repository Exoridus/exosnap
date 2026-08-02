#pragma once
#include <QString>
#include <QWidget>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QFrame;

namespace exosnap::ui::dialogs {

// Export card shown over the edit view (see
// docs/superpowers/specs/2026-08-02-edit-surface-single-view-design.md).
//
// Presentation only: the export itself — thread, trim range, marker sidecar,
// atomic rename — stays in EditExportPage, which drives this card through the
// show*/setProgress slots and reacts to its signals.
//
// One card, four states:
//
//   Options --Export--> Running --+-- ok --> Done
//                                 +-- err -> Failed --Retry--> Running
//
// Escape and a backdrop click close the card (not the edit session) and are
// blocked while Running — Cancel is the only way out of a running export.
class ExportOverlay : public QWidget {
    Q_OBJECT
  public:
    enum class State { Options, Running, Done, Failed };

    explicit ExportOverlay(QWidget* parent = nullptr);

    void openCard();  // show in Options, progress reset to 0
    void closeCard(); // hide; ignored while Running

    [[nodiscard]] bool isCardOpen() const noexcept;
    [[nodiscard]] State state() const noexcept;

    // Selections, read by the page when it starts an export.
    [[nodiscard]] QString containerKey() const; // "mkv" | "mp4"
    [[nodiscard]] QString saveModeKey() const;  // "new" | "overwrite"

    // Driven by the page while an export runs.
    void showRunning();
    void setProgress(int percent);
    void showDone(const QString& output_path);
    void showFailed(const QString& error_message);

    void applyThemeStyles();

  signals:
    void exportRequested();
    void cancelRequested();
    void retryRequested();
    void openFolderRequested();
    void revealFileRequested();
    void closeRequested(); // Close button, Escape, or backdrop click

  protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    State state_ = State::Options;
};

} // namespace exosnap::ui::dialogs
