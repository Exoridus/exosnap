#pragma once
#include <QFrame>
#include <QString>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QWidget;

namespace exosnap::ui::widgets {

// Export panel of the edit surface: an embedded card in the right rail, under
// the Details card — not a modal (see
// docs/superpowers/specs/2026-08-03-edit-export-panel-responsive-design.md).
//
// Two combo boxes and a static destination line never justified a card over the
// view, and the rail had the vertical room standing empty. The panel carries the
// output choice *and* the progress/result of the run, so an export never hides
// the clip it was started from.
//
// Presentation only: the export itself — thread, trim range, marker sidecar,
// atomic rename — stays in EditExportPage, which drives this panel through the
// show*/setProgress slots and reacts to its signals. The run is started from the
// surface's own action bar, so the panel deliberately carries no Export button
// of its own; `Retry` after a failure is the one exception, and it repeats a run
// rather than starting a fresh one.
//
//   Options --(action bar)--> Running --+-- ok --> Done
//                                       +-- err -> Failed --Retry--> Running
//
// The output rows stay in place across all four states (disabled while a run is
// in flight), so the panel never swaps its content out from under the pointer.
class ExportPanel : public QFrame {
    Q_OBJECT
  public:
    enum class State { Options, Running, Done, Failed };

    explicit ExportPanel(QWidget* parent = nullptr);

    [[nodiscard]] State state() const noexcept;

    // Selections, read by the page when it starts an export.
    [[nodiscard]] QString containerKey() const; // "mkv" | "mp4"
    [[nodiscard]] QString saveModeKey() const;  // "new" | "overwrite"

    // Back to Options with the status area cleared. Used when a run is cancelled
    // and when the surface is handed a different clip.
    void reset();

    // Driven by the page while an export runs.
    void showRunning();
    void setProgress(int percent);
    void showDone(const QString& output_path);
    void showFailed(const QString& error_message);

    void applyThemeStyles();

  signals:
    // Abort the running export. Only reachable from Running.
    void cancelRequested();
    void retryRequested();
    void openFolderRequested();
    void revealFileRequested();

    // The panel now has progress or a result to show. The host owns what that
    // means for the surface around it — at the minimum window height the rail
    // has to scroll the status into view, and a report the user has to go
    // looking for is not a report.
    void statusShown();

  private:
    void buildPanel();
    // Shows the status group for the current state and enables/disables the
    // output rows (a run in flight must not have its target changed under it).
    void refreshStateVisibility();
    // The destination line states what the selected save mode actually does.
    void refreshDestinationText();

    State state_ = State::Options;

    QLabel* title_label_ = nullptr;

    // ---- Output rows (present in every state) ----
    QWidget* options_content_ = nullptr;
    QLabel* container_label_ = nullptr;
    QComboBox* container_combo_ = nullptr;
    QLabel* save_mode_label_ = nullptr;
    QComboBox* save_mode_combo_ = nullptr;
    QLabel* dest_label_ = nullptr;

    // ---- Status area (hidden in Options) ----
    QFrame* status_separator_ = nullptr;

    QWidget* running_content_ = nullptr;
    QLabel* status_label_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QPushButton* cancel_btn_ = nullptr;

    // Done and Failed share one layout shape; the badge and colours follow the
    // state.
    QWidget* result_content_ = nullptr;
    QLabel* result_icon_label_ = nullptr;
    QLabel* result_title_label_ = nullptr;
    QLabel* result_detail_label_ = nullptr;
    QPushButton* open_folder_btn_ = nullptr;
    QPushButton* reveal_btn_ = nullptr;
    QPushButton* retry_btn_ = nullptr;
};

} // namespace exosnap::ui::widgets
