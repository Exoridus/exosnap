import QtQuick
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    // The three capture-excluded overlays moved to SettingsOverlaysSection when
    // they gained content configuration, and the PresentMon opt-in to Developer:
    // an elevation-gated measurement probe is not presence. What stays is how the
    // app announces itself while it runs -- a toast, the tray, and what happens
    // when a recording finishes.
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
        label: qsTr("Keep running in tray")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.keepRunningInTray
            Accessible.name: qsTr("Keep running in tray")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.keepRunningInTray = value
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
