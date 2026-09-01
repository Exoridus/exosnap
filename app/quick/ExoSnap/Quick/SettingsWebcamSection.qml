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
    subtitle: qsTr("Position and size are set in the Record preview.")

    // The empty state has the shape of the filled one. A notice where the picture
    // will be means the card jumps into a different geometry the moment a camera
    // is plugged in; a box that is already the right size just fills up.
    //
    // Not a caution tone either: no camera connected is a fact about the machine,
    // not a problem this product measured, and the palette reserves amber for
    // things that are actually wrong.
    Rectangle {
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.line
        radius: ExoTheme.radiusMd
        visible: !root.settings.webcamAvailable
        Layout.fillWidth: true
        Layout.preferredHeight: Math.round(width * 9 / 16)
        Layout.maximumHeight: 220

        ColumnLayout {
            anchors.centerIn: parent
            spacing: ExoTheme.spacingXs

            Label {
                text: qsTr("No camera found")
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignHCenter
                color: ExoTheme.textSecondary
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                }
            }

            // No Rescan button. Capture devices are discovered as they arrive, so
            // a button here would offer work the product has already done.
            Label {
                text: qsTr("Connect a camera and it appears here")
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignHCenter
                color: ExoTheme.textMuted
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontCaption
                }
            }
        }
    }

    ExoSettingRow {
        label: qsTr("Include webcam")
        hint: qsTr("Adds a picture-in-picture overlay to the recording")
        stacked: root.stacked
        controlWidth: ExoTheme.controlSlotSwitch
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
        controlWidth: ExoTheme.controlSlotWide
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
        controlWidth: ExoTheme.controlSlotSwitch
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
        controlWidth: ExoTheme.controlSlotSwitch
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
