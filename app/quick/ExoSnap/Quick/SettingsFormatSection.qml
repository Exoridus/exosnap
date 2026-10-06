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
        visible: root.settings.expertMode && root.settings.bitDepthRelevant
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
        visible: root.settings.expertMode && root.settings.chromaRelevant
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
        visible: root.settings.hdrRelevant
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
        label: qsTr("Encoding device")
        hint: qsTr("Which GPU runs the encoder")
        warning: root.settings.encoderDeviceHint
        stacked: root.stacked
        visible: root.settings.expertMode && root.settings.encoderDeviceOptions.length > 0
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.encoderDeviceOptions
            value: root.settings.encoderDevice
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Encoding device")
            onValueActivated: value => root.settings.encoderDevice = value
        }
    }

    ExoSettingRow {
        label: qsTr("NVENC preset")
        hint: qsTr("Speed versus quality on NVIDIA GPUs (P1-P7)")
        stacked: root.stacked
        visible: root.settings.expertMode && root.settings.nvencPresetOptions.length > 0
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.nvencPresetOptions
            value: root.settings.nvencPreset
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("NVENC preset")
            onValueActivated: value => root.settings.nvencPreset = value
        }
    }

    ExoSettingRow {
        label: qsTr("B-frames")
        info: qsTr("Reordered frames can improve compression. They increase encoder buffering and may affect compatibility with players and editing tools.")
        warning: root.settings.nvencBframesHint
        stacked: root.stacked
        visible: root.settings.expertMode && root.settings.nvencAdvancedRelevant
        Layout.fillWidth: true
        ExoSelect {
            options: root.settings.nvencBframesOptions
            value: root.settings.nvencBframes
            enabled: !root.settings.controlsLocked && root.settings.nvencBframesOptions.length > 1
            Layout.fillWidth: true
            Accessible.name: qsTr("B-frames")
            onValueActivated: value => root.settings.nvencBframes = value
        }
    }

    ExoSettingRow {
        label: qsTr("B-frame reference mode")
        info: qsTr("Using B-frames as references may improve compression and increases dependencies within a GOP.")
        warning: root.settings.nvencBRefHint
        stacked: root.stacked
        visible: root.settings.expertMode && root.settings.nvencBRefRelevant
        Layout.fillWidth: true
        ExoSelect {
            options: root.settings.nvencBRefOptions
            value: root.settings.nvencBRef
            enabled: !root.settings.controlsLocked && root.settings.nvencBRefOptions.length > 1
            Layout.fillWidth: true
            Accessible.name: qsTr("B-frame reference mode")
            onValueActivated: value => root.settings.nvencBRef = value
        }
    }

    ExoSettingRow {
        label: qsTr("Lookahead")
        info: qsTr("Analyzes upcoming frames before encoding. It increases latency, GPU work and memory use.")
        warning: root.settings.nvencLookaheadHint
        stacked: root.stacked
        controlWidth: ExoTheme.controlSlotSwitch
        visible: root.settings.expertMode && root.settings.nvencAdvancedRelevant
        Layout.fillWidth: true
        ExoSwitch {
            checked: root.settings.nvencLookahead
            enabled: !root.settings.controlsLocked && root.settings.nvencLookaheadSupported
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            Accessible.name: qsTr("Lookahead")
            onToggledByUser: value => root.settings.nvencLookahead = value
        }
    }

    ExoSettingRow {
        label: qsTr("Lookahead depth")
        hint: qsTr("Frames buffered for analysis")
        stacked: root.stacked
        visible: root.settings.expertMode && root.settings.nvencLookahead
        Layout.fillWidth: true
        ExoNumberField {
            from: root.settings.nvencLookaheadMinDepth
            to: root.settings.nvencLookaheadMaxDepth
            value: root.settings.nvencLookaheadDepth
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Lookahead depth")
            onValueCommitted: value => root.settings.nvencLookaheadDepth = value
        }
    }

    ExoSettingRow {
        label: qsTr("Spatial AQ")
        info: qsTr("Redistributes quality within each frame. It uses GPU work and can trade small text detail for quality in other regions. It is off by default, including CQ.")
        warning: root.settings.nvencSpatialAqHint
        stacked: root.stacked
        controlWidth: ExoTheme.controlSlotSwitch
        visible: root.settings.expertMode && root.settings.nvencAdvancedRelevant
        Layout.fillWidth: true
        ExoSwitch {
            checked: root.settings.nvencSpatialAq
            enabled: !root.settings.controlsLocked && root.settings.nvencSpatialAqSupported
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            Accessible.name: qsTr("Spatial AQ")
            onToggledByUser: value => root.settings.nvencSpatialAq = value
        }
    }

    ExoSettingRow {
        label: qsTr("Temporal AQ")
        info: qsTr("Redistributes quality across frames according to motion. It uses GPU work and can increase bitrate variation.")
        warning: root.settings.nvencTemporalAqHint
        stacked: root.stacked
        controlWidth: ExoTheme.controlSlotSwitch
        visible: root.settings.expertMode && root.settings.nvencAdvancedRelevant
        Layout.fillWidth: true
        ExoSwitch {
            checked: root.settings.nvencTemporalAq
            enabled: !root.settings.controlsLocked && root.settings.nvencTemporalAqSupported
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            Accessible.name: qsTr("Temporal AQ")
            onToggledByUser: value => root.settings.nvencTemporalAq = value
        }
    }

    ExoSettingRow {
        label: qsTr("Multipass")
        info: qsTr("An extra analysis pass may improve bitrate allocation for VBR and CBR. Full-resolution analysis costs more GPU work than quarter-resolution analysis.")
        warning: root.settings.nvencMultipassHint
        stacked: root.stacked
        visible: root.settings.expertMode && root.settings.nvencMultipassRelevant
        Layout.fillWidth: true
        ExoSelect {
            options: root.settings.nvencMultipassOptions
            value: root.settings.nvencMultipass
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Multipass")
            onValueActivated: value => root.settings.nvencMultipass = value
        }
    }
}
