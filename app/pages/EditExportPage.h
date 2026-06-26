#pragma once
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QFrame;
class QProgressBar;
class QScrollArea;
class QEvent;
class QObject;

namespace exosnap {

// Edit/Export-Surface — Phase G (UI-Shell, Placeholder-Character).
// The real Trim/Export engine arrives in 0.11.
class EditExportPage : public QWidget {
    Q_OBJECT
  public:
    enum class Phase {
        Review,    // View, no editing
        Edit,      // Trim + Marker (disabled)
        Output,    // Format choice (disabled)
        Exporting, // Export running (stub)
        Done,      // Export complete
        Failed,    // Export failed
    };

    explicit EditExportPage(QWidget* parent = nullptr);

    void setRecordingInfo(const QString& file_path, const QString& duration, const QString& size,
                          const QString& resolution, const QString& fps, const QString& video_codec,
                          const QString& audio_codec, const QString& container);

    [[nodiscard]] Phase phase() const noexcept {
        return phase_;
    }
    void setPhase(Phase phase);

  signals:
    void backRequested();
    void exportCompleted(const QString& output_path);

  private slots:
    void onBackClicked();
    void onExportClicked();
    void onCancelExportClicked();
    void onDoneClicked();
    void onOpenFolderClicked();
    void onRevealFileClicked();
    void onRetryExportClicked();

  protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

  private:
    void buildUi();
    void refreshPhase();

    Phase phase_ = Phase::Review;

    QString file_path_;
    QString duration_;
    QString size_;
    QString resolution_;
    QString fps_;
    QString video_codec_;
    QString audio_codec_;
    QString container_;

    // Mode-Bar
    QPushButton* back_btn_ = nullptr;
    QLabel* title_label_ = nullptr;
    QLabel* filename_label_ = nullptr;
    QPushButton* primary_action_btn_ = nullptr;
    QPushButton* secondary_action_btn_ = nullptr;

    // Phase-Stepper
    QWidget* stepper_widget_ = nullptr;
    QLabel* stepper_review_lbl_ = nullptr;
    QLabel* stepper_edit_lbl_ = nullptr;
    QLabel* stepper_output_lbl_ = nullptr;

    // Player-Area
    QFrame* player_frame_ = nullptr;
    QLabel* player_icon_label_ = nullptr;
    QLabel* player_meta_label_ = nullptr;

    // Edit-Controls (all disabled)
    QWidget* edit_controls_ = nullptr;
    QPushButton* trim_btn_ = nullptr;
    QPushButton* add_marker_btn_ = nullptr;
    QPushButton* split_chapter_btn_ = nullptr;
    QLabel* duration_label_ = nullptr;

    // Timeline (visual, disabled)
    QFrame* timeline_frame_ = nullptr;
    QLabel* timeline_in_label_ = nullptr;
    QLabel* timeline_out_label_ = nullptr;

    // Output-Panel
    QWidget* output_panel_ = nullptr;
    QFrame* output_opt_keep_mkv_ = nullptr;
    QFrame* output_opt_remux_mp4_ = nullptr;
    QFrame* output_opt_reencode_ = nullptr;
    QLabel* dest_folder_label_ = nullptr;
    QPushButton* browse_dest_btn_ = nullptr;

    // Exporting-Panel
    QWidget* exporting_panel_ = nullptr;
    QLabel* exporting_status_label_ = nullptr;
    QProgressBar* exporting_bar_ = nullptr;
    QLabel* exporting_detail_label_ = nullptr;

    // Done/Failed-Panel
    QWidget* result_panel_ = nullptr;
    QLabel* result_icon_label_ = nullptr;
    QLabel* result_title_label_ = nullptr;
    QLabel* result_detail_label_ = nullptr;
    QPushButton* result_open_folder_btn_ = nullptr;
    QPushButton* result_reveal_btn_ = nullptr;

    // Detail-Rail (right side)
    QFrame* detail_rail_ = nullptr;
    QLabel* fact_duration_val_ = nullptr;
    QLabel* fact_size_val_ = nullptr;
    QLabel* fact_res_val_ = nullptr;
    QLabel* fact_fps_val_ = nullptr;
    QLabel* fact_video_val_ = nullptr;
    QLabel* fact_audio_val_ = nullptr;
    QLabel* fact_container_val_ = nullptr;

    // 0.11-Placeholder-Banner
    QFrame* placeholder_banner_ = nullptr;

    // Demo timer for export simulation
    class QTimer* export_demo_timer_ = nullptr;
};

} // namespace exosnap
