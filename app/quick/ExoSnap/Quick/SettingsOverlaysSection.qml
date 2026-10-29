pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

// Settings → Overlays.
//
// One place for every surface ExoSnap draws ON TOP OF the recorded screen. It
// was split out of "Notifications & overlays" once the two overlays gained
// content configuration: an in-window toast and a capture-excluded HUD share a
// word and nothing else, and the combined card had grown into two unrelated
// halves.
//
// WHAT THIS CARD CONFIGURES
// -------------------------
// Behaviour and content: whether a surface appears, and which measured values it
// carries. It deliberately exposes NO visual values — opacity, corner radius,
// shadow, colour and placement are decided once in ExoTheme and the overlay QML
// and are not preferences. A user who can set an overlay's opacity can make it
// invisible and then report it as broken.
//
// Every element toggle below names a value with a live runtime producer; the
// list is closed by models::OverlayContentPolicy for exactly that reason.
ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    // Element toggles are a grid of checkboxes under their preset rather than
    // ExoSettingRow controls: they are a sub-choice of the preset above them, and
    // giving each one a full label/hint row would read as six independent
    // settings. They start on the label edge and keep the card's row rhythm, so
    // the grid reads as part of the card and not as a nested panel.
    component ElementToggle: ExoCheckBox {
        Layout.fillWidth: true
    }

    component ElementGrid: GridLayout {
        columns: root.stacked ? 1 : 2
        columnSpacing: ExoTheme.spacingLg
        rowSpacing: ExoTheme.spacingSm
        Layout.fillWidth: true
    }

    title: qsTr("Overlays")

    // ── Recording HUD ────────────────────────────────────────────────────────

    ExoSettingRow {
        label: qsTr("Recording overlay")
        hint: qsTr("Visible while recording, never in the file.")
        stacked: root.stacked
        controlWidth: ExoTheme.controlSlotSwitch
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.showRecordingOverlay
            Accessible.name: qsTr("Recording overlay")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.showRecordingOverlay = value
        }
    }

    ExoSettingRow {
        label: qsTr("Recording overlay content")
        stacked: root.stacked
        visible: root.settings.showRecordingOverlay
        Layout.fillWidth: true

        // A select, not a segmented control: three presets with translated
        // names do not fit the control slot side by side.
        ExoSelect {
            options: root.settings.recordingOverlayPresetOptions
            value: root.settings.recordingOverlayPreset
            Accessible.name: qsTr("Recording overlay content")
            Layout.fillWidth: true
            onValueActivated: value => root.settings.recordingOverlayPreset = value
        }
    }

    ElementGrid {
        visible: root.settings.showRecordingOverlay

        ElementToggle {
            text: qsTr("Elapsed time")
            checked: root.settings.recordingOverlayElapsed
            onToggledByUser: value => root.settings.setRecordingOverlayElement("elapsed", value)
        }

        ElementToggle {
            text: qsTr("File size")
            checked: root.settings.recordingOverlayOutputSize
            onToggledByUser: value => root.settings.setRecordingOverlayElement("size", value)
        }

        ElementToggle {
            text: qsTr("Source name")
            checked: root.settings.recordingOverlaySourceName
            onToggledByUser: value => root.settings.setRecordingOverlayElement("source", value)
        }
    }

    // ── Diagnostics HUD ──────────────────────────────────────────────────────

    ExoSettingRow {
        label: qsTr("Diagnostics overlay")
        stacked: root.stacked
        controlWidth: ExoTheme.controlSlotSwitch
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.showDiagnosticsOverlay
            Accessible.name: qsTr("Diagnostics overlay")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.showDiagnosticsOverlay = value
        }
    }

    ExoSettingRow {
        label: qsTr("Diagnostics overlay content")
        stacked: root.stacked
        visible: root.settings.showDiagnosticsOverlay
        Layout.fillWidth: true

        // A select, not a segmented control: three presets with translated
        // names do not fit the control slot side by side.
        ExoSelect {
            options: root.settings.diagnosticsOverlayPresetOptions
            value: root.settings.diagnosticsOverlayPreset
            Accessible.name: qsTr("Diagnostics overlay content")
            Layout.fillWidth: true
            onValueActivated: value => root.settings.diagnosticsOverlayPreset = value
        }
    }

    ElementGrid {
        visible: root.settings.showDiagnosticsOverlay

        // Labels pair the overlay's own short token with what it means, so the
        // pill stays readable without the card having to invent a second
        // vocabulary for the same four values.
        ElementToggle {
            text: qsTr("health · pipeline status")
            checked: root.settings.diagnosticsOverlayHealth
            onToggledByUser: value => root.settings.setDiagnosticsOverlayElement("health", value)
        }

        ElementToggle {
            text: qsTr("fps · capture rate")
            checked: root.settings.diagnosticsOverlayFps
            onToggledByUser: value => root.settings.setDiagnosticsOverlayElement("fps", value)
        }

        ElementToggle {
            text: qsTr("drop · dropped frames")
            checked: root.settings.diagnosticsOverlayDrop
            onToggledByUser: value => root.settings.setDiagnosticsOverlayElement("drop", value)
        }

        ElementToggle {
            text: qsTr("drift · A/V drift")
            checked: root.settings.diagnosticsOverlayDrift
            onToggledByUser: value => root.settings.setDiagnosticsOverlayElement("drift", value)
        }

        ElementToggle {
            text: qsTr("size · file size")
            checked: root.settings.diagnosticsOverlaySize
            onToggledByUser: value => root.settings.setDiagnosticsOverlayElement("size", value)
        }

        ElementToggle {
            text: qsTr("Muted or degraded audio sources")
            checked: root.settings.diagnosticsOverlayMutedSources
            onToggledByUser: value => root.settings.setDiagnosticsOverlayElement("muted", value)
        }
    }

    // ── Interactive overlay ──────────────────────────────────────────────────

    ExoSettingRow {
        label: qsTr("Quick control pill")
        hint: qsTr("Pause, stop and still frame while recording")
        stacked: root.stacked
        controlWidth: ExoTheme.controlSlotSwitch
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.showQuickControls
            Accessible.name: qsTr("Quick control pill")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.showQuickControls = value
        }
    }
}
