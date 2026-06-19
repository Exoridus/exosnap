#pragma once
#include <QWidget>

#include "../../../libs/capability/include/capability/audio_ui_state.h"
#include "../../../libs/recorder_core/include/recorder_core/audio_input_device.h"
#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"
#include "../models/WebcamSettings.h"
#include "../services/AudioDeviceNotifier.h"
#include "../services/WebcamDeviceNotifier.h"
#include "../viewmodels/PresentationState.h"

#include <filesystem>
#include <string>
#include <vector>

class QAction;
class QBoxLayout;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QResizeEvent;
class QSpinBox;
class QToolButton;

namespace exosnap::ui::widgets {
class ExoToggle;
class VUMeterWidget;
class WebcamSetupPanel;
} // namespace exosnap::ui::widgets

namespace exosnap::ui::dialogs {
class UpdateSettingsPanel;
} // namespace exosnap::ui::dialogs

namespace exosnap {

class ConfigPage : public QWidget {
    Q_OBJECT
  public:
    struct ProfileOption {
        QString id;
        QString label;
        bool built_in = false;
        bool modified = false;
        bool available = true;
        QString availability_reason;
    };

    explicit ConfigPage(const OutputSettingsModel& initial_settings, const VideoSettingsModel& initial_video,
                        QWidget* parent = nullptr);

    void setOutputSettings(const OutputSettingsModel& settings);
    void setVideoSettings(const VideoSettingsModel& settings);
    void setOutputFolder(const std::filesystem::path& folder);
    void setAudioUiState(const capability::AudioUiState& state);
    void setWebcamSettings(const WebcamSettings& settings);
    void setReadinessStatus(const QString& status_label);
    // Preset card contract: options = presets (id + label); selected_id = active preset;
    // default_id = startup-default preset (shown with a badge); dirty = unsaved changes.
    void setPresetOptions(const std::vector<ProfileOption>& options, const QString& selected_id,
                          const QString& default_id, bool dirty);
    // Lightweight dirty-only update (avoids rebuilding the full combo).
    void setPresetDirty(bool dirty);
    void setActiveProfileName(const QString& profile_name);
    void setRecordingControlsLocked(bool locked);

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    // Drive the embedded Webcam card deterministically for visual-test scenarios.
    void applyVisualWebcamState(bool available, bool mirror);

    // Drive the inline preset save-error affordance for visual-test scenarios.
    // Passing true shows a deterministic error label (name-conflict copy);
    // passing false hides it.  No real save is performed.
    void applyVisualPresetSaveError(bool show);
#endif

    // Live audio meter update forwarded from RecordPage via MainWindow.
    // sys01/app01/mic01: pre-computed 0..1 dock-level values (0 = inactive or silence).
    // sys/app/mic_active: true when the meter service is running for that source.
    void setAudioMeterLevels(float sys01, float app01, float mic01, bool sys_active, bool app_active, bool mic_active);

    // Reactive device-change handlers (driven by MainWindow from the three notifiers).
    // These preserve selection state and never emit settings-changed or dirty the preset.
    void onAudioDevicesChanged(const exosnap::AudioDeviceSnapshot& snap);
    void onWebcamDevicesChanged(const exosnap::WebcamDeviceSnapshot& snap);

  signals:
    void formatSettingsChanged(const OutputSettingsModel& settings);
    // Preset-selection signal.
    void presetSelected(const QString& id);
    void videoSettingsChanged(const VideoSettingsModel& settings);
    void audioSettingsChanged(const capability::AudioUiState& state);
    void webcamSettingsChanged(const WebcamSettings& settings);
    void diagnosticsRequested();
    void webcamDetailsRequested();
    void advancedRequested();

    // Emitted when the user presses the Rescan button in the Audio card.
    // MainWindow connects this to audio_notifier_.rescan() so Rescan and the
    // reactive path share the same canonical refresh, with no duplicate
    // enumeration and no duplicate devices.
    void audioRescanRequested();

