#include "notifications/NotificationNames.h"

#include <array>
#include <utility>

namespace exosnap::notifications {
namespace {

// One table per enum, walked in both directions. A pair of switch statements
// would let the two directions disagree; a table cannot.
constexpr std::array<std::pair<NotificationType, const char*>, 19> kTypeNames{{
    {NotificationType::LowStorage, "lowStorage"},
    {NotificationType::Saved, "saved"},
    {NotificationType::UnexpectedStop, "unexpectedStop"},
    {NotificationType::RecoveryAvailable, "recoveryAvailable"},
    {NotificationType::UpdateAvailable, "updateAvailable"},
    {NotificationType::FramesDropped, "framesDropped"},
    {NotificationType::SettingsRepaired, "settingsRepaired"},
    {NotificationType::PresetSwitched, "presetSwitched"},
    {NotificationType::OverlayOmitted, "overlayOmitted"},
    {NotificationType::HotkeyConflict, "hotkeyConflict"},
    {NotificationType::SettingsSaveFailed, "settingsSaveFailed"},
    {NotificationType::AudioDefaultDeviceChanged, "audioDefaultDeviceChanged"},
    {NotificationType::AudioSourceDegraded, "audioSourceDegraded"},
    {NotificationType::CaptureActionFailed, "captureActionFailed"},
    {NotificationType::FrameCaptured, "frameCaptured"},
    {NotificationType::RecoveryProtectionUnavailable, "recoveryProtectionUnavailable"},
    {NotificationType::SettingsLoadFailed, "settingsLoadFailed"},
    {NotificationType::PresetTransferFailed, "presetTransferFailed"},
    {NotificationType::WindowCaptureStalled, "windowCaptureStalled"},
}};

constexpr std::array<std::pair<NotificationAction, const char*>, 13> kActionNames{{
    {NotificationAction::None, "none"},
    {NotificationAction::OpenFolder, "openFolder"},
    {NotificationAction::OpenRecovery, "openRecovery"},
    {NotificationAction::ChangeFolder, "changeFolder"},
    {NotificationAction::ShowFile, "showFile"},
    {NotificationAction::Discard, "discard"},
    {NotificationAction::OpenUpdate, "openUpdate"},
    {NotificationAction::Edit, "edit"},
    {NotificationAction::RelaunchElevated, "relaunchElevated"},
    {NotificationAction::OpenDiagnostics, "openDiagnostics"},
    {NotificationAction::UndoPresetSwitch, "undoPresetSwitch"},
    {NotificationAction::OpenHotkeys, "openHotkeys"},
    {NotificationAction::SendReport, "sendReport"},
}};

} // namespace

QString NotificationTypeName(NotificationType type) {
    for (const auto& [value, name] : kTypeNames) {
        if (value == type)
            return QString::fromLatin1(name);
    }
    return {};
}

QString NotificationActionName(NotificationAction action) {
    for (const auto& [value, name] : kActionNames) {
        if (value == action)
            return QString::fromLatin1(name);
    }
    return {};
}

bool ParseNotificationType(const QString& name, NotificationType* out) {
    for (const auto& [value, spelling] : kTypeNames) {
        if (name.compare(QLatin1String(spelling), Qt::CaseInsensitive) == 0) {
            if (out != nullptr)
                *out = value;
            return true;
        }
    }
    return false;
}

bool ParseNotificationAction(const QString& name, NotificationAction* out) {
    for (const auto& [value, spelling] : kActionNames) {
        if (name.compare(QLatin1String(spelling), Qt::CaseInsensitive) == 0) {
            if (out != nullptr)
                *out = value;
            return true;
        }
    }
    return false;
}

} // namespace exosnap::notifications
