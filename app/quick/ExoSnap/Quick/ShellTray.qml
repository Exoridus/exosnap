import QtQuick
import Qt.labs.platform as Platform

// The notification-area icon and its menu.
//
// Everything this file decides is presentation: which row goes where, and which
// separator sits between them. What a row SAYS, whether it is offered at all and
// what a click on it means come from TrayAdapter, which reads the same appearance
// table the taskbar's thumbnail strip does. There is deliberately no
// `if (recording)` here -- a second place deriving when Pause is legal is the
// defect this project keeps finding.
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
        Platform.MenuItem {
            text: root.tray.showHideText
            onTriggered: root.tray.triggerShowHide()
        }

        Platform.MenuSeparator {}

        Platform.MenuItem {
            text: root.tray.recordItem.text
            icon.source: root.tray.recordItem.icon
            visible: root.tray.recordItem.visible
            enabled: root.tray.recordItem.enabled
            onTriggered: root.tray.triggerTransport(TrayAdapter.RecordRow)
        }

        Platform.MenuItem {
            text: root.tray.pauseResumeItem.text
            icon.source: root.tray.pauseResumeItem.icon
            visible: root.tray.pauseResumeItem.visible
            enabled: root.tray.pauseResumeItem.enabled
            onTriggered: root.tray.triggerTransport(TrayAdapter.PauseResumeRow)
        }

        Platform.MenuItem {
            text: root.tray.stopItem.text
            icon.source: root.tray.stopItem.icon
            visible: root.tray.stopItem.visible
            enabled: root.tray.stopItem.enabled
            onTriggered: root.tray.triggerTransport(TrayAdapter.StopRow)
        }

        Platform.MenuSeparator {}

        Platform.MenuItem {
            text: qsTr("Open output folder")
            onTriggered: root.tray.triggerOpenOutputFolder()
        }

        // Hidden until there is something unread. The count is the whole point of
        // the entry: a toast raised while the window was not on screen is
        // otherwise invisible.
        Platform.MenuItem {
            text: root.tray.notificationsText
            visible: root.tray.notificationsVisible
            onTriggered: root.tray.triggerNotifications()
        }

        Platform.MenuSeparator {}

        Platform.MenuItem {
            text: qsTr("Quit ExoSnap")
            onTriggered: root.tray.triggerQuit()
        }
    }
}
