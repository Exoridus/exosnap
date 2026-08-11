import QtQuick
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Appearance")

    ExoSettingRow {
        label: qsTr("Theme")
        hint: qsTr("App highlight colour and surface palette")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.themeOptions
            value: root.settings.themeId
            Layout.fillWidth: true
            Accessible.name: qsTr("Theme")
            onValueActivated: value => root.settings.themeId = value
        }
    }
}
