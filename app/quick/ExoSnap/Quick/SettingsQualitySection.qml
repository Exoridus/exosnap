import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Video quality & timing")

    ExoSettingRow {
        label: qsTr("Quality")
        info: qsTr("Constant quality holds a visual target instead of a fixed bitrate: the encoder spends whatever bits a scene needs, so a still desktop stays small and a fast game grows.

A tier aims at the same look on every codec. Each one is given a different quantizer to get there, and AV1 reaches it with a much smaller file than H.264 does.

Expert mode replaces this ladder with the rate-control mode and ExoSnap's quality scale.")
        stacked: root.stacked
        // Expert states the same setting twice: the rows below expose the
        // rate-control mode and the CQ value this tier is a name for. Same swap
        // the Frame rate row makes.
        visible: !root.settings.expertMode
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
        label: qsTr("Rate control")
        info: qsTr("Constant quality targets a look and lets the bitrate move with the scene. Variable bitrate aims at an average and allows peaks. Constant bitrate holds one rate at all times, which only matters when the file has to fit a fixed budget.")
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
        label: qsTr("Constant quality (CQ)")
        info: qsTr("ExoSnap's own quality scale, 1 to 51, where lower is better. It is not the number handed to the encoder: each codec is given a quantizer calibrated to that point on the scale, shown under the field. The scale is H.264's quantizer exactly, HEVC runs a little finer, and AV1 uses its own 0 to 255 qindex domain.

Screen content with small text degrades earlier than video does, so the useful range for recording a desktop is narrower than the scale suggests.")
        hint: root.settings.nativeQuantizerHint
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

    // A FACT, not a choice, and the row is shaped so it cannot be mistaken for
    // one: no control, no chevron, no affordance. The encode device is not
    // selectable (product spec 2.1) -- the capture path creates one D3D11 device
    // and NVENC opens on that same device -- but "which GPU is this running on"
    // is the first thing a user checks when quality or performance surprises
    // them, and sending them to Diagnostics for it made a settled fact feel like
    // a diagnostic.
    ExoSettingRow {
        label: qsTr("Encoding on")
        hint: root.settings.encodeAdapterName.length > 0
              ? qsTr("Fixed by the capture device — not a setting")
              : qsTr("Named once the capability probe has run")
        stacked: root.stacked
        controlWidth: 260
        Layout.fillWidth: true

        Label {
            text: root.settings.encodeAdapterName.length > 0 ? root.settings.encodeAdapterName
                                                             : qsTr("Detecting…")
            textFormat: Text.PlainText
            elide: Text.ElideRight
            horizontalAlignment: root.stacked ? Text.AlignLeft : Text.AlignRight
            verticalAlignment: Text.AlignVCenter
            color: root.settings.encodeAdapterName.length > 0 ? ExoTheme.text : ExoTheme.textDim
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontBody
            }
        }
    }

    ExoSettingRow {
        label: qsTr("Capture cursor")
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
