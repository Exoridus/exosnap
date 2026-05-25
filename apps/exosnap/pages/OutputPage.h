#pragma once
#include <QWidget>

#include "../models/OutputSettingsModel.h"

#include <vector>

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QAction;
class QToolButton;

namespace exosnap {

class OutputPage : public QWidget {
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

    explicit OutputPage(const OutputSettingsModel& initial_settings, QWidget* parent = nullptr);

    void setOutputSettings(const OutputSettingsModel& settings);
    void setProfileOptions(const std::vector<ProfileOption>& options, const QString& active_profile_id,
                           bool active_profile_modified);
    void setActiveProfileName(const QString& profile_name);
    [[nodiscard]] QString activeProfileId() const;

  signals:
    void outputSettingsChanged(const OutputSettingsModel& settings);
    void activeProfileChanged(const QString& profile_id);
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

  private:
    void onContainerChanged(int id);
    void onVideoCodecChanged(int index);
    void onAudioCodecChanged(int index);
    void onBrowse();
    void onDestinationEditingFinished();
    void onPatternEditingFinished();
    void onProfileSelectionChanged(int index);
    void onImportProfiles();
    void onExportSelectedProfile();
    void onExportAllUserProfiles();
    void onResetAllSettingsAndProfiles();
    void onDeleteActiveProfile();
    void applySettingsToUi();
    void emitCurrentSettings();
    void reconcileContainerCodecRules();
    void updateAudioCodecChoices();
    void updateVideoCodecChoices();
    void updateEffectiveOutputPreview();
    void updateValidationState();
    void updateProfileActionState();
    void promptCreateProfileFromCurrent();
    void promptCreateProfileFromSafeDefault();
    void promptRenameActiveProfile();
    void promptSaveModifiedBuiltInAsNew();

    OutputSettingsModel settings_;
    std::filesystem::path last_valid_output_folder_;
    std::wstring last_valid_naming_pattern_;
    QString active_profile_name_;
    bool active_profile_is_built_in_ = true;
    bool active_profile_is_modified_ = false;
    bool active_profile_is_available_ = true;
    std::vector<ProfileOption> profile_options_;
    QComboBox* profile_combo_ = nullptr;
    QLabel* profile_status_label_ = nullptr;
    QPushButton* reset_profile_btn_ = nullptr;
    QPushButton* save_as_new_btn_ = nullptr;
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
    QButtonGroup* container_group_ = nullptr;
    QRadioButton* mkv_radio_ = nullptr;
    QRadioButton* webm_radio_ = nullptr;
    QRadioButton* mp4_radio_ = nullptr;
    QComboBox* video_codec_combo_ = nullptr;
    QComboBox* audio_codec_combo_ = nullptr;
    QLineEdit* destination_edit_ = nullptr;
    QLineEdit* naming_edit_ = nullptr;
    QLabel* folder_validation_label_ = nullptr;
    QLabel* pattern_validation_label_ = nullptr;
    QLabel* mp4_info_label_ = nullptr;
    QLabel* effective_output_path_label_ = nullptr;
    QPushButton* browse_btn_ = nullptr;
};

} // namespace exosnap
