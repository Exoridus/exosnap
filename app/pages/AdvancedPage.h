#pragma once
#include <QWidget>

#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"

class QComboBox;
class QLabel;

namespace exosnap::ui::widgets {
class ExoCheckBox;
}

namespace exosnap {

class AdvancedPage : public QWidget {
    Q_OBJECT
  public:
    explicit AdvancedPage(QWidget* parent = nullptr);

    // Reflects the active profile and resolved capture settings in the read-only
    // baseline panel. Called by MainWindow whenever the active profile changes.
    void setBaseline(const OutputSettingsModel& output, const VideoSettingsModel& video, const QString& profile_name);

    // RECORDING-OVERLAY-R1: populate the overlay checkbox from the stored setting.
    // Call once after construction with the persisted value.
    void setShowOverlay(bool show);

    // DIAGNOSTICS-OVERLAY-R1: populate the diagnostics overlay checkbox from the stored setting.
    // Call once after construction with the persisted value.
    void setShowDiagnosticsOverlay(bool show);

    // NOTIFY-TOASTS-R1: populate the notifications checkbox from the stored setting.
    // Call once after construction with the persisted value.
    void setShowNotifications(bool show);

    // TRAY-CLOSE-TO-TRAY-R1: populate the keep-in-tray checkbox from the stored setting.
    // Call once after construction with the persisted value.
    void setKeepRunningInTray(bool keep);

    // QUICK-PILL-R1: populate the quick-controls checkbox from the stored setting.
    // Call once after construction with the persisted value.
    void setShowQuickControls(bool show);

  signals:
    void backToSettingsRequested();
    // Emitted when the "Show recording overlay" checkbox changes.
    void showOverlayChanged(bool show);
    // DIAGNOSTICS-OVERLAY-R1: emitted when the "Show diagnostics overlay" checkbox changes.
    void showDiagnosticsOverlayChanged(bool show);
    // NOTIFY-TOASTS-R1: emitted when the "Show notifications" checkbox changes.
    void showNotificationsChanged(bool show);
    // TRAY-CLOSE-TO-TRAY-R1: emitted when the "Keep running in tray" checkbox changes.
    void keepRunningInTrayChanged(bool keep);
    // QUICK-PILL-R1: emitted when the "Quick controls" checkbox changes.
    void showQuickControlsChanged(bool show);

  private:
    void onReset();

    QComboBox* log_level_combo_ = nullptr;
    ui::widgets::ExoCheckBox* nvtx_check_ = nullptr;
    ui::widgets::ExoCheckBox* overlay_check_ = nullptr;
    ui::widgets::ExoCheckBox* diagnostics_overlay_check_ = nullptr;
    ui::widgets::ExoCheckBox* notifications_check_ = nullptr;
    ui::widgets::ExoCheckBox* keep_in_tray_check_ = nullptr;   // TRAY-CLOSE-TO-TRAY-R1
    ui::widgets::ExoCheckBox* quick_controls_check_ = nullptr; // QUICK-PILL-R1

    QLabel* baseline_profile_label_ = nullptr;
    QLabel* baseline_container_label_ = nullptr;
    QLabel* baseline_video_label_ = nullptr;
    QLabel* baseline_quality_label_ = nullptr;
    QLabel* baseline_framerate_label_ = nullptr;
    QLabel* baseline_audio_label_ = nullptr;
    QLabel* baseline_cursor_label_ = nullptr;
};

} // namespace exosnap
