#pragma once
#include <QWidget>

#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"

#include <filesystem>
#include <string>
#include <vector>

class QButtonGroup;
class QComboBox;
class QLabel;
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
    void setProfileOptions(const std::vector<ProfileOption>& options, const QString& active_profile_id,
                           bool active_profile_modified);
    void setActiveProfileName(const QString& profile_name);

  signals:
    void formatSettingsChanged(const OutputSettingsModel& settings);
    void activeProfileChanged(const QString& profile_id);

  private:
    void onContainerChanged(int id);
    void onVideoCodecChanged(int index);
    void onAudioCodecChanged(int index);
    void onProfileSelectionChanged(int index);
    void emitCurrentFormatSettings();
    void reconcileContainerCodecRules();
    void updateVideoCodecChoices();
    void updateAudioCodecChoices();
    void updateFormatDisplay();

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

    QLabel* video_summary_label_ = nullptr;
    QLabel* audio_summary_label_ = nullptr;
    QLabel* output_folder_label_ = nullptr;
};

} // namespace exosnap
