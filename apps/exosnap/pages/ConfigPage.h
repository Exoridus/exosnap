#pragma once
#include <QWidget>

#include "../../../libs/capability/include/capability/audio_ui_state.h"
#include "../../../libs/recorder_core/include/recorder_core/audio_input_device.h"
#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"
#include "../models/WebcamSettings.h"

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
class QRadioButton;
class QResizeEvent;
class QToolButton;

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
    void setProfileOptions(const std::vector<ProfileOption>& options, const QString& active_profile_id,
                           bool active_profile_modified);
    void setActiveProfileName(const QString& profile_name);
    void setRecordingControlsLocked(bool locked);

  signals:
    void formatSettingsChanged(const OutputSettingsModel& settings);
    void activeProfileChanged(const QString& profile_id);
    void videoSettingsChanged(const VideoSettingsModel& settings);
    void audioSettingsChanged(const capability::AudioUiState& state);
    void webcamSettingsChanged(const WebcamSettings& settings);
    void diagnosticsRequested();
    void webcamDetailsRequested();
    void advancedRequested();

    void newFromCurrentRequested(const QString& name);
    void newFromSafeDefaultRequested(const QString& name);
    void duplicateActiveProfileRequested();
    void renameActiveProfileRequested(const QString& name);
    void deleteActiveProfileRequested();
    void resetActiveProfileRequested();
    void saveModifiedBuiltInAsNewRequested(const QString& name);
    void importProfilesRequested(const QString& file_path);
    void exportSelectedProfileRequested(const QString& file_path);
    void exportAllUserProfilesRequested(const QString& file_path);
    void resetAllSettingsAndProfilesRequested();

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
    void onCfrChanged();
    void onCursorChanged();
    void updateQualitySummary();
    void updateQualitySegmentSelection();
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

    void onAudioAppToggled();
    void onAudioMicToggled();
    void onAudioSysToggled();
    void onAudioAppSeparateToggled();
    void onAudioMicSeparateToggled();
    void onAudioSysSeparateToggled();
    void onMicDeviceChanged(int index);
    void refreshMicDevices();
    void emitCurrentAudioSettings();
    void updateAudioSourceAvailability();

    void onWebcamEnabledToggled();
    void onWebcamDeviceChanged(int index);
    void refreshWebcamDevices();
    void emitCurrentWebcamSettings();
    void updateWebcamInfoLabel();

    void onImportProfiles();
    void onExportSelectedProfile();
    void onExportAllUserProfiles();
    void onDeleteActiveProfile();
    void onResetAllSettingsAndProfiles();
    void updateProfileActionState();
    void promptCreateProfileFromCurrent();
    void promptCreateProfileFromSafeDefault();
    void promptRenameActiveProfile();
    void promptSaveModifiedBuiltInAsNew();

    capability::AudioUiState audio_ui_state_;
    WebcamSettings webcam_settings_;

    OutputSettingsModel format_settings_;
    VideoSettingsModel video_settings_;
    QString active_profile_name_;
    std::vector<ProfileOption> profile_options_;

    QBoxLayout* columns_layout_ = nullptr;
    QBoxLayout* output_split_layout_ = nullptr;

    QButtonGroup* container_group_ = nullptr;
    QRadioButton* mkv_radio_ = nullptr;
    QRadioButton* webm_radio_ = nullptr;
    QRadioButton* mp4_radio_ = nullptr;
    QComboBox* video_codec_combo_ = nullptr;
    QComboBox* audio_codec_combo_ = nullptr;
    QComboBox* profile_combo_ = nullptr;
    QLabel* format_display_label_ = nullptr;

    QComboBox* quality_combo_ = nullptr;
    QButtonGroup* quality_segment_group_ = nullptr;
    QPushButton* quality_segment_small_ = nullptr;
    QPushButton* quality_segment_balanced_ = nullptr;
    QPushButton* quality_segment_high_ = nullptr;
    QLabel* quality_badge_label_ = nullptr;
    QLabel* quality_settings_label_ = nullptr;
    QCheckBox* cfr_check_ = nullptr;
    QCheckBox* cursor_check_ = nullptr;

    QLabel* audio_summary_label_ = nullptr;

    QCheckBox* app_enabled_check_ = nullptr;
    QCheckBox* app_separate_check_ = nullptr;
    QLabel* app_source_label_ = nullptr;

    QCheckBox* mic_enabled_check_ = nullptr;
    QCheckBox* mic_separate_check_ = nullptr;
    QComboBox* mic_device_combo_ = nullptr;
    QLabel* mic_source_label_ = nullptr;

    std::vector<recorder_core::AudioInputDeviceInfo> mic_devices_;

    QCheckBox* sys_enabled_check_ = nullptr;
    QCheckBox* sys_separate_check_ = nullptr;
    QLabel* sys_source_label_ = nullptr;

    QLineEdit* destination_edit_ = nullptr;
    QPushButton* browse_btn_ = nullptr;
    QLineEdit* naming_edit_ = nullptr;
    QLabel* folder_validation_label_ = nullptr;
    QLabel* pattern_validation_label_ = nullptr;
    QLabel* example_filename_label_ = nullptr;

    QFrame* readiness_panel_ = nullptr;
    QLabel* readiness_badge_label_ = nullptr;
    QLabel* readiness_detail_label_ = nullptr;
    QPushButton* view_details_btn_ = nullptr;

    QLabel* profile_status_label_ = nullptr;
    QPushButton* save_as_new_btn_ = nullptr;
    QPushButton* reset_profile_btn_ = nullptr;
    QToolButton* profile_overflow_btn_ = nullptr;
    QAction* new_from_current_action_ = nullptr;
    QAction* new_from_safe_default_action_ = nullptr;
    QAction* duplicate_profile_action_ = nullptr;
    QAction* rename_profile_action_ = nullptr;
    QAction* delete_profile_action_ = nullptr;
    QAction* import_profiles_action_ = nullptr;
    QAction* export_selected_action_ = nullptr;
    QAction* export_all_users_action_ = nullptr;
    QAction* reset_all_action_ = nullptr;
    bool active_profile_is_built_in_ = true;
    bool active_profile_is_modified_ = false;
    bool active_profile_is_available_ = true;

    QCheckBox* webcam_enabled_check_ = nullptr;
    QComboBox* webcam_device_combo_ = nullptr;
    QLabel* webcam_info_label_ = nullptr;
    QPushButton* webcam_details_btn_ = nullptr;

    QLabel* lock_note_label_ = nullptr;
    bool controls_locked_ = false;

    QLabel* token_help_label_ = nullptr;
    QPushButton* token_help_toggle_btn_ = nullptr;
};

} // namespace exosnap
