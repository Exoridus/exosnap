#pragma once
#include <QImage>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "../models/RecordingMarker.h"
#include <recorder_core/edit_player_session.h>
#include <recorder_core/mp4_remuxer.h>
#include <recorder_core/pipeline_diagnostics.h>

class QElapsedTimer;
class QLabel;
class QPushButton;
class QFrame;
class QScrollArea;
class QTimer;
class QEvent;
class QObject;
class QResizeEvent;
class QShowEvent;

namespace exosnap {

namespace ui::widgets {
class EditTimeline;
class EditPlayerSurface;
class EditDetailsRail;
class ExportPanel;
} // namespace ui::widgets

// Context passed to EditExportPage when opening the edit surface.
// Contains everything needed to populate the edit view and run an export.
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

// Edit/Export surface: one view — player, trim timeline, and a right rail
// carrying the details card plus the export panel the page drives; the
// post-flight report rides as a header icon. Export itself is a real stream-copy
// via mp4_remuxer; markers ride along as a retimed JSON sidecar (never container
// chapters). The only modal left in the flow is the overwrite confirmation.
class EditExportPage : public QWidget {
    Q_OBJECT
  public:
    explicit EditExportPage(QWidget* parent = nullptr);
    ~EditExportPage() override;

    // Primary entry point: full context from the completed recording session.
    void setEditContext(const EditContext& ctx);

    // Legacy shim: partial data from the notification toast (no master path or diagnostics).
    void setRecordingInfo(const QString& file_path, const QString& duration, const QString& size,
                          const QString& resolution, const QString& fps, const QString& video_codec,
                          const QString& audio_codec, const QString& container);

    // True while a stream-copy export is actually running. Dismissing the surface
    // (Escape, backdrop click, nav-away) is blocked for exactly this window, so a
    // running export is never silently abandoned.
    [[nodiscard]] bool isExportRunning() const noexcept {
        return export_running_;
    }

    // True when closing would drop work: a trim range is set, or the clip carries
    // markers.
    [[nodiscard]] bool hasUnsavedEdits() const;

    // Asks before that work is dropped. Returns true when closing may proceed
    // (nothing to lose, or the user chose to discard).
    [[nodiscard]] bool confirmDiscardEdits();

    // Preview playback clock: drives the timeline playhead. The decoded frame
    // itself is driven by player_session_ (real decode); this clock stays the
    // playhead-position source the UI already had — see setPreviewPlaying().
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

    // Visual-harness / test injection for the timeline's row stack and its
    // thumbnail strip. Both normally come from the opened clip, which no
    // fixture carries: `tile_count` of -1 fills the strip, 0 leaves it empty
    // (the state a real decode passes through while its tiles are still
    // arriving), and anything between renders a partly filled strip.
    void setTimelineFixture(const QStringList& audio_track_labels, int tile_count);

  signals:
    void backRequested();
    void exportCompleted(const QString& output_path);

  private slots:
    void onBackClicked();
    void onExportClicked();
    void onCancelExportClicked();
    void onOpenFolderClicked();
    void onRevealFileClicked();
    void onRetryExportClicked();
    void onTrimHandleReleased(qint64 start_ms, qint64 end_ms);
    void onScrubStarted();
    void onScrubMoved(qint64 position_ms);
    void onScrubFinished();
    void onPreviewTick();
    void onDecodedFrameReady(QImage frame); // marshalled onto the UI thread via invokeMethod

  protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    // Severity of the post-flight report, as carried by the header icon.
    enum class ReportSeverity {
        Neutral,  // Good / Unavailable / no snapshot: quiet info glyph
        Warning,  // amber triangle + short label
        Critical, // coral triangle + short label
    };

    void buildUi();
    void applyThemeStyles();
    void refreshReportIcon();
    void loadMarkers();
    // Asks before an export that replaces the original recording. Returns true
    // when the export may proceed (either it writes a new file, or the user
    // confirmed the replacement).
    bool confirmOverwrite();
    void runExport();
    void refreshPlayButton();
    // The action bar's Export button is the only way to start a run, so it has
    // to be out of reach while one is already in flight.
    void refreshExportAction();
    // Scrolls the rail so the export panel's status is on screen. At the minimum
    // window height the panel sits below the fold, and progress or a result the
    // user has to go looking for is not a report.
    void revealExportPanel();
    // Retunes the preview timer to the open clip's frame rate, capped at the
    // refresh rate of the screen the window is on.
    void refreshPreviewTickInterval();
    void updatePlayerHeight();
    // Width-driven layout: the rail keeps the details card and the export panel,
    // so it is narrowed rather than dropped as the surface gets tighter.
    void updateResponsiveLayout();
    [[nodiscard]] qint64 durationMs() const noexcept;

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

    // Post-flight report, computed in setEditContext() and shown as the header
    // icon's tooltip (severity rides on the icon itself).
    QString report_drops_text_;
    QString report_drift_text_;
    QString report_health_text_;
    ReportSeverity report_severity_ = ReportSeverity::Neutral;

    // Trim state
    std::vector<int64_t> keyframe_timestamps_; // sorted keyframe PTS in microseconds
    std::vector<RecordingMarker> markers_;
    int64_t trim_start_us_ = recorder_core::TrimRange::kNoTimestamp;
    int64_t trim_end_us_ = recorder_core::TrimRange::kNoTimestamp;
    double duration_seconds_ = 0.0; // total recording duration; 0 = unknown (inert timeline)

    // Preview playback clock (position source; decoded frames come from player_session_)
    QTimer* preview_timer_ = nullptr;
    QElapsedTimer* preview_elapsed_ = nullptr;
    bool preview_playing_ = false;
    bool resume_after_scrub_ = false;
    qint64 preview_position_ms_ = 0;

    // Export thread + output path tracking
    std::thread export_thread_;
    std::atomic<bool> export_cancel_{false};
    bool export_running_ = false;
    std::filesystem::path export_output_path_;
    QString last_export_error_; // real error message from the last failed export

    // Mode-Bar
    QFrame* mode_bar_ = nullptr;
    QPushButton* back_btn_ = nullptr;
    QLabel* title_label_ = nullptr;
    QLabel* filename_label_ = nullptr;
    QLabel* report_icon_ = nullptr;
    QLabel* report_label_ = nullptr; // only populated for Warning / Critical

    // Bottom action bar (Export button bottom-right, like the Record page dock)
    QFrame* action_bar_ = nullptr;
    QPushButton* primary_action_btn_ = nullptr;

    // Player-Area
    QFrame* player_frame_ = nullptr;
    QPushButton* play_pause_btn_ = nullptr;
    ui::widgets::EditPlayerSurface* player_surface_ = nullptr;
    QLabel* player_meta_label_ = nullptr;

    // Real decoder session driving player_surface_. Opened per clip in
    // setEditContext(), closed in hideEvent(). Its frame callback fires on
    // internal worker threads and is marshalled to the UI thread via
    // onDecodedFrameReady (Qt::QueuedConnection).
    std::unique_ptr<recorder_core::EditPlayerSession> player_session_;

    // Timeline (interactive: trim handles, markers, playhead)
    ui::widgets::EditTimeline* timeline_ = nullptr;

    // Right rail: a scroll area (a short window must scroll the column rather
    // than clip the export panel out of reach) around the details card and the
    // export panel.
    QScrollArea* rail_scroll_ = nullptr;
    ui::widgets::EditDetailsRail* detail_rail_ = nullptr;

    // Export panel: presentation only, driven from runExport().
    ui::widgets::ExportPanel* export_panel_ = nullptr;
};

} // namespace exosnap
