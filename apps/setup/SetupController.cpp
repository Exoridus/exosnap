#include "SetupController.h"

#include <algorithm>
#include <utility>

namespace exosnap::setup {

namespace {

std::uint32_t ExitCode(std::int32_t status) {
    const auto value = static_cast<std::uint32_t>(status);
    return (value & 0xffff0000U) == 0x80070000U ? value & 0xffffU : value;
}

} // namespace

SetupController::SetupController(QString version, QString install_path, bool passive, bool uninstall_requested,
                                 bool repair_requested)
    : version_(std::move(version)), install_path_(std::move(install_path)), passive_(passive),
      uninstall_requested_(uninstall_requested), repair_requested_(repair_requested) {
    action_ = uninstall_requested_ ? SetupAction::Uninstall
              : repair_requested_  ? SetupAction::Repair
                                   : SetupAction::Install;
}

void SetupController::receive(const SetupEvent& event) {
    switch (event.kind) {
    case SetupEventKind::Detected:
        if (event.status != 0) {
            page_ = QStringLiteral("failure");
            exit_code_ = ExitCode(event.status);
            break;
        }
        installed_ = event.installed;
        installed_version_ = QString::fromWCharArray(event.version ? event.version : L"");
        page_ = passive_               ? QStringLiteral("applying")
                : !installed_          ? QStringLiteral("install")
                : uninstall_requested_ ? QStringLiteral("uninstall")
                                       : QStringLiteral("manage");
        if (repair_requested_ && installed_ && !passive_) {
            begin(SetupAction::Repair);
        }
        break;
    case SetupEventKind::Applying:
        page_ = QStringLiteral("applying");
        can_cancel_ = event.can_cancel;
        break;
    case SetupEventKind::Progress:
        percent_ = static_cast<int>(std::min(event.progress, 100U));
        measured_ = true;
        can_cancel_ = event.can_cancel;
        break;
    case SetupEventKind::PackageBegin:
        package_ = QString::fromWCharArray(event.text ? event.text : L"");
        can_cancel_ = event.can_cancel;
        break;
    case SetupEventKind::PackageComplete:
        can_cancel_ = event.can_cancel;
        break;
    case SetupEventKind::Error:
        detail_ = QString::fromWCharArray(event.text ? event.text : L"");
        launch_failed_ = page_ == QStringLiteral("success");
        break;
    case SetupEventKind::FilesInUse:
        files_in_use_ = true;
        detail_ = QString::fromWCharArray(event.text ? event.text : L"");
        break;
    case SetupEventKind::Completed:
        exit_code_ = ExitCode(event.status);
        can_cancel_ = false;
        restart_pending_ = event.restart_required;
        if ((event.restart_required && exit_code_ == 0) || exit_code_ == 3010U || exit_code_ == 1641U) {
            exit_code_ = 3010U;
            page_ = QStringLiteral("restart");
        } else if (exit_code_ == 1602U && !files_in_use_) {
            page_ = QStringLiteral("cancelled");
        } else {
            page_ = exit_code_ == 0 ? QStringLiteral("success") : QStringLiteral("failure");
        }
        if (passive_) {
            emit finished(exit_code_);
        }
        break;
    }
    if (page_ != QStringLiteral("applying") || !can_cancel_)
        cancel_confirmation_ = false;
    emit changed();
}

void SetupController::setLicenseText(QString text) {
    license_ = std::move(text);
}
QString SetupController::page() const {
    return page_;
}
QString SetupController::installPath() const {
    return install_path_;
}
QString SetupController::licenseText() const {
    return license_;
}
QString SetupController::detail() const {
    return detail_.isEmpty() && exit_code_ != 0 ? tr("Windows Installer returned error %1.").arg(exit_code_) : detail_;
}
bool SetupController::passive() const {
    return passive_;
}
bool SetupController::indeterminate() const {
    return !measured_;
}
bool SetupController::desktopShortcut() const {
    return desktop_shortcut_;
}
bool SetupController::removeUserData() const {
    return remove_user_data_;
}
bool SetupController::cancelConfirmationVisible() const {
    return cancel_confirmation_;
}
qreal SetupController::progress() const {
    return percent_ / 100.0;
}
bool SetupController::decision() const {
    return !passive_ && (page_ == QStringLiteral("install") || page_ == QStringLiteral("manage") ||
                         page_ == QStringLiteral("uninstall"));
}
bool SetupController::closeEnabled() const {
    return !passive_ && page_ != QStringLiteral("detecting") &&
           (page_ != QStringLiteral("applying") || (can_cancel_ && !cancelling_));
}
QString SetupController::versionText() const {
    if (!installed_ && !installed_version_.isEmpty())
        return tr("%1 \u2192 %2").arg(installed_version_, version_);
    return installed_ && decision() && page_ != QStringLiteral("install")
               ? tr("%1 installed").arg(installed_version_.isEmpty() ? version_ : installed_version_)
               : version_;
}
QString SetupController::headline() const {
    if (page_ == QStringLiteral("install"))
        return installed_version_.isEmpty() ? tr("Install ExoSnap") : tr("Update ExoSnap");
    if (page_ == QStringLiteral("manage"))
        return tr("Manage ExoSnap");
    if (page_ == QStringLiteral("uninstall"))
        return tr("Uninstall ExoSnap");
    if (page_ == QStringLiteral("applying")) {
        if (action_ == SetupAction::Uninstall)
            return tr("Uninstalling ExoSnap");
        return action_ == SetupAction::Repair ? tr("Repairing ExoSnap") : tr("Installing ExoSnap");
    }
    if (page_ == QStringLiteral("success")) {
        if (action_ == SetupAction::Uninstall)
            return tr("ExoSnap was uninstalled");
        return action_ == SetupAction::Repair ? tr("ExoSnap was repaired") : tr("ExoSnap is installed");
    }
    if (page_ == QStringLiteral("restart"))
        return tr("Restart to finish");
    if (page_ == QStringLiteral("cancelled"))
        return tr("Setup was cancelled");
    if (page_ == QStringLiteral("failure"))
        return tr("Setup didn't complete");
    return tr("Checking installation");
}
QString SetupController::description() const {
    if (page_ == QStringLiteral("install"))
        return tr("For everyone on this computer. Everything Setup needs is included.");
    if (page_ == QStringLiteral("uninstall"))
        return tr(
            "Removes the app, its Start menu entry and its shortcut. Recordings and exported files are never touched.");
    if (page_ == QStringLiteral("applying")) {
        if (cancelling_)
            return tr("Cancelling. Waiting for Windows Installer to finish safely.");
        if (!can_cancel_)
            return percent_ >= 90 ? tr("Finishing up. This step can't be interrupted.")
                                  : tr("This step can't be interrupted.");
        if (package_ == QStringLiteral("VCRedistX64"))
            return tr("Preparing Microsoft Visual C++ Runtime");
        return tr("Preparing program files");
    }
    if (page_ == QStringLiteral("success")) {
        if (launch_failed_)
            return tr("ExoSnap is installed but couldn't be started. Launch it from the Start menu.");
        if (action_ == SetupAction::Uninstall)
            return remove_user_data_ ? tr("Your ExoSnap data was deleted. Recordings were kept.")
                                     : tr("Your ExoSnap data and recordings were kept.");
        return tr("You'll find it in the Start menu.");
    }
    if (page_ == QStringLiteral("restart"))
        return tr("Restart Windows to complete the operation.");
    if (page_ == QStringLiteral("cancelled"))
        return tr("Setup stopped. Recordings and exported files were kept.");
    if (page_ == QStringLiteral("failure") && files_in_use_)
        return tr("Close ExoSnap and any apps using its files, then try Setup again.");
    if (page_ == QStringLiteral("failure"))
        return restart_pending_
                   ? tr("Windows Installer stopped with error %1. Restart Windows before trying again.").arg(exit_code_)
                   : tr("Windows Installer stopped with error %1.").arg(exit_code_);
    return {};
}
QString SetupController::primaryText() const {
    if (passive_)
        return {};
    if (page_ == QStringLiteral("install"))
        return installed_version_.isEmpty() ? tr("Install") : tr("Update");
    if (page_ == QStringLiteral("uninstall"))
        return remove_user_data_ ? tr("Uninstall and delete data") : tr("Uninstall");
    if (page_ == QStringLiteral("success") && action_ != SetupAction::Uninstall)
        return tr("Launch ExoSnap");
    if (page_ == QStringLiteral("restart"))
        return tr("Restart now");
    return {};
}
QString SetupController::secondaryText() const {
    if (passive_)
        return {};
    if (page_ == QStringLiteral("uninstall"))
        return tr("Back");
    if (page_ == QStringLiteral("restart"))
        return tr("Later");
    if (page_ == QStringLiteral("install") || (page_ == QStringLiteral("applying") && closeEnabled()))
        return tr("Cancel");
    if (page_ != QStringLiteral("applying") && page_ != QStringLiteral("detecting"))
        return tr("Close");
    return {};
}
QString SetupController::statusTone() const {
    if (page_ == QStringLiteral("failure"))
        return QStringLiteral("error");
    if (page_ == QStringLiteral("restart"))
        return QStringLiteral("warning");
    if (page_ == QStringLiteral("success") && action_ != SetupAction::Uninstall)
        return QStringLiteral("success");
    return QStringLiteral("neutral");
}
void SetupController::setDesktopShortcut(bool enabled) {
    if (page_ == QStringLiteral("install") && desktop_shortcut_ != enabled) {
        desktop_shortcut_ = enabled;
        emit changed();
    }
}
void SetupController::setRemoveUserData(bool enabled) {
    if (page_ == QStringLiteral("uninstall") && remove_user_data_ != enabled) {
        remove_user_data_ = enabled;
        emit changed();
    }
}
void SetupController::begin(SetupAction action) {
    action_ = action;
    page_ = QStringLiteral("applying");
    can_cancel_ = false;
    measured_ = false;
    cancelling_ = false;
    detail_.clear();
    emit changed();
    emit applyRequested(action, desktop_shortcut_, action == SetupAction::Uninstall && remove_user_data_);
}
void SetupController::activatePrimary() {
    if (passive_)
        return;
    if (page_ == QStringLiteral("install"))
        begin(SetupAction::Install);
    else if (page_ == QStringLiteral("uninstall"))
        begin(SetupAction::Uninstall);
    else if (page_ == QStringLiteral("success") && action_ != SetupAction::Uninstall)
        emit launchRequested();
    else if (page_ == QStringLiteral("restart"))
        emit restartRequested();
}
void SetupController::activateSecondary() {
    if (page_ == QStringLiteral("uninstall") && !passive_) {
        remove_user_data_ = false;
        page_ = QStringLiteral("manage");
        emit changed();
    } else {
        requestClose();
    }
}
void SetupController::chooseRepair() {
    if (page_ == QStringLiteral("manage") && !passive_)
        begin(SetupAction::Repair);
}
void SetupController::chooseUninstall() {
    if (page_ == QStringLiteral("manage") && !passive_) {
        remove_user_data_ = false;
        page_ = QStringLiteral("uninstall");
        emit changed();
    }
}
bool SetupController::requestClose() {
    if (!closeEnabled())
        return false;
    if (page_ == QStringLiteral("applying")) {
        cancel_confirmation_ = true;
        emit changed();
        return false;
    }
    emit finished(exit_code_);
    return true;
}

void SetupController::confirmCancel() {
    if (!cancel_confirmation_)
        return;
    cancel_confirmation_ = false;
    if (page_ == QStringLiteral("applying") && closeEnabled()) {
        cancelling_ = true;
        emit cancelRequested();
    }
    emit changed();
}

void SetupController::dismissCancel() {
    cancel_confirmation_ = false;
    emit changed();
}

} // namespace exosnap::setup
