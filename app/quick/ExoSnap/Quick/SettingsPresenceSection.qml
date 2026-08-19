import QtQuick
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    // The three capture-excluded overlays moved to SettingsOverlaysSection when
    // they gained content configuration. What stays here is presence in the
    // Windows shell and in the app window itself — a toast, the tray, and where
    // a finished recording lands.
    title: qsTr("Notifications & presence")

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
        hint: qsTr("Keep running when window closed")
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

    ExoSettingRow {
        label: qsTr("Present & tearing diagnostics")
        hint: qsTr("Elevation-gated PresentMon observation · opt-in")
        stacked: root.stacked
        controlWidth: 60
        visible: root.settings.expertMode
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.presentDiagnosticsOptIn
            Accessible.name: qsTr("Present and tearing diagnostics")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.presentDiagnosticsOptIn = value
        }
    }
}