    // ---- Preset management signals ----
    void savePresetRequested();
    void savePresetAsRequested(const QString& name);
    void newPresetRequested();
    void duplicatePresetRequested();
    void renamePresetRequested(const QString& name);
    void deletePresetRequested();
    void resetChangesRequested();
    void resetToDefaultsRequested();
    void setDefaultPresetRequested();
    // Emitted when the user opens the Manage presets overlay.
    void managePresetsRequested();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void updateResponsiveLayout();
    void onContainerChanged(int id);
    void onVideoCodecChanged(int index);
    void onAudioCodecChanged(int index);
    void onProfileSelectionChanged(int index);
    void onQualityChanged(int index);
    void onQualitySegmentSelected(int preset_id);
    void onFrameRateChanged(int index);
    void onTimingSelected(int timing_id);
    void onOutputResolutionSelected(int mode_id);
    void onSplitModeChanged(int index);
    void updateSplitSelection();
    void onSplitSizeModeChanged(int index);
    void updateSplitSizeSelection();
    void onCursorChanged();
    void updateQualitySummary();
    void updateQualitySegmentSelection();
    void updateFrameRateSelection();
    void updateTimingSelection();
    void updateOutputResolutionSelection();
    void updateCustomResolutionVisibility();
    void updateCustomResolutionValidation();
    void onCustomWidthChanged(int value);
    void onCustomHeightChanged(int value);
    void updateEffectiveOutputSummary();
    void onBrowse();
    void onDestinationEditingFinished();
    void onPatternEditingFinished();
    void emitCurrentFormatSettings();
    void emitCurrentVideoSettings();
    void reconcileContainerCodecRules();
    void updateVideoCodecChoices();
    void updateAudioCodecChoices();
    void updateFormatDisplay();
    void updateOutputValidationState();
    void updateExampleFilename();

    // Single entry point for atomic audio widget state application.
    // Derives widget visibility, enabled, checked, and label states from the
    // stored audio_ui_state_ and controls_locked_ in one pass.  Both
    // setAudioUiState() and setRecordingControlsLocked() delegate here after
    // updating their respective stored value, ensuring call order cannot produce
    // inconsistent widget state.
    void applyAudioConfigurationState();

    void onAudioAppToggled();
    void onAudioMicToggled();
    void onAudioSysToggled();
    void onAudioAppSeparateToggled();
    void onAudioMicSeparateToggled();
    void onAudioSysSeparateToggled();
    void onMicDeviceChanged(int index);
    void refreshMicDevices();
    void emitCurrentAudioSettings();

    // Preset management handlers.
    void onSavePreset();
    void onSavePresetAs();
    void onNewPreset();
    void onDuplicatePreset();
    void onRenamePreset();
    void onDeletePreset();
    void onResetChanges();
    void onResetToDefaults();
    void onSetDefaultPreset();
    void onManagePresets();
    void updatePresetActionState();

    capability::AudioUiState audio_ui_state_;
    WebcamSettings webcam_settings_;

    OutputSettingsModel format_settings_;
    VideoSettingsModel video_settings_;
    QString active_profile_name_;
    std::vector<ProfileOption> profile_options_;
    QString active_preset_id_;
    QString default_preset_id_;
    bool preset_dirty_ = false;
    // Current selected preset's built_in/available flags (set by setPresetOptions).
    bool active_preset_is_built_in_ = false;
    bool active_preset_is_available_ = true;

    QBoxLayout* columns_layout_ = nullptr;
    QBoxLayout* output_split_layout_ = nullptr;

    QButtonGroup* container_group_ = nullptr;
    QPushButton* mkv_radio_ = nullptr;
    QPushButton* webm_radio_ = nullptr;
    QPushButton* mp4_radio_ = nullptr;
    QComboBox* video_codec_combo_ = nullptr;
    QComboBox* audio_codec_combo_ = nullptr;
    QComboBox* profile_combo_ = nullptr;
    QLabel* format_display_label_ = nullptr;

    QComboBox* quality_combo_ = nullptr;
    QComboBox* frame_rate_combo_ = nullptr;
    QButtonGroup* quality_segment_group_ = nullptr;
    QPushButton* quality_segment_small_ = nullptr;
    QPushButton* quality_segment_balanced_ = nullptr;
    QPushButton* quality_segment_high_ = nullptr;
    QLabel* quality_badge_label_ = nullptr;
    QLabel* quality_settings_label_ = nullptr;
    QButtonGroup* timing_group_ = nullptr;
    QPushButton* timing_cfr_btn_ = nullptr;
    QPushButton* timing_vfr_btn_ = nullptr;
    QCheckBox* cursor_check_ = nullptr;

    QLabel* audio_summary_label_ = nullptr;

    // Per-source compact mono meters in the Settings Audio card.
    ui::widgets::VUMeterWidget* audio_sys_meter_ = nullptr;
    ui::widgets::VUMeterWidget* audio_app_meter_ = nullptr;
    ui::widgets::VUMeterWidget* audio_mic_meter_ = nullptr;
    QLabel* audio_sys_db_label_ = nullptr;
    QLabel* audio_app_db_label_ = nullptr;
    QLabel* audio_mic_db_label_ = nullptr;

    // Application audio section — shown for Window targets, hidden for Display/Region.
    QWidget* app_row_section_ = nullptr;

    QCheckBox* app_enabled_check_ = nullptr;
    ui::widgets::ExoToggle* app_separate_check_ = nullptr; // DF-12: pill toggle replaces QCheckBox
    QLabel* app_source_label_ = nullptr;

