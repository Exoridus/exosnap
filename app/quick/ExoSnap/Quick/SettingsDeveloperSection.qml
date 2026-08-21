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
        hint: qsTr("Privacy-scrubbed, never sent without consent")
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

    ExoSettingRow {
        label: qsTr("Present, tearing & latency diagnostics")
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

    // A row, not a loose button: every other control in this card sits on the
    // right-hand control axis, and an action parked at the left margin reads as
    // a stray link under the form rather than as part of it.
    ExoSettingRow {
        label: qsTr("Diagnostics")
        hint: qsTr("Inspect capture and system health")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoButton {
            text: qsTr("Open Diagnostics")
            Layout.alignment: Qt.AlignRight
            onClicked: root.settings.openDiagnostics()
        }
    }
}
