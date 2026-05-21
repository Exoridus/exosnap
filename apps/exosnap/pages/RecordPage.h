#pragma once
#include <QWidget>
#include <memory>
#include <recorder_core/audio_input_device.h>
#include <vector>

#include "../services/RecordingCoordinator.h"
#include "../viewmodels/RecordViewModel.h"

class QComboBox;
class QCheckBox;
class QFrame;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace exosnap {

namespace ui::widgets {
class CaptureTargetCard;
class PreviewSurface;
class SectionRuleHeader;
class VUMeterWidget;
} // namespace ui::widgets

class RecordPage : public QWidget {
    Q_OBJECT
  public:
    explicit RecordPage(QWidget* parent = nullptr);

  signals:
    void chromeStateChanged(bool recording, const QString& status_label, const QString& context_text);

  private slots:
    void onStart();
    void onStop();
    void onSelectMonitorTarget();
    void onSelectWindowTarget();
    void onSelectRegionTarget();

  private:
    struct ReadinessRow {
        QLabel* icon = nullptr;
        QLabel* title = nullptr;
        QLabel* detail = nullptr;
    };

    void initCoordinator();
    void refresh();
    void updateStatsDisplay();
    void updateResultDisplay();
    void updateTargetCards();
    void updateReadinessRows();
    void updateAudioPlaceholders();
    void updateAudioControls();
    void updateAudioControlsVisibility();
    void updateAudioTrackPreview();
    void syncTargetSelectionToCombo(int target_index);
    void onAppAudioToggled(bool checked);
    void onSysAudioToggled(bool checked);
    void onSeparateTracksToggled(bool checked);
    void onMicToggled(bool checked);
    void onMicDeviceChanged(int index);
    void onMicChannelChanged(int index);
    void populateMicDeviceCombo();

    int monitor_target_index_ = -1;
    int window_target_index_ = -1;
    int region_target_index_ = -1;
    std::vector<recorder_core::AudioInputDeviceInfo> mic_devices_;

    RecordViewModel view_model_;
    std::unique_ptr<RecordingCoordinator> coordinator_;

    QLabel* capability_label_ = nullptr;
    QComboBox* target_combo_ = nullptr;
    ui::widgets::PreviewSurface* preview_surface_ = nullptr;
    QLabel* control_state_label_ = nullptr;
    QLabel* timer_label_ = nullptr;
    QLabel* size_value_label_ = nullptr;
    QLabel* est_value_label_ = nullptr;
    QPushButton* start_btn_ = nullptr;
    QPushButton* stop_btn_ = nullptr;
    QLabel* quick_toggle_note_label_ = nullptr;
    ui::widgets::SectionRuleHeader* capture_header_ = nullptr;
    ui::widgets::CaptureTargetCard* monitor_card_ = nullptr;
    ui::widgets::CaptureTargetCard* window_card_ = nullptr;
    ui::widgets::CaptureTargetCard* region_card_ = nullptr;
    ui::widgets::SectionRuleHeader* readiness_header_ = nullptr;
    QFrame* readiness_panel_ = nullptr;
    std::vector<ReadinessRow> readiness_rows_;
    ui::widgets::SectionRuleHeader* audio_settings_header_ = nullptr;
    QCheckBox* app_audio_check_ = nullptr;
    QCheckBox* sys_audio_check_ = nullptr;
    QCheckBox* separate_tracks_check_ = nullptr;
    QCheckBox* mic_check_ = nullptr;
    QWidget* mic_device_row_ = nullptr;
    QComboBox* mic_device_combo_ = nullptr;
    QWidget* mic_channel_row_ = nullptr;
    QComboBox* mic_channel_combo_ = nullptr;
    QFrame* track_preview_panel_ = nullptr;
    QVBoxLayout* track_preview_layout_ = nullptr;
    ui::widgets::SectionRuleHeader* audio_header_ = nullptr;
    ui::widgets::VUMeterWidget* app_meter_ = nullptr;
    ui::widgets::VUMeterWidget* mic_meter_ = nullptr;
    ui::widgets::VUMeterWidget* sys_meter_ = nullptr;
    QLabel* app_db_label_ = nullptr;
    QLabel* mic_db_label_ = nullptr;
    QLabel* sys_db_label_ = nullptr;
    ui::widgets::SectionRuleHeader* destination_header_ = nullptr;
    QLabel* output_path_label_ = nullptr;
    QLabel* output_meta_label_ = nullptr;
    QFrame* result_panel_ = nullptr;
    QLabel* result_status_label_ = nullptr;
    QLabel* result_path_label_ = nullptr;
    QLabel* result_phase_label_ = nullptr;
    QLabel* result_hresult_label_ = nullptr;
    QLabel* result_detail_label_ = nullptr;
};

} // namespace exosnap
