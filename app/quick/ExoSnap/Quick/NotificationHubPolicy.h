#pragma once

#include "notifications/NotificationEvent.h"

#include <QString>

namespace exosnap::notifications {

// ---------------------------------------------------------------------------
// NotificationHubPolicy
// ---------------------------------------------------------------------------
// Pure, framework-free hub policy shared by every notification-hub surface.
// Nothing here touches QWidget or QQuick — only QString and the
// exosnap::notifications data types already used by NotificationEvent.h — so
// it is safe to call from either frontend and to unit-test without
// constructing a window.
//
// NOTE for the lead: this file lives under app/quick/ExoSnap/Quick/ only
// because the agent that wrote it (NOTIFY-QUICK-R1) was restricted to that
// directory. It has no Quick dependency at all and belongs beside
// AdvisoryStatusForType() in app/notifications/NotificationEvent.h — please
// move it there once both frontends can share one copy. Two call sites in
// app/ui/chrome/NotificationHubPanel.cpp and app/MainWindow.cpp currently
// carry their own inline versions of the first two functions below (see each
// function's doc comment for the exact line); once this header moves,
// point them at it instead of their local copies.
// ---------------------------------------------------------------------------

// Ranks a hub advisory status so the bell can report the WORST unread entry,
// not the newest — the product rule the bell's severity dot depends on
// (docs/product-spec.md §9: "takes its colour from the worst unread entry").
// Extracted verbatim from the local `rank` lambda inside
// NotificationHubPanel::worstUnreadStatus() (app/ui/chrome/NotificationHubPanel.cpp):
// same three rungs, same "info"/"success" tie at the bottom rung, same
// fallback for anything unrecognised. Kept in lockstep with
// AdvisoryStatusForType()'s four return strings just above in this header's
// sibling file.
[[nodiscard]] inline int AdvisoryStatusRank(const QString& status) {
    if (status == QStringLiteral("error"))
        return 3;
    if (status == QStringLiteral("caution"))
        return 2;
    return 1; // "info", "success", and anything unrecognised
}

// Collapses an event onto a stable hub identity: a standing condition (or the
// one-shot preset switch) keeps a single entry that is replaced in place each
// time it recurs, while every other event gets its own permanent entry keyed
// by sequence. This parallels — it does not extract, since the Widgets
// version is entangled with QWidget child/divider bookkeeping — the switch in
// MainWindow::recordEventInHub() (app/MainWindow.cpp): same four collapsed
// ids ("low-disk", "recovery-available", "preset-switched",
// "audio-source-degraded"), same "evt-<sequence>" fallback for everything
// else. UpdateAvailable additionally collapses onto "update-available" here
// (matching the id MainWindow's onUpdateCheckComplete() writes by hand,
// bypassing recordEventInHub entirely) so a Quick model built on this key
// alone behaves the same without needing that bypass — see
// NotificationsAdapter::removeEntryByKey() for the piece that still cannot be
// mechanical: nothing re-raises an event when a later update check comes back
// "up to date", so clearing that entry stays the composition root's job.
[[nodiscard]] inline QString NotificationHubEntryKey(const NotificationEvent& event) {
    switch (event.type) {
    case NotificationType::LowStorage:
        return QStringLiteral("low-disk");
    case NotificationType::RecoveryAvailable:
        return QStringLiteral("recovery-available");
    case NotificationType::PresetSwitched:
        return QStringLiteral("preset-switched");
    case NotificationType::AudioSourceDegraded:
        return QStringLiteral("audio-source-degraded");
    case NotificationType::UpdateAvailable:
        return QStringLiteral("update-available");
    default:
        return QStringLiteral("evt-%1").arg(event.sequence);
    }
}

// Button/label text for a NotificationAction. Not an extraction: the Widgets
// hub spells its action labels inline in MainWindow::recordEventInHub(),
// interleaved with its own opaque deep-link id scheme ("settings/output",
// "reveal:<path>", …) that a Quick surface has no use for — it routes actions
// through a typed signal instead (NotificationsAdapter::actionTriggered).
// This is a fresh mapping, but it deliberately reuses the exact words a user
// may already know from the Widgets hub/toasts/dispatchNotificationAction()
// wherever the intent is identical. Exhaustive switch, no default: adding a
// NotificationAction is a compile error here until its label is decided —
// the same safety property AdvisoryStatusForType() uses for NotificationType.
[[nodiscard]] inline QString NotificationActionLabel(NotificationAction action) {
    switch (action) {
    case NotificationAction::None:
        return QString();
    case NotificationAction::OpenFolder:
        return QStringLiteral("Show in folder");
    case NotificationAction::OpenRecovery:
        return QStringLiteral("Recover");
    case NotificationAction::ChangeFolder:
        return QStringLiteral("Change folder");
    case NotificationAction::ShowFile:
        return QStringLiteral("Show file");
    case NotificationAction::Discard:
        return QStringLiteral("Discard");
    case NotificationAction::OpenUpdate:
        return QStringLiteral("View update");
    case NotificationAction::Edit:
        return QStringLiteral("Edit");
    case NotificationAction::RelaunchElevated:
        return QStringLiteral("Restart as administrator");
    case NotificationAction::OpenDiagnostics:
        return QStringLiteral("View diagnostics");
    case NotificationAction::UndoPresetSwitch:
        return QStringLiteral("Undo");
    case NotificationAction::OpenHotkeys:
        return QStringLiteral("Rebind");
    }
    // Only reachable through a cast from an out-of-range value.
    return QString();
}

} // namespace exosnap::notifications
