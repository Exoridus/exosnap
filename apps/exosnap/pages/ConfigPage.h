#pragma once
#include <QWidget>

#include "../../../libs/capability/include/capability/audio_ui_state.h"
#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"
#include "../models/WebcamSettings.h"

#include <filesystem>
#include <string>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

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

  signals:
    void formatSettingsChanged(const OutputSettingsModel& settings);
    void activeProfileChanged(const QString& profile_id);
    void videoSettingsChanged(const VideoSettingsModel& settings);
    void audioSettingsChanged(const capability::AudioUiState& state);
    void webcamSettingsChanged(const WebcamSettings& settings);

  private:
    void onContainerChanged(int id);
    void onVideoCodecChanged(int index);
    void onAudioCodecChanged(int index);
    void onProfileSelectionChanged(int index);
    void onQualityChanged(int index);
    void onCfrChanged();
    void onCursorChanged();
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
    void emitCurrentAudioSettings();
    void updateAudioSourceAvailability();

    void onWebcamEnabledToggled();
    void onWebcamDeviceChanged(int index);
    void refreshWebcamDevices();
    void emitCurrentWebcamSettings();

    capability::AudioUiState audio_ui_state_;
    WebcamSettings webcam_settings_;

    OutputSettingsModel format_settings_;
    VideoSettingsModel video_settings_;
    QString active_profile_name_;
    std::vector<ProfileOption> profile_options_;

    QButtonGroup* container_group_ = nullptr;
    QRadioButton* mkv_radio_ = nullptr;
    QRadioButton* webm_radio_ = nullptr;
    QRadioButton* mp4_radio_ = nullptr;
    QComboBox* video_codec_combo_ = nullptr;
    QComboBox* audio_codec_combo_ = nullptr;
    QComboBox* profile_combo_ = nullptr;
    QLabel* format_display_label_ = nullptr;

    QComboBox* quality_combo_ = nullptr;
    QCheckBox* cfr_check_ = nullptr;
    QCheckBox* cursor_check_ = nullptr;

    QLabel* audio_summary_label_ = nullptr;

    QCheckBox* app_enabled_check_ = nullptr;
    QCheckBox* app_separate_check_ = nullptr;
    QLabel* app_source_label_ = nullptr;

    QCheckBox* mic_enabled_check_ = nullptr;
    QCheckBox* mic_separate_check_ = nullptr;
    QLabel* mic_source_label_ = nullptr;

    QCheckBox* sys_enabled_check_ = nullptr;
    QCheckBox* sys_separate_check_ = nullptr;
    QLabel* sys_source_label_ = nullptr;

    QLineEdit* destination_edit_ = nullptr;
    QPushButton* browse_btn_ = nullptr;
    QLineEdit* naming_edit_ = nullptr;
    QLabel* folder_validation_label_ = nullptr;
    QLabel* pattern_validation_label_ = nullptr;
    QLabel* example_filename_label_ = nullptr;

    QLabel* readiness_badge_label_ = nullptr;

    QCheckBox* webcam_enabled_check_ = nullptr;
    QComboBox* webcam_device_combo_ = nullptr;
    QLabel* webcam_info_label_ = nullptr;
};

} // namespace exosnap
