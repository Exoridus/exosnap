#pragma once
#include <QWidget>
#include <filesystem>
#include <memory>
#include <recorder_core/audio_input_device.h>
#include <vector>

#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"
#include "../models/WebcamSettings.h"
#include "../services/PreviewService.h"
#include "../services/RecordingCoordinator.h"
#include "../ui/widgets/RegionSelectionOverlay.h"
#include "../viewmodels/RecordViewModel.h"

#include <capability/capability_set.h>
#include <capability/config_types.h>

class QComboBox;
class QBoxLayout;
class QFrame;
class QLabel;
class QPushButton;
class QResizeEvent;
class QSlider;
class QVBoxLayout;

namespace exosnap {

namespace ui::widgets {
class AudioSourceRow;
class CaptureTargetCard;
class ExoCheckBox;
class PreviewSurface;
class SectionRuleHeader;
class VUMeterWidget;
} // namespace ui::widgets

namespace ui::dialogs {
class SourcePickerDialog;
}

class RecordPage : public QWidget {
    Q_OBJECT
  public:
    explicit RecordPage(QWidget* parent = nullptr);
    ~RecordPage() override;
    void setOutputSettings(const OutputSettingsModel& settings);
    void setVideoSettings(const VideoSettingsModel& settings);
    void setWebcamSettings(const WebcamSettings& settings);
    void setActiveProfileName(const std::string& profile_name);
    void applyPersistedAudioSettings(const capability::AudioUiState& state);
    void setRuntimeCapabilities(const capability::CapabilitySet& caps);
    void rebroadcastChromeState();

  signals:
    void chromeStateChanged(bool recording, const QString& status_label, const QString& context_text);
    void chromeRuntimeMetricsChanged(const QString& elapsed_text, const QString& bitrate_text, const QString& drop_text,
                                     const QString& size_text);
    void navigateToOutputPage();
    void navigateToDiagnosticsPage();
    void audioSettingsChanged(const capability::AudioUiState& state);

  public slots:
    void onHotkeyToggle();
    void onHotkeyPauseToggle();

  private slots:
    void onStart();
    void onStop();
    void onPause();
    void onResume();
    void onSelectMonitorTarget();
    void onSelectWindowTarget();
    void onSelectRegionTarget();
    void onOpenSourcePicker();
    void onTargetPickerChanged(int index);
    void onRefreshTargets();
    void onRegionSelected(QRect region_virtual_screen);
    void onRegionCancelled();

  private:
    // Resolve target and start recording (after any overlay selection is complete).
    void doStartRecording(std::optional<recorder_core::CaptureRegion> crop_region = std::nullopt);
    // Ensure the region overlay widget exists.
    void ensureRegionOverlay();

    struct ReadinessRow {
        QLabel* icon = nullptr;
        QLabel* title = nullptr;
        QLabel* detail = nullptr;
    };

    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void ensureCoordinatorInit();
    void initCoordinator();
    void refresh();
    void updateStatsDisplay();
    void updateResultDisplay();
    void updateTargetCards();
    void updateReadinessRows();
    void updateResponsiveLayout();
    void updateAudioMeterLevels();
    void updateAudioControls();
    void updateAudioControlsVisibility();
    void updateAudioTrackPreview();
    void updateHeroButton();
    void updateSourceChip();
    void updateOpenFolderButtonState();
    void updateDestinationMeta();
    void syncTargetSelectionToCombo(int target_index);
    void enumerateTargets(bool preserve_current_selection);
    void rebuildTargetPicker();
    void onAudioRowEnabledChanged(int row_index, bool enabled);
    void onAudioRowMergeChanged(int row_index, bool merge);
    void swapAudioSourceRows(int a, int b);
    void rebuildAudioRowWidgets();
    void updateAudioRowMergeVisibility();
    void onMicDeviceChanged(int index);
    void onMicChannelChanged(int index);
    void onMicGainChanged(int db_value);
    void openOutputFolder();
    void setOutputSettingsSummary(const OutputSettingsModel& settings);
    void populateMicDeviceCombo();
    void updateMicDeviceNoteLabel();
    void syncMicMeterService();
    void syncSysMeterService();
    void syncAppMeterService();
    void emitAudioSettingsChanged();
    void emitChromeState();
    void syncCoordinatorTargetContext();
    void startPreviewIfIdle();
    void updatePreviewHeightClamp();
    QString buildChromeStatusLabel() const;
    QString buildPreviewBottomLeftText(bool recording) const;
    QString buildPreviewBottomRightText(bool recording) const;
    QString buildTimerText(bool recording) const;
    bool isSourceSelectionLocked() const;

    int monitor_target_index_ = -1;
    int window_target_index_ = -1;
    std::vector<int> monitor_target_indices_;
    std::vector<int> window_target_indices_;
    std::vector<recorder_core::AudioInputDeviceInfo> mic_devices_;

    RecordViewModel view_model_;
    std::unique_ptr<RecordingCoordinator> coordinator_;
    std::unique_ptr<PreviewService> preview_service_;

