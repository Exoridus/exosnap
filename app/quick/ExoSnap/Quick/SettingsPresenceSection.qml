import QtQuick
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    // The three capture-excluded overlays moved to SettingsOverlaysSection when
    // they gained content configuration, and the PresentMon opt-in to Developer:
    // an elevation-gated measurement probe is not presence. What stays is how the
    // app announces itself while it runs -- a toast, what happens when a recording
    // finishes, and the pair below: where the window goes when it is put away, and
    // whether it is in anyone's capture while it stays.
    title: qsTr("App behaviour")

    ExoSettingRow {
        label: qsTr("Notifications")
        hint: qsTr("Toasts for saved / low disk / stops")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.showNotifications
            Accessible.name: qsTr("Notifications")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.showNotifications = value
        }
    }

    ExoSettingRow {
        label: qsTr("Minimize ExoSnap to the system tray")
        hint: qsTr("Minimizing hides the window; the tray icon brings it back")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.minimizeToTray
            Accessible.name: qsTr("Minimize ExoSnap to the system tray")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.minimizeToTray = value
        }
    }

    ExoSettingRow {
        label: qsTr("Hide the ExoSnap window from screen capture")
        // The reach is the part the label cannot carry: this is a Windows
        // property of the window itself, so it applies to every capture on the
        // machine, not only ExoSnap's own.
        hint: qsTr("Applies to all capture software — calls, screen sharing, screenshots")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.hideWindowFromCapture
            Accessible.name: qsTr("Hide the ExoSnap window from screen capture")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.hideWindowFromCapture = value
        }
    }

    ExoSettingRow {
        label: qsTr("Open editor when finished")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.openEditorWhenFinished
            Accessible.name: qsTr("Open editor when finished")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.openEditorWhenFinished = value
        }
    }
}
