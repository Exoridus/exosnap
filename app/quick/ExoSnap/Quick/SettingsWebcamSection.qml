import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    // One gate for the whole card. Every control here needs a camera and an idle
    // recorder; spelling the pair out per row is how the chroma-key rows ended up
    // editable while the rest of the card was receded.
    readonly property bool webcamEditable: !root.settings.controlsLocked && root.settings.webcamAvailable

    title: qsTr("Webcam")
    subtitle: qsTr("Position and size are configured in the Record preview.")

    ExoNotice {
        text: qsTr("No webcam was detected. Connect one and press Rescan on the Record page.")
        visible: !root.settings.webcamAvailable
        Layout.fillWidth: true
    }

    ExoSettingRow {
        label: qsTr("Include webcam")
        hint: qsTr("Adds a picture-in-picture overlay to the recording")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.webcamEnabled
            enabled: root.webcamEditable
            Accessible.name: qsTr("Include webcam")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.webcamEnabled = value
        }
    }

    ExoSettingRow {
        label: qsTr("Camera")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.webcamDeviceOptions
            value: root.settings.webcamDeviceId
            enabled: root.webcamEditable
            Layout.fillWidth: true
            Accessible.name: qsTr("Webcam device")
            onValueActivated: value => root.settings.webcamDeviceId = value
        }
    }

    ExoSettingRow {
        label: qsTr("Capture format")
        stacked: root.stacked
        // Resolution and frame rate share this slot.
        controlWidth: 320
        Layout.fillWidth: true

        RowLayout {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            ExoSelect {
                options: root.settings.webcamResolutionOptions
                value: root.settings.webcamResolution
                enabled: root.webcamEditable
                Layout.fillWidth: true
                Accessible.name: qsTr("Webcam resolution")
                onValueActivated: value => root.settings.webcamResolution = value
            }

            ExoSelect {
                options: root.settings.webcamFrameRateOptions
                value: root.settings.webcamFrameRate
                enabled: root.webcamEditable
                Layout.preferredWidth: 100
                Accessible.name: qsTr("Webcam frame rate")
                onValueActivated: value => root.settings.webcamFrameRate = value
            }
        }
    }

    ExoSettingRow {
        label: qsTr("Mirror image")
        hint: qsTr("Applied to the preview and the recording alike")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.webcamMirror
            enabled: root.webcamEditable
            Accessible.name: qsTr("Mirror webcam image")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.webcamMirror = value
        }
    }

    ExoSettingRow {
        label: qsTr("Overlay opacity")
        stacked: root.stacked
        Layout.fillWidth: true

        RowLayout {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            ExoSlider {
                from: 0
                to: 1
                stepSize: 0.05
                value: root.settings.webcamOpacity
                enabled: root.webcamEditable
                Layout.fillWidth: true
                Accessible.name: qsTr("Webcam overlay opacity")
                onMovedByUser: value => root.settings.webcamOpacity = value
            }

            Label {
                text: qsTr("%1 %").arg(Math.round(root.settings.webcamOpacity * 100))
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignRight
                color: root.webcamEditable ? ExoTheme.textSecondary : ExoTheme.textDim
                Layout.preferredWidth: 48
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: ExoTheme.fontSecondary
                }
            }
        }
    }

    ExoSettingRow {
        label: qsTr("Chroma key")
        hint: qsTr("Removes a solid background colour behind you")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.chromaKeyEnabled
            enabled: root.webcamEditable
            Accessible.name: qsTr("Chroma key")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.chromaKeyEnabled = value
        }
    }

    ExoSettingRow {
        label: qsTr("Key colour")
        stacked: root.stacked
        visible: root.settings.chromaKeyEnabled
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.chromaKeyColorOptions
            value: root.settings.chromaKeyColorMode
            enabled: root.webcamEditable
            Layout.fillWidth: true
            Accessible.name: qsTr("Chroma key colour")
            onValueActivated: value => root.settings.chromaKeyColorMode = value
        }
    }

    SettingsChromaSlider {
        label: qsTr("Tolerance")
        value: root.settings.chromaKeyTolerance
        locked: !root.webcamEditable
        stacked: root.stacked
        visible: root.settings.chromaKeyEnabled
        Layout.fillWidth: true
        onValueEdited: value => root.settings.chromaKeyTolerance = value
    }

    SettingsChromaSlider {
        label: qsTr("Softness")
        value: root.settings.chromaKeySoftness
        locked: !root.webcamEditable
        stacked: root.stacked
        visible: root.settings.chromaKeyEnabled
        Layout.fillWidth: true
        onValueEdited: value => root.settings.chromaKeySoftness = value
    }

    SettingsChromaSlider {
        label: qsTr("Spill reduction")
        value: root.settings.chromaKeySpill
        locked: !root.webcamEditable
        stacked: root.stacked
        visible: root.settings.chromaKeyEnabled
        Layout.fillWidth: true
        onValueEdited: value => root.settings.chromaKeySpill = value
    }
}
