#pragma once
#include <QString>
#include <QWidget>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

#include "../models/RecordingMarker.h"
#include <recorder_core/mp4_remuxer.h>
#include <recorder_core/pipeline_diagnostics.h>

class QComboBox;
class QElapsedTimer;
class QLabel;
class QPushButton;
class QFrame;
class QProgressBar;
class QScrollArea;
class QTimer;
class QEvent;
class QObject;

namespace exosnap {

namespace ui::widgets {
class EditTimeline;
}

// Context passed to EditExportPage when opening the edit surface.
// Contains everything needed for the Review, Edit, and Output phases.
struct EditContext {
    // File metadata (from the completed recording result)
    QString output_path;     // final output (MP4 or MKV)
    QString mkv_master_path; // edit master (MKV); same as output for MKV recordings
    QString duration;        // human-readable duration (e.g. "1:23")
    QString size;            // human-readable file size (e.g. "142 MB")
    QString resolution;      // e.g. "1920x1080"
    QString fps;             // e.g. "60 fps CFR"
    QString video_codec;     // e.g. "AV1 (NVENC)"
    QString audio_codec;     // e.g. "Opus"
    QString container;       // e.g. "MKV" or "MP4"

    // Post-flight data (from RecordPage diagnostics tracking)
    double peak_av_drift_ms = 0.0;
    bool av_drift_available = false;
    recorder_core::RecordingDiagnosticsSnapshot completed_snapshot;

    // Markers pre-loaded from the recording session (fallback if sidecar cannot be read)
    std::vector<RecordingMarker> markers;
    QString marker_sidecar_path; // companion .markers.json path

    // Total recording duration in seconds (0.0 = unknown). Used to place
    // markers proportionally on the Edit timeline; unknown duration renders an
    // inert timeline (no handles, playhead, or markers).
    double duration_seconds = 0.0;
};

// Edit/Export-Surface: Review (post-flight report), Edit (trim handles +
// playhead directly on the timeline), Output (container/save-mode choice), and
// real stream-copy export via mp4_remuxer. Markers ride along an export as a
// retimed JSON sidecar (never container chapters).
class EditExportPage : public QWidget {
    Q_OBJECT
  public:
    enum class Phase {
        Review,    // Post-flight report; read-only
        Edit,      // Trim handles + playhead on the timeline
        Output,    // Container / save-mode choice
        Exporting, // Real stream-copy export running
        Done,      // Export complete
        Failed,    // Export failed
    };

    explicit EditExportPage(QWidget* parent = nullptr);
    ~EditExportPage() override;

    // Primary entry point: full context from the completed recording session.
    void setEditContext(const EditContext& ctx);

    // Legacy shim: partial data from the notification toast (no master path or diagnostics).
    void setRecordingInfo(const QString& file_path, const QString& duration, const QString& size,
                          const QString& resolution, const QString& fps, const QString& video_codec,
                          const QString& audio_codec, const QString& container);

    [[nodiscard]] Phase phase() const noexcept {
        return phase_;
    }
    void setPhase(Phase phase);

    // Preview playback clock: drives the timeline playhead. The video frame
    // itself is still the deferred 0.11 preview — the clock, playhead, and
    // scrub semantics are real and a frame view will attach to this position.
    void setPreviewPlaying(bool playing);
    [[nodiscard]] bool isPreviewPlaying() const noexcept {
        return preview_playing_;
    }
    void setPreviewPositionMs(qint64 position_ms);
    [[nodiscard]] qint64 previewPositionMs() const noexcept {
        return preview_position_ms_;
    }

    // Trim range in clip milliseconds (visual-harness / test injection; the
    // interactive path is the timeline's own handles).
    void setTrimRangeMs(qint64 start_ms, qint64 end_ms);

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
    void onTrimHandleReleased(qint64 start_ms, qint64 end_ms);
    void onScrubStarted();
    void onScrubMoved(qint64 position_ms);
    void onScrubFinished();
    void onPreviewTick();

  protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void buildUi();
    void refreshPhase();
    void loadMarkers();
    void runExport();
    void refreshPlayButton();
    void updatePlayerHeight();
    [[nodiscard]] qint64 durationMs() const noexcept;

    Phase phase_ = Phase::Review;

    // Full context set by setEditContext (primary path).
    EditContext ctx_;

    QString file_path_;
    QString duration_;
    QString size_;
    QString resolution_;
    QString fps_;
    QString video_codec_;
    QString audio_codec_;
    QString container_;

    // Review Panel (post-flight report)
    QWidget* review_panel_ = nullptr;
    QLabel* review_drop_label_ = nullptr;
    QLabel* review_drift_label_ = nullptr;
    QLabel* review_health_label_ = nullptr;

    // Output combos (container + save mode)
    QComboBox* output_container_combo_ = nullptr;
    QComboBox* output_save_mode_combo_ = nullptr;

    // Trim state
    std::vector<int64_t> keyframe_timestamps_; // sorted keyframe PTS in microseconds
    std::vector<RecordingMarker> markers_;
    int64_t trim_start_us_ = recorder_core::TrimRange::kNoTimestamp;
    int64_t trim_end_us_ = recorder_core::TrimRange::kNoTimestamp;
    double duration_seconds_ = 0.0; // total recording duration; 0 = unknown (inert timeline)

    // Preview playback clock (position only; no decoded frames yet)
    QTimer* preview_timer_ = nullptr;
    QElapsedTimer* preview_elapsed_ = nullptr;
    bool preview_playing_ = false;
    bool resume_after_scrub_ = false;
    qint64 preview_position_ms_ = 0;

    // Export thread + output path tracking
    std::thread export_thread_;
    std::atomic<bool> export_cancel_{false};
    std::filesystem::path export_output_path_;
    QString last_export_error_; // real error message from the last failed export

    // Mode-Bar
    QPushButton* back_btn_ = nullptr;
    QLabel* title_label_ = nullptr;
    QLabel* filename_label_ = nullptr;

    // Bottom action bar (Save button bottom-right, like the Record page dock)
    QFrame* action_bar_ = nullptr;
    QPushButton* primary_action_btn_ = nullptr;
    QPushButton* secondary_action_btn_ = nullptr;

    // Phase-Stepper
    QWidget* stepper_widget_ = nullptr;
    QLabel* stepper_review_lbl_ = nullptr;
    QLabel* stepper_edit_lbl_ = nullptr;
    QLabel* stepper_output_lbl_ = nullptr;

    // Player-Area
    QFrame* player_frame_ = nullptr;
    QPushButton* play_pause_btn_ = nullptr;
    QLabel* player_meta_label_ = nullptr;

    // Timeline (interactive: trim handles, markers, playhead)
    ui::widgets::EditTimeline* timeline_ = nullptr;

    // Output-Panel
    QWidget* output_panel_ = nullptr;
    QLabel* dest_folder_label_ = nullptr;

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

    // Details card (right rail)
    QFrame* detail_rail_ = nullptr;
    QLabel* fact_duration_val_ = nullptr;
    QLabel* fact_size_val_ = nullptr;
    QLabel* fact_res_val_ = nullptr;
    QLabel* fact_fps_val_ = nullptr;
    QLabel* fact_video_val_ = nullptr;
    QLabel* fact_audio_val_ = nullptr;
    QLabel* fact_container_val_ = nullptr;
};

} // namespace exosnap
