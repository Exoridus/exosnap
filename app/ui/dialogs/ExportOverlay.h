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
    // Abort the running export. Only ever emitted from Running — in every other
    // state the same button dismisses the card via closeRequested().
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
    void buildCard();
    void syncGeometryToParent();
    // Shows the content group for the current state and relabels/repositions
    // the shared button row (Options/Failed share the primary button; every
    // state but Done shares the cancel-or-close button).
    void refreshStateVisibility();

    State state_ = State::Options;

    QFrame* card_ = nullptr;

    // ---- Options content ----
    QWidget* options_content_ = nullptr;
    QLabel* options_title_label_ = nullptr;
    QLabel* container_label_ = nullptr;
    QComboBox* container_combo_ = nullptr;
    QLabel* save_mode_label_ = nullptr;
    QComboBox* save_mode_combo_ = nullptr;
    QLabel* dest_title_label_ = nullptr;
    QLabel* dest_folder_label_ = nullptr;

    // ---- Running content ----
    QWidget* running_content_ = nullptr;
    QLabel* status_label_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;

    // ---- Result content (Done and Failed share the same layout shape) ----
    QWidget* result_content_ = nullptr;
    QLabel* result_icon_label_ = nullptr;
    QLabel* result_title_label_ = nullptr;
    QLabel* result_detail_label_ = nullptr;

    // ---- Shared button row ----
    // exportPrimaryBtn: "Export" in Options, "Retry" in Failed, hidden otherwise.
    // exportCancelBtn: "Cancel" in Options/Running, "Close" in Done/Failed —
    // always present, its meaning follows its current label.
    QPushButton* primary_btn_ = nullptr;
    QPushButton* cancel_btn_ = nullptr;
    QPushButton* open_folder_btn_ = nullptr;
    QPushButton* reveal_btn_ = nullptr;
};

} // namespace exosnap::ui::dialogs
