import QtQuick
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Recording format")
    subtitle: root.settings.formatSummary

    ExoSettingRow {
        label: qsTr("Container")
        hint: qsTr("MKV recommended · MP4 most compatible")
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
        info: qsTr("How much colour detail is kept beside the brightness detail. 4:2:0 stores colour at quarter resolution and is what every player and editor expects. 4:4:4 keeps colour at full resolution, which is visible on coloured text and thin interface lines, and needs 8-bit H.264 or HEVC.")
        warning: root.settings.chromaHint
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
        info: qsTr("What happens when the captured display is in HDR. Tone-mapping produces an SDR file that looks correct everywhere. Native HDR10 keeps the full range but needs HEVC or AV1 and a player that understands it.")
        warning: root.settings.hdrHint
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
