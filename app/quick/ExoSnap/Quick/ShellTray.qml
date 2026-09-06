import QtQuick
import Qt.labs.platform as Platform

// The notification-area icon and its menu.
//
// Everything this file decides is presentation: which row goes where, and which
// separator sits between them. What a row SAYS, whether it may be acted on and
// what a click on it means come from TrayAdapter, which reads the same appearance
// table the taskbar's thumbnail strip does. There is deliberately no
// `if (recording)` here -- a second place deriving when Pause is legal is the
// defect this project keeps finding.
//
// Every row is always present. A menu is read as the list of what the
// application can do, so a state that may not be acted on greys its row rather
// than removing it; only the blocked-reason caption comes and goes, because it
// exists only when there is a reason to name.
//
// Qt.labs.platform is a labs module and its types are not guaranteed to stay
// source-compatible across Qt versions. That is why the C++ seam beneath it is a
// plain model: a future incompatibility is a rewrite of this file alone.
Platform.SystemTrayIcon {
    id: root

    required property TrayAdapter tray

    visible: root.tray.active
    icon.source: root.tray.iconSource
    tooltip: root.tray.tooltip

    onActivated: (reason) => root.tray.handleActivation(reason)

    menu: Platform.Menu {
        // A native popup menu has no header item, so there is deliberately no
        // status caption here -- the icon and its tooltip already carry the
        // session's state. The blocked-reason row is the menu's only caption: a
        // sentence, not an action, so it is disabled and carries the caution
        // glyph rather than reading as a broken command.
        Platform.MenuItem {
            text: root.tray.blockedReason
            icon.source: root.tray.blockedReasonIcon
            visible: root.tray.blockedReasonVisible
            enabled: false
        }

        // Tied to the same row: with no status caption above it any more, an
        // unconditional separator here would be the menu's own first line
        // whenever nothing is blocked.
        Platform.MenuSeparator {
            visible: root.tray.blockedReasonVisible
        }

        Platform.MenuItem {
            text: root.tray.recordItem.text
            icon.source: root.tray.recordItem.icon
            enabled: root.tray.recordItem.enabled
            onTriggered: root.tray.triggerTransport(TrayAdapter.RecordRow)
        }

        Platform.MenuItem {
            text: root.tray.pauseResumeItem.text
            icon.source: root.tray.pauseResumeItem.icon
            enabled: root.tray.pauseResumeItem.enabled
            onTriggered: root.tray.triggerTransport(TrayAdapter.PauseResumeRow)
        }

        Platform.MenuItem {
            text: root.tray.stopItem.text
            icon.source: root.tray.stopItem.icon
            enabled: root.tray.stopItem.enabled
            onTriggered: root.tray.triggerTransport(TrayAdapter.StopRow)
        }

        Platform.MenuSeparator {}

        Platform.MenuItem {
            text: qsTr("Show window")
            icon.source: root.tray.showWindowIcon
            onTriggered: root.tray.triggerShowWindow()
        }

        Platform.MenuItem {
            text: qsTr("Open output folder")
            icon.source: root.tray.outputFolderIcon
            onTriggered: root.tray.triggerOpenOutputFolder()
        }

        // The count is the point of the entry: a toast raised while the window
        // was not on screen is otherwise invisible. The row stays without one
        // because it also opens the window, which is never the wrong offer.
        Platform.MenuItem {
            text: root.tray.notificationsText
            icon.source: root.tray.notificationsIcon
            onTriggered: root.tray.triggerNotifications()
        }

        Platform.MenuSeparator {}

        Platform.MenuItem {
            text: qsTr("Quit ExoSnap")
            icon.source: root.tray.quitIcon
            onTriggered: root.tray.triggerQuit()
        }
    }
}