    QLabel* capability_label_ = nullptr;
    QComboBox* target_combo_ = nullptr;
    QBoxLayout* cockpit_split_layout_ = nullptr;
    QFrame* target_picker_panel_ = nullptr;
    QLabel* target_picker_kind_label_ = nullptr;
    QComboBox* target_picker_combo_ = nullptr;
    QPushButton* target_refresh_btn_ = nullptr;
    QLabel* target_picker_note_label_ = nullptr;
    ui::widgets::PreviewSurface* preview_surface_ = nullptr;
    QLabel* control_state_label_ = nullptr;
    QLabel* timer_label_ = nullptr;
    ui::widgets::SectionRuleHeader* capture_header_ = nullptr;
    QWidget* source_row_ = nullptr;
    QFrame* source_chip_panel_ = nullptr;
    QLabel* source_kind_label_ = nullptr;
    QLabel* source_name_label_ = nullptr;
    QLabel* source_meta_label_ = nullptr;
    QLabel* source_preset_label_ = nullptr;
    QLabel* source_lock_label_ = nullptr;
    QPushButton* change_source_btn_ = nullptr;
    ui::widgets::CaptureTargetCard* monitor_card_ = nullptr;
    ui::widgets::CaptureTargetCard* window_card_ = nullptr;
    ui::widgets::CaptureTargetCard* region_card_ = nullptr;
    QPushButton* region_pick_btn_ = nullptr;
    QLabel* region_summary_label_ = nullptr;
    QWidget* region_options_panel_ = nullptr;
    ui::widgets::RegionSelectionOverlay* region_overlay_ = nullptr;
    ui::widgets::ExoCheckBox* select_on_record_check_ = nullptr;
    ui::widgets::SectionRuleHeader* readiness_header_ = nullptr;
    QFrame* readiness_panel_ = nullptr;
    QFrame* readiness_rule_ = nullptr;
    QWidget* readiness_rows_container_ = nullptr;
    QPushButton* readiness_diagnostics_btn_ = nullptr;
    std::vector<ReadinessRow> readiness_rows_;
    ui::widgets::SectionRuleHeader* audio_settings_header_ = nullptr;
    QWidget* audio_rows_container_ = nullptr;
    QVBoxLayout* audio_rows_layout_ = nullptr;
    std::vector<ui::widgets::AudioSourceRow*> audio_source_rows_;
    int drag_source_index_ = -1;
    int drag_start_y_ = 0;
    QWidget* mic_device_row_ = nullptr;
    QComboBox* mic_device_combo_ = nullptr;
    QPushButton* mic_refresh_btn_ = nullptr;
    QLabel* mic_device_note_label_ = nullptr;
    QWidget* mic_channel_row_ = nullptr;
    QComboBox* mic_channel_combo_ = nullptr;
    QWidget* mic_gain_row_ = nullptr;
    QSlider* mic_gain_slider_ = nullptr;
    QLabel* mic_gain_value_label_ = nullptr;
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
    QLabel* result_file_label_ = nullptr;
    QLabel* result_stats_label_ = nullptr;
    QLabel* result_path_label_ = nullptr;
    QLabel* result_technical_label_ = nullptr;
    QFrame* result_technical_separator_ = nullptr;
    std::filesystem::path last_output_folder_;
    capability::Container current_container_ = capability::Container::Matroska;
    capability::VideoCodec current_video_codec_ = capability::VideoCodec::H264Nvenc;
    capability::AudioCodec current_audio_codec_ = capability::AudioCodec::AacMf;
    std::wstring active_profile_name_;
    float preflight_mic_rms_ = 0.0f;
    float preflight_sys_rms_ = 0.0f;
    float preflight_app_rms_ = 0.0f;
    uint32_t preflight_app_pid_ = 0;
    bool coordinator_needs_init_ = true;
    capability::CapabilitySet shared_runtime_caps_{};
    bool shared_runtime_caps_received_ = false;

    // Rail dashboard controls
    QWidget* audio_settings_panel_ = nullptr;
    QFrame* destination_panel_ = nullptr;
    QPushButton* hero_action_btn_ = nullptr;
    QPushButton* secondary_action_btn_ = nullptr;
    QPushButton* rail_diagnostics_btn_ = nullptr;
    QFrame* rail_control_panel_ = nullptr;
    QFrame* rail_stats_grid_ = nullptr;
    QLabel* rail_size_value_label_ = nullptr;
    QLabel* rail_drop_value_label_ = nullptr;
    QLabel* rail_encoder_value_label_ = nullptr;
    QLabel* rail_fps_value_label_ = nullptr;
    QLabel* rail_readiness_label_ = nullptr;
    QLabel* rail_summary_label_ = nullptr;
    QLabel* rail_stats_label_ = nullptr;
    QLabel* readiness_summary_label_ = nullptr;
    QPushButton* result_open_folder_btn_ = nullptr;
    QPushButton* result_record_again_btn_ = nullptr;
};

} // namespace exosnap
