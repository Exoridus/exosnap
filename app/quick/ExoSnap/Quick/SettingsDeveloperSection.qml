import QtQuick
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Developer")

    ExoSettingRow {
        label: qsTr("Log level")
        hint: qsTr("Narrows what is recorded in the application log")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.logLevelOptions
            value: root.settings.developerLogLevel
            Layout.fillWidth: true
            Accessible.name: qsTr("Log level")
            onValueActivated: value => root.settings.developerLogLevel = value
        }
    }

    ExoSettingRow {
        label: qsTr("Crash reports")
        hint: qsTr("Reports are privacy-scrubbed and never sent without consent")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.crashReportPolicyOptions
            value: root.settings.crashReportPolicy
            Layout.fillWidth: true
            Accessible.name: qsTr("Crash report policy")
            onValueActivated: value => root.settings.crashReportPolicy = value
        }
    }

    ExoButton {
        text: qsTr("Open Diagnostics")
        quiet: true
        Layout.alignment: Qt.AlignLeft
        onClicked: root.settings.openDiagnostics()
    }
}
