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
        // Captions, not actions: disabled is what makes the platform draw them
        // as text rather than as something to click. No glyph either -- an icon
        // column beside a caption reads as an entry that failed to load one.
        Platform.MenuItem {
            text: root.tray.statusText
            enabled: false
        }

        Platform.MenuItem {
            text: root.tray.blockedReason
            visible: root.tray.blockedReasonVisible
            enabled: false
        }

        Platform.MenuSeparator {}

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
