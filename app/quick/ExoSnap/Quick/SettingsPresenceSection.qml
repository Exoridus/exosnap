import QtQuick
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("App behaviour")

    ExoSettingRow {
        label: qsTr("Notifications")
        hint: qsTr("Saved, low disk space, stopped")
        stacked: root.stacked
        controlWidth: ExoTheme.controlSlotSwitch
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
        stacked: root.stacked
        controlWidth: ExoTheme.controlSlotSwitch
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
        hint: qsTr("Hidden in calls, screen sharing and screenshots")
        stacked: root.stacked
        controlWidth: ExoTheme.controlSlotSwitch
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.hideWindowFromCapture
            Accessible.name: qsTr("Hide the ExoSnap window from screen capture")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.hideWindowFromCapture = value
        }
    }
}
