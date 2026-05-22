#pragma once
#include <QWidget>
#include <filesystem>
#include <memory>
#include <recorder_core/audio_input_device.h>
#include <vector>

#include "../models/OutputSettingsModel.h"
#include "../services/RecordingCoordinator.h"
#include "../viewmodels/RecordViewModel.h"

class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace exosnap {

namespace ui::widgets {
class CaptureTargetCard;
class ExoCheckBox;
class PreviewSurface;
class SectionRuleHeader;
class VUMeterWidget;
} // namespace ui::widgets

class RecordPage : public QWidget {
    Q_OBJECT
  public:
    explicit RecordPage(QWidget* parent = nullptr);
    void setOutputSettings(const OutputSettingsModel& settings);
    void applyPersistedAudioSettings(const capability::AudioUiState& state);

  signals:
    void chromeStateChanged(bool recording, const QString& status_label, const QString& context_text);
    void chromeRuntimeMetricsChanged(const QString& elapsed_text, const QString& bitrate_text,
                                     const QString& drop_text);
    void navigateToOutputPage();
    void audioSettingsChanged(const capability::AudioUiState& state);

  private slots:
    void onStart();
    void onStop();
    void onSelectMonitorTarget();
    void onSelectWindowTarget();
    void onSelectRegionTarget();
    void onTargetPickerChanged(int index);
    void onRefreshTargets();

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
    void updateAudioMeterLevels();
    void updateAudioControls();
    void updateAudioControlsVisibility();
    void updateAudioTrackPreview();
    void updateOpenFolderButtonState();
    void syncTargetSelectionToCombo(int target_index);
    void enumerateTargets(bool preserve_current_selection);
    void rebuildTargetPicker();
    void onAppAudioToggled(bool checked);
    void onSysAudioToggled(bool checked);
    void onSeparateTracksToggled(bool checked);
    void onMicToggled(bool checked);
    void onMicDeviceChanged(int index);
    void onMicChannelChanged(int index);
    void openOutputFolder();
    void setOutputSettingsSummary(const OutputSettingsModel& settings);
    void populateMicDeviceCombo();
    void updateMicDeviceNoteLabel();
    void emitAudioSettingsChanged();
    void emitChromeState();
    void syncCoordinatorTargetContext();
    QString buildChromeStatusLabel() const;
    QString buildPreviewBottomLeftText(bool recording) const;
    QString buildPreviewBottomRightText(bool recording) const;
    QString buildTimerText(bool recording) const;

    int monitor_target_index_ = -1;
    int window_target_index_ = -1;
    int region_target_index_ = -1;
    std::vector<int> monitor_target_indices_;
    std::vector<int> window_target_indices_;
    std::vector<recorder_core::AudioInputDeviceInfo> mic_devices_;

    RecordViewModel view_model_;
    std::unique_ptr<RecordingCoordinator> coordinator_;

    QLabel* capability_label_ = nullptr;
    QComboBox* target_combo_ = nullptr;
    QFrame* target_picker_panel_ = nullptr;
    QLabel* target_picker_kind_label_ = nullptr;
    QComboBox* target_picker_combo_ = nullptr;
    QPushButton* target_refresh_btn_ = nullptr;
    QLabel* target_picker_note_label_ = nullptr;
    ui::widgets::PreviewSurface* preview_surface_ = nullptr;
    QLabel* control_state_label_ = nullptr;
    QLabel* timer_label_ = nullptr;
    QPushButton* start_btn_ = nullptr;
    QPushButton* stop_btn_ = nullptr;
    ui::widgets::SectionRuleHeader* capture_header_ = nullptr;
    ui::widgets::CaptureTargetCard* monitor_card_ = nullptr;
    ui::widgets::CaptureTargetCard* window_card_ = nullptr;
    ui::widgets::CaptureTargetCard* region_card_ = nullptr;
    ui::widgets::SectionRuleHeader* readiness_header_ = nullptr;
    QFrame* readiness_panel_ = nullptr;
    std::vector<ReadinessRow> readiness_rows_;
    ui::widgets::SectionRuleHeader* audio_settings_header_ = nullptr;
    ui::widgets::ExoCheckBox* app_audio_check_ = nullptr;
    ui::widgets::ExoCheckBox* sys_audio_check_ = nullptr;
    ui::widgets::ExoCheckBox* separate_tracks_check_ = nullptr;
    ui::widgets::ExoCheckBox* mic_check_ = nullptr;
    QWidget* mic_device_row_ = nullptr;
    QComboBox* mic_device_combo_ = nullptr;
    QPushButton* mic_refresh_btn_ = nullptr;
    QLabel* mic_device_note_label_ = nullptr;
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
    QPushButton* open_folder_btn_ = nullptr;
    QPushButton* destination_settings_btn_ = nullptr;
    QFrame* result_panel_ = nullptr;
    QLabel* result_title_label_ = nullptr;
    QLabel* result_message_label_ = nullptr;
    QLabel* result_action_label_ = nullptr;
    QLabel* result_stats_label_ = nullptr;
    QLabel* result_path_label_ = nullptr;
    QLabel* result_technical_label_ = nullptr;
    QFrame* result_technical_separator_ = nullptr;
    std::filesystem::path last_output_folder_;
};

} // namespace exosnap
