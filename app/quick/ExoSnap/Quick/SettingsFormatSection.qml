import QtQuick
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Container & codecs")
    subtitle: root.settings.formatSummary

    ExoSettingRow {
        label: qsTr("Container")
        hint: qsTr("MKV safest · MP4 most compatible")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.containerOptions
            value: root.settings.container
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Container")
            onValueActivated: value => root.settings.container = value
        }
    }

    ExoSettingRow {
        label: qsTr("Video codec")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.videoCodecOptions
            value: root.settings.videoCodec
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Video codec")
            onValueActivated: value => root.settings.videoCodec = value
        }
    }

    ExoSettingRow {
        label: qsTr("Audio codec")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.audioCodecOptions
            value: root.settings.audioCodec
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Audio codec")
            onValueActivated: value => root.settings.audioCodec = value
        }
    }

    ExoNotice {
        text: root.settings.compatNotice
        visible: !root.settings.compatOk
        Layout.fillWidth: true
    }

    ExoSettingRow {
        label: qsTr("Video bit depth")
        stacked: root.stacked
        visible: root.settings.expertMode
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.bitDepthOptions
            value: root.settings.bitDepth
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Video bit depth")
            onValueActivated: value => root.settings.bitDepth = value
        }
    }

    ExoSettingRow {
        label: qsTr("Chroma subsampling")
        hint: root.settings.chromaHint
        stacked: root.stacked
        visible: root.settings.expertMode
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.chromaOptions
            value: root.settings.chroma
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Chroma subsampling")
            onValueActivated: value => root.settings.chroma = value
        }
    }

    ExoSettingRow {
        label: qsTr("Colour range")
        stacked: root.stacked
        visible: root.settings.expertMode
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.colorRangeOptions
            value: root.settings.colorRange
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Colour range")
            onValueActivated: value => root.settings.colorRange = value
        }
    }

    ExoSettingRow {
        label: qsTr("HDR handling")
        hint: root.settings.hdrHint
        stacked: root.stacked
        visible: root.settings.expertMode && root.settings.hdrRelevant
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.hdrModeOptions
            value: root.settings.hdrMode
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("HDR handling")
            onValueActivated: value => root.settings.hdrMode = value
        }
    }

    ExoSettingRow {
        label: qsTr("Encoder preset")
        hint: qsTr("Speed versus quality inside the encoder")
        stacked: root.stacked
        visible: root.settings.expertMode
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.encoderPresetOptions
            value: root.settings.encoderPreset
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Encoder preset")
            onValueActivated: value => root.settings.encoderPreset = value
        }
    }
}
