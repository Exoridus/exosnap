import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
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

    // The filled state: the camera as it will be recorded, mirror, opacity and
    // chroma key included. Only an included camera is opened; an excluded one
    // shows why the box is empty instead of quietly running the device.
    Rectangle {
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.line
        radius: ExoTheme.radiusMd
        clip: true
        visible: root.settings.webcamAvailable
        Layout.fillWidth: true
        Layout.preferredHeight: Math.round(width * 9 / 16)
        Layout.maximumHeight: 220

        ExoWebcamPreviewItem {
            id: cameraPreview

            feed: root.settings.webcamPreviewFeed as WebcamPreviewFeed
            visible: root.settings.webcamEnabled
            anchors.fill: parent
            anchors.margins: 1
        }

        Label {
            text: root.settings.webcamEnabled ? qsTr("Starting camera…") : qsTr("Camera off")
            textFormat: Text.PlainText
            horizontalAlignment: Text.AlignHCenter
            color: ExoTheme.textSecondary
            visible: !root.settings.webcamEnabled || !cameraPreview.hasFrame
            anchors.centerIn: parent
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontSecondary
            }
        }
    }

    ExoSettingRow {
        label: qsTr("Include webcam")
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
                Layout.preferredWidth: 84
                Accessible.name: qsTr("Webcam frame rate")
                onValueActivated: value => root.settings.webcamFrameRate = value
            }
        }
    }

    ExoSettingRow {
        label: qsTr("Mirror image")
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
        hint: qsTr("Removes a solid-colour backdrop")
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

    // One key colour, typed as hex or picked, shown whether or not the key is
    // on so it can be set up first. The '#' is the field's own sigil, so only
    // the digits are typed, and one pasted along with them is dropped at once.
    ExoSettingRow {
        label: qsTr("Key colour")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoTextField {
            value: root.settings.chromaKeyHex
            leadingGlyph: ExoGlyph.Hash
            errorText: root.settings.chromaKeyHexError
            enabled: root.webcamEditable
            trailingGlyph: ExoGlyph.Swatch
            trailingGlyphColor: root.settings.chromaKeyColor
            trailingAccessibleName: qsTr("Pick colour")
            trailingActive: keyColorDialog.visible
            Layout.fillWidth: true
            Accessible.name: qsTr("Chroma key colour")
            font.family: ExoTheme.monoFamily
            onTextEdited: {
                if (text.startsWith("#"))
                    text = text.slice(1);
            }
            onCommitted: value => root.settings.chromaKeyHex = value
            onTrailingClicked: keyColorDialog.open()
        }

        ColorDialog {
            id: keyColorDialog

            title: qsTr("Chroma key colour")
            selectedColor: root.settings.chromaKeyColor
            onAccepted: root.settings.chromaKeyColor = selectedColor
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

    // The camera's own image controls (UVC), only those this camera reports.
    // Last in the card so opening it pushes nothing else down, and laid out as
    // ordinary rows of this card rather than a nested panel. A control nobody
    // touched keeps the camera's value; the reset hands every one back.
    ExoSettingRow {
        label: qsTr("Camera image")
        hint: root.settings.webcamControlsAdjusted ? qsTr("Adjusted")
                                                   : qsTr("Camera defaults")
        stacked: root.stacked
        visible: root.settings.webcamAvailable && root.settings.webcamControls.length > 0
        controlWidth: 100
        Layout.fillWidth: true

        SettingsGroupToggle {
            id: cameraImageToggle

            accessibleName: qsTr("Camera image controls")
        }
    }

    // Built when first opened: thirteen control rows per camera are page-load
    // work for a group that is collapsed nearly all the time.
    Loader {
        active: cameraImageToggle.open && root.settings.webcamAvailable && root.settings.webcamControls.length > 0
        visible: active
        Layout.fillWidth: true

        sourceComponent: ColumnLayout {
            spacing: ExoTheme.rowSpacing

            Repeater {
                model: root.settings.webcamControls

                delegate: ExoSettingRow {
                    id: controlRow

                    required property var modelData
                    readonly property bool isChoice: controlRow.modelData.options !== undefined
                    readonly property var controlState: root.settings.webcamControlStates[String(controlRow.modelData.control)]
                                                        ?? ({ value: 0, automatic: false })
                    readonly property bool manual: !controlRow.controlState.automatic && controlRow.modelData.canManual

                    label: controlRow.modelData.label
                    stacked: root.stacked
                    controlWidth: ExoTheme.controlSlot + 40
                    Layout.fillWidth: true

                    ExoSelect {
                        visible: controlRow.isChoice
                        options: controlRow.isChoice ? controlRow.modelData.options : []
                        value: controlRow.controlState.value
                        enabled: root.webcamEditable
                        // The shared select width, not this row's wider slider slot.
                        Layout.preferredWidth: ExoTheme.controlSlot
                        Layout.alignment: Qt.AlignRight
                        Accessible.name: controlRow.modelData.label
                        onValueActivated: value => root.settings.setWebcamControlValue(controlRow.modelData.control, value)
                    }

                    // Slider, value and the Auto slot share one column layout in every
                    // row, with the slot held open where a control has no Auto, so the
                    // sliders line up down the group.
                    RowLayout {
                        visible: !controlRow.isChoice
                        spacing: ExoTheme.spacingSm
                        Layout.fillWidth: true

                        ExoSlider {
                            from: controlRow.modelData.minimum
                            to: controlRow.modelData.maximum
                            stepSize: controlRow.modelData.step
                            value: controlRow.controlState.value
                            enabled: root.webcamEditable && controlRow.manual
                            Layout.fillWidth: true
                            Accessible.name: controlRow.modelData.label
                            onMovedByUser: value => root.settings.setWebcamControlValue(controlRow.modelData.control,
                                                                                        Math.round(value))
                        }

                        Label {
                            // An automatic control's last manual value says nothing
                            // about what the camera is doing now.
                            text: controlRow.controlState.automatic ? "\u2013" : controlRow.controlState.value
                            textFormat: Text.PlainText
                            horizontalAlignment: Text.AlignRight
                            color: root.webcamEditable && controlRow.manual ? ExoTheme.textSecondary : ExoTheme.textDim
                            Layout.preferredWidth: 40
                            font {
                                family: ExoTheme.monoFamily
                                pixelSize: ExoTheme.fontSecondary
                            }
                        }

                        Item {
                            implicitHeight: autoBox.implicitHeight
                            Layout.preferredWidth: 56

                            ExoCheckBox {
                                id: autoBox

                                text: qsTr("Auto")
                                visible: controlRow.modelData.canAutomatic
                                checked: controlRow.controlState.automatic
                                enabled: root.webcamEditable
                                anchors.verticalCenter: parent.verticalCenter
                                Accessible.name: qsTr("Automatic %1").arg(controlRow.modelData.label)
                                onToggledByUser: value => root.settings.setWebcamControlAutomatic(
                                                     controlRow.modelData.control, value)
                            }
                        }
                    }
                }
            }

            ExoButton {
                text: qsTr("Reset to camera defaults")
                compact: true
                quiet: true
                enabled: root.webcamEditable && root.settings.webcamControlsAdjusted
                Layout.alignment: Qt.AlignRight
                onClicked: root.settings.resetWebcamControls()
            }
        }
    }
}
