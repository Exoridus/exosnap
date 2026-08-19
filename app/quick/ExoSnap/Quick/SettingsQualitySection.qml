import QtQuick
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Quality & timing")

    ExoSettingRow {
        label: qsTr("Quality")
        hint: qsTr("Lower CQ means higher quality and larger files")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.qualityPresetOptions
            value: root.settings.qualityPreset
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Quality preset")
            onValueActivated: value => root.settings.qualityPreset = value
        }
    }

    ExoSettingRow {
        label: qsTr("Constant quality (CQ)")
        stacked: root.stacked
        visible: root.settings.expertMode
        Layout.fillWidth: true

        ExoNumberField {
            from: 1
            to: 51
            value: root.settings.cq
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Constant quality value")
            onValueCommitted: value => root.settings.cq = value
        }
    }

    ExoSettingRow {
        label: qsTr("Rate control")
        stacked: root.stacked
        visible: root.settings.expertMode
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.rateControlOptions
            value: root.settings.rateControl
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Rate control mode")
            onValueActivated: value => root.settings.rateControl = value
        }
    }

    ExoSettingRow {
        label: qsTr("Video bitrate")
        hint: qsTr("Target bitrate for VBR/CBR · ignored in CQ mode")
        stacked: root.stacked
        visible: root.settings.expertMode && root.settings.bitrateRelevant
        Layout.fillWidth: true

        ExoNumberField {
            from: 1000
            to: 400000
            stepSize: 1000
            suffix: qsTr("kbps")
            value: root.settings.bitrateKbps
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Video bitrate")
            onValueCommitted: value => root.settings.bitrateKbps = value
        }
    }

    ExoSettingRow {
        label: qsTr("Frame rate")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.frameRateOptions
            value: root.settings.frameRate
            visible: !root.settings.expertMode
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Frame rate")
            onValueActivated: value => root.settings.frameRate = value
        }

        ExoNumberField {
            from: 1
            to: Math.max(1, root.settings.maxFrameRate)
            suffix: qsTr("fps")
            value: root.settings.frameRate
            visible: root.settings.expertMode
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Frame rate")
            onValueCommitted: value => root.settings.frameRate = value
        }
    }

    ExoSettingRow {
        label: qsTr("Frame timing")
        hint: qsTr("Constant rate · editor-friendly")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.timingOptions
            value: root.settings.cfr ? 1 : 0
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Frame timing")
            onValueActivated: value => root.settings.cfr = value === 1
        }
    }

    ExoSettingRow {
        label: qsTr("Frame pacing")
        stacked: root.stacked
        visible: root.settings.expertMode
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.framePacingOptions
            value: root.settings.framePacing
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Frame pacing")
            onValueActivated: value => root.settings.framePacing = value
        }
    }

    ExoSettingRow {
        label: qsTr("Keyframe interval")
        hint: qsTr("Shorter intervals allow finer trim points")
        stacked: root.stacked
        visible: root.settings.expertMode
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.keyframeIntervalOptions
            value: root.settings.keyframeInterval
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Keyframe interval")
            onValueActivated: value => root.settings.keyframeInterval = value
        }
    }

    ExoSettingRow {
        label: qsTr("Capture cursor")
        hint: qsTr("Show the mouse pointer")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.captureCursor
            enabled: !root.settings.controlsLocked
            Accessible.name: qsTr("Capture cursor")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.captureCursor = value
        }
    }
}
