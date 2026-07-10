#pragma once
#include <QWidget>

#include "../models/OutputSettingsModel.h"

#include <vector>

class QAction;
class QComboBox;
class QLabel;
class QPushButton;
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
    void saveAsNewRequested(const QString& name);
    void renameActiveProfileRequested(const QString& name);
    void deleteActiveProfileRequested();
    void resetActiveProfileRequested();
    void importProfilesRequested(const QString& file_path);
    void exportSelectedProfileRequested(const QString& file_path);

  private:
    void onProfileSelectionChanged(int index);
    // Renders combo-selection-derived UI state (status label, action
    // visibility) for the option at `index`. Never emits — shared by the
    // sync path (setProfileOptions) and the user-interaction path
    // (onProfileSelectionChanged), which emit differently.
    void applySelectionState(int index);
    void onImportProfiles();
    void onExportSelectedProfile();
    void onDeleteActiveProfile();
    void updateProfileActionState();
    void promptSaveAsNew();
    void promptRenameActiveProfile();

    bool active_profile_is_built_in_ = true;
    bool active_profile_is_modified_ = false;
    bool active_profile_is_available_ = true;
    std::vector<ProfileOption> profile_options_;
    QComboBox* profile_combo_ = nullptr;
    QLabel* profile_status_label_ = nullptr;
    QPushButton* reset_profile_btn_ = nullptr;
    QPushButton* save_as_new_btn_ = nullptr;
    QPushButton* delete_profile_btn_ = nullptr;
    QToolButton* profile_overflow_btn_ = nullptr;
    QAction* save_preset_as_action_ = nullptr;
    QAction* rename_profile_action_ = nullptr;
    QAction* import_profiles_action_ = nullptr;
    QAction* export_selected_action_ = nullptr;
};

} // namespace exosnap
