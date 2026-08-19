import QtQuick
import QtQuick.Layouts

// The four microphone DSP stages. Each stage's numeric parameter only appears
// while that stage is on, so an inactive stage never advertises a value the
// pipeline ignores.
ColumnLayout {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    spacing: ExoTheme.spacingSm

    ExoSettingRow {
        label: qsTr("High-pass filter")
        hint: qsTr("Removes low-frequency rumble below the cutoff frequency")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.micHpfEnabled
            enabled: !root.settings.controlsLocked
            Accessible.name: qsTr("Microphone high-pass filter")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.micHpfEnabled = value
        }
    }

    ExoSettingRow {
        label: qsTr("Cutoff frequency")
        stacked: root.stacked
        visible: root.settings.micHpfEnabled
        Layout.fillWidth: true
        Layout.leftMargin: ExoTheme.spacingLg

        ExoNumberField {
            from: 20
            to: 400
            stepSize: 10
            suffix: qsTr("Hz")
            value: Math.round(root.settings.micHpfCutoffHz)
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("High-pass cutoff frequency")
            onValueCommitted: value => root.settings.micHpfCutoffHz = value
        }
    }

    ExoSettingRow {
        label: qsTr("Noise gate")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.micGateEnabled
            enabled: !root.settings.controlsLocked
            Accessible.name: qsTr("Microphone noise gate")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.micGateEnabled = value
        }
    }

    ExoSettingRow {
        label: qsTr("Gate threshold")
        stacked: root.stacked
        visible: root.settings.micGateEnabled
        Layout.fillWidth: true
        Layout.leftMargin: ExoTheme.spacingLg

        ExoNumberField {
            from: -90
            to: 0
            suffix: qsTr("dB")
            value: Math.round(root.settings.micGateThresholdDb)
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Noise gate threshold")
            onValueCommitted: value => root.settings.micGateThresholdDb = value
        }
    }

    ExoSettingRow {
        label: qsTr("Automatic gain control")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.micAgcEnabled
            enabled: !root.settings.controlsLocked
            Accessible.name: qsTr("Microphone automatic gain control")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.micAgcEnabled = value
        }
    }

    ExoSettingRow {
        label: qsTr("AGC target loudness")
        stacked: root.stacked
        visible: root.settings.micAgcEnabled
        Layout.fillWidth: true
        Layout.leftMargin: ExoTheme.spacingLg

        ExoNumberField {
            from: -40
            to: 0
            suffix: qsTr("dB")
            value: Math.round(root.settings.micAgcTargetDb)
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Automatic gain control target")
            onValueCommitted: value => root.settings.micAgcTargetDb = value
        }
    }

    ExoSettingRow {
        label: qsTr("RNNoise suppression")
        hint: qsTr("Neural background-noise suppression")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.micRnnoiseEnabled
            enabled: !root.settings.controlsLocked
            Accessible.name: qsTr("RNNoise suppression")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.micRnnoiseEnabled = value
        }
    }
}
