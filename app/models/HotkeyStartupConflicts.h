#pragma once

// Policy for hotkeys that could not be registered with Windows at startup.
//
// GlobalHotkeyService::SetRegistrar deliberately reports structured failures
// instead of logging or notifying, so somebody above it has to decide what a
// failure means. That decision is identical for every frontend, and it is not
// obvious enough to re-derive per frontend:
//
//  - Windows offers no way to learn which process holds a combo, and no way to
//    take it. A binding that failed to register is dead weight -- it would
//    swallow nothing, do nothing, and re-warn on every launch -- so it is
//    dropped rather than kept.
//  - A binding the user chose themselves worked when they chose it, so losing it
//    is worth telling them about, with a Rebind notification that deep-links to
//    Settings -> Hotkeys.
//  - A binding still at its shipped default is dropped SILENTLY. The user never
//    asked for that combination, and a machine where NVIDIA or another tool
//    already holds it would otherwise greet every launch with a warning about a
//    shortcut its owner never picked. The log line remains, for a report.
//
// The default/custom split below has to be computed BEFORE anything is unset:
// unsetting clears the binding, after which every action reads as "at default"
// and the distinction is gone.

#include "services/GlobalHotkeyService.h"

#include <vector>

namespace exosnap::models {

struct HotkeyStartupConflicts {
    // Failed to register and were at their default binding: log only.
    std::vector<HotkeyAction> default_failed;
    // Failed to register and were user-chosen: notify.
    std::vector<HotkeyAction> custom_failed;

    [[nodiscard]] bool empty() const noexcept {
        return default_failed.empty() && custom_failed.empty();
    }
};

// `failed` is SetRegistrar's return value; `service` must not have been mutated
// since, so the at-default test still reflects what the user had.
[[nodiscard]] inline HotkeyStartupConflicts ClassifyHotkeyStartupConflicts(const std::vector<HotkeyAction>& failed,
                                                                           const GlobalHotkeyService& service) {
    HotkeyStartupConflicts conflicts;
    for (const HotkeyAction action : failed) {
        if (service.IsAtDefault(action))
            conflicts.default_failed.push_back(action);
        else
            conflicts.custom_failed.push_back(action);
    }
    return conflicts;
}

// Body copy for the "your shortcut is gone" notification. Singular/plural is
// part of the wording decision, not of the caller's formatting.
[[nodiscard]] inline QString HotkeyConflictNotificationBody(const QStringList& custom_action_names) {
    const QString joined = custom_action_names.join(QStringLiteral(", "));
    return custom_action_names.size() > 1
               ? QStringLiteral("%1 were already in use by Windows or another app, so ExoSnap removed them. "
                                "Pick new shortcuts to re-enable them.")
                     .arg(joined)
               : QStringLiteral("%1 was already in use by Windows or another app, so ExoSnap removed it. "
                                "Pick a new shortcut to re-enable it.")
                     .arg(joined);
}

} // namespace exosnap::models