    QCheckBox* mic_enabled_check_ = nullptr;
    ui::widgets::ExoToggle* mic_separate_check_ = nullptr; // DF-12: pill toggle replaces QCheckBox
    QComboBox* mic_device_combo_ = nullptr;
    // Compact Rescan affordance for the Settings Audio card.
    // Emits audioRescanRequested() which MainWindow routes to audio_notifier_.rescan().
    QPushButton* audio_rescan_btn_ = nullptr;
    QLabel* mic_source_label_ = nullptr;

    std::vector<recorder_core::AudioInputDeviceInfo> mic_devices_;

    QCheckBox* sys_enabled_check_ = nullptr;
    ui::widgets::ExoToggle* sys_separate_check_ = nullptr; // DF-12: pill toggle replaces QCheckBox
    QLabel* sys_source_label_ = nullptr;

    QLineEdit* destination_edit_ = nullptr;
    QPushButton* browse_btn_ = nullptr;
    QLineEdit* naming_edit_ = nullptr;
    QButtonGroup* output_resolution_group_ = nullptr;
    QPushButton* output_res_native_btn_ = nullptr;
    QPushButton* output_res_4k_btn_ = nullptr;
    QPushButton* output_res_1440_btn_ = nullptr;
    QPushButton* output_res_1080_btn_ = nullptr;
    QPushButton* output_res_720_btn_ = nullptr;
    QPushButton* output_res_custom_btn_ = nullptr;
    QLabel* output_effective_summary_label_ = nullptr;

    // Split recording widgets (SPLIT-RECORDING-R1 / SPLIT-BY-SIZE-R1).
    QComboBox* split_mode_combo_ = nullptr;
    QSpinBox* split_custom_minutes_spin_ = nullptr;
    QWidget* split_custom_widget_ = nullptr;
    QLabel* split_summary_label_ = nullptr;
    // Size-split controls (SPLIT-BY-SIZE-R1).
    QComboBox* split_size_mode_combo_ = nullptr;
    QSpinBox* split_custom_size_spin_ = nullptr;
    QWidget* split_size_custom_widget_ = nullptr;

    // Custom resolution widgets (CUSTOM-OUTPUT-RESOLUTION-R1).
    QWidget* custom_resolution_widget_ = nullptr;
    QSpinBox* custom_width_spin_ = nullptr;
    QSpinBox* custom_height_spin_ = nullptr;
    QLabel* custom_resolution_validation_label_ = nullptr;
    // Stashed values preserved across mode switches.
    uint32_t stashed_custom_width_ = 0;
    uint32_t stashed_custom_height_ = 0;
    QLabel* folder_validation_label_ = nullptr;
    QLabel* pattern_validation_label_ = nullptr;
    QLabel* example_filename_label_ = nullptr;

    QFrame* readiness_panel_ = nullptr;
    QLabel* readiness_badge_label_ = nullptr;
    QLabel* readiness_detail_label_ = nullptr;
    QPushButton* view_details_btn_ = nullptr;

    // Preset card widgets.
    QLabel* profile_status_label_ = nullptr;
    QLabel* preset_dirty_indicator_ = nullptr;
    QLabel* preset_default_badge_ = nullptr;
    QPushButton* preset_save_btn_ = nullptr;
    QPushButton* preset_save_as_btn_ = nullptr;
    QToolButton* profile_overflow_btn_ = nullptr;
    // Preset management actions in the overflow menu.
    QAction* save_preset_action_ = nullptr;
    QAction* save_preset_as_action_ = nullptr;
    QAction* new_preset_action_ = nullptr;
    QAction* duplicate_preset_action_ = nullptr;
    QAction* rename_preset_action_ = nullptr;
    QAction* delete_preset_action_ = nullptr;
    QAction* reset_changes_action_ = nullptr;
    QAction* reset_to_defaults_action_ = nullptr;
    QAction* set_default_preset_action_ = nullptr;
    QAction* manage_presets_action_ = nullptr;

    ui::widgets::WebcamSetupPanel* webcam_setup_panel_ = nullptr;

    // UPDATE-WIRE-R1: embedded "Software updates" card (Settings → near Advanced).
    // Wired from MainWindow via findChild(objectName "settingsUpdatePanel").
    ui::dialogs::UpdateSettingsPanel* update_settings_panel_ = nullptr;

    QLabel* lock_note_label_ = nullptr;
    bool controls_locked_ = false;

    QLabel* token_help_label_ = nullptr;
    QPushButton* token_help_toggle_btn_ = nullptr;

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    // Inline error label for preset save-error visual-test scenario.
    // Created lazily on first applyVisualPresetSaveError(true) call and placed
    // below the preset selector row.  Hidden in all non-error-scenario states.
    QLabel* visual_preset_error_label_ = nullptr;
#endif
};

} // namespace exosnap
