#pragma once

#include <QObject>
#include <QString>

#include "SetupBridge.h"

namespace exosnap::setup {

class SetupController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString page READ page NOTIFY changed FINAL)
    Q_PROPERTY(QString versionText READ versionText NOTIFY changed FINAL)
    Q_PROPERTY(QString headline READ headline NOTIFY changed FINAL)
    Q_PROPERTY(QString description READ description NOTIFY changed FINAL)
    Q_PROPERTY(QString detail READ detail NOTIFY changed FINAL)
    Q_PROPERTY(QString primaryText READ primaryText NOTIFY changed FINAL)
    Q_PROPERTY(QString secondaryText READ secondaryText NOTIFY changed FINAL)
    Q_PROPERTY(QString statusTone READ statusTone NOTIFY changed FINAL)
    Q_PROPERTY(QString installPath READ installPath CONSTANT FINAL)
    Q_PROPERTY(QString licenseText READ licenseText CONSTANT FINAL)
    Q_PROPERTY(bool decision READ decision NOTIFY changed FINAL)
    Q_PROPERTY(bool closeEnabled READ closeEnabled NOTIFY changed FINAL)
    Q_PROPERTY(bool cancelConfirmationVisible READ cancelConfirmationVisible NOTIFY changed FINAL)
    Q_PROPERTY(bool passive READ passive CONSTANT FINAL)
    Q_PROPERTY(bool indeterminate READ indeterminate NOTIFY changed FINAL)
    Q_PROPERTY(bool desktopShortcut READ desktopShortcut WRITE setDesktopShortcut NOTIFY changed FINAL)
    Q_PROPERTY(bool removeUserData READ removeUserData WRITE setRemoveUserData NOTIFY changed FINAL)
    Q_PROPERTY(qreal progress READ progress NOTIFY changed FINAL)

  public:
    explicit SetupController(QString version, QString install_path, bool passive = false,
                             bool uninstall_requested = false, bool repair_requested = false);
    void receive(const SetupEvent& event);
    void setLicenseText(QString text);
    [[nodiscard]] QString page() const;
    [[nodiscard]] QString versionText() const;
    [[nodiscard]] QString headline() const;
    [[nodiscard]] QString description() const;
    [[nodiscard]] QString detail() const;
    [[nodiscard]] QString primaryText() const;
    [[nodiscard]] QString secondaryText() const;
    [[nodiscard]] QString statusTone() const;
    [[nodiscard]] QString installPath() const;
    [[nodiscard]] QString licenseText() const;
    [[nodiscard]] bool decision() const;
    [[nodiscard]] bool closeEnabled() const;
    [[nodiscard]] bool cancelConfirmationVisible() const;
    [[nodiscard]] bool passive() const;
    [[nodiscard]] bool indeterminate() const;
    [[nodiscard]] bool desktopShortcut() const;
    [[nodiscard]] bool removeUserData() const;
    [[nodiscard]] qreal progress() const;
    void setDesktopShortcut(bool enabled);
    void setRemoveUserData(bool enabled);
    Q_INVOKABLE void activatePrimary();
    Q_INVOKABLE void activateSecondary();
    Q_INVOKABLE void chooseRepair();
    Q_INVOKABLE void chooseUninstall();
    Q_INVOKABLE bool requestClose();
    Q_INVOKABLE void confirmCancel();
    Q_INVOKABLE void dismissCancel();

  signals:
    void changed();
    void applyRequested(SetupAction action, bool desktop_shortcut, bool remove_user_data);
    void cancelRequested();
    void launchRequested();
    void restartRequested();
    void openLogRequested();
    void finished(std::uint32_t exit_code);

  private:
    void begin(SetupAction action);
    QString version_;
    QString installed_version_;
    QString install_path_;
    QString license_;
    QString page_ = QStringLiteral("detecting");
    QString detail_;
    QString package_;
    SetupAction action_ = SetupAction::Install;
    std::uint32_t exit_code_ = 0;
    int percent_ = 0;
    bool passive_ = false;
    bool uninstall_requested_ = false;
    bool repair_requested_ = false;
    bool installed_ = false;
    bool can_cancel_ = false;
    bool measured_ = false;
    bool cancelling_ = false;
    bool desktop_shortcut_ = false;
    bool remove_user_data_ = false;
    bool restart_pending_ = false;
    bool cancel_confirmation_ = false;
    bool files_in_use_ = false;
    bool launch_failed_ = false;
};

} // namespace exosnap::setup
