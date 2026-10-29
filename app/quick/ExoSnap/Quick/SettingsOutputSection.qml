import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ExoCard {
    id: root

    function focusControl(target: int): var { return target === SettingsAdapter.OutputDestination ? destinationField : resolutionControl; }

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Output")
    subtitle: root.settings.outputSummary

    function focusDestination(): void {
        destinationField.forceActiveFocus(Qt.OtherFocusReason);
    }

    ExoSettingRow {
        label: qsTr("Destination folder")
        controlWidth: ExoTheme.controlSlotWide
        stacked: root.stacked
        Layout.fillWidth: true

        // Typed or picked: the adapter validates either way and states the
        // problem in the row's warning line.
        ExoTextField {
            id: destinationField
            implicitWidth: ExoTheme.controlSlotWide

            value: root.settings.outputFolder
            enabled: !root.settings.controlsLocked
            errorText: root.settings.folderValidation
            trailingGlyph: ExoGlyph.Folder
            trailingAccessibleName: qsTr("Choose folder")
            trailingActive: folderDialog.visible
            Layout.fillWidth: true
            Accessible.name: qsTr("Destination folder")
            onCommitted: value => root.settings.outputFolder = value
            onTrailingClicked: folderDialog.open()
            font.family: ExoTheme.monoFamily
        }
    }

    SettingsOutputFolderDialog {
        id: folderDialog

        settings: root.settings
    }

    ExoSettingRow {
        label: qsTr("Filename pattern")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoTextField {
            id: patternField
            implicitWidth: ExoTheme.controlSlot

            value: root.settings.namingPattern
            enabled: !root.settings.controlsLocked
            errorText: root.settings.patternValidation
            trailingGlyph: ExoGlyph.Braces
            trailingAccessibleName: qsTr("Insert placeholder")
            trailingActive: placeholderMenu.visible
            onTrailingClicked: placeholderMenu.visible ? placeholderMenu.close() : placeholderMenu.open()
            Layout.fillWidth: true
            Accessible.name: qsTr("Filename pattern")
            onCommitted: value => root.settings.namingPattern = value

            // Inserted where the caret stood and committed at once, so a click
            // in the list behaves like typing the placeholder there.
            function insertPlaceholder(token: string): void {
                const at = Math.min(patternField.cursorPosition, patternField.text.length);
                patternField.text = patternField.text.slice(0, at) + token + patternField.text.slice(at);
                patternField.cursorPosition = at + token.length;
                patternField.committed(patternField.text);
            }

            Popup {
                id: placeholderMenu

                // Right-aligned under the field, and wider than it: two columns
                // of placeholder, meaning and current value need the room.
                width: Math.max(patternField.width, 380)
                x: patternField.width - width
                y: patternField.height + ExoTheme.spacingXs
                padding: ExoTheme.spacingSm
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

                background: Rectangle {
                    color: ExoTheme.surfaceRaised
                    border.width: 1
                    border.color: ExoTheme.line
                    radius: ExoTheme.radiusMd
                }

                contentItem: GridLayout {
                    columns: 2
                    columnSpacing: ExoTheme.spacingXs
                    rowSpacing: ExoTheme.spacingXs

                    Repeater {
                        model: root.settings.filenameTokens

                        delegate: AbstractButton {
                            id: placeholderEntry

                            required property var modelData

                            hoverEnabled: true
                            focusPolicy: Qt.StrongFocus
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Insert %1, %2").arg(placeholderEntry.modelData.token)
                                                                    .arg(placeholderEntry.modelData.label)
                            onClicked: {
                                patternField.insertPlaceholder(placeholderEntry.modelData.token);
                                placeholderMenu.close();
                            }

                            HoverHandler {
                                cursorShape: Qt.PointingHandCursor
                            }

                            background: Rectangle {
                                radius: ExoTheme.radiusSm
                                color: placeholderEntry.hovered ? ExoTheme.surfaceHover : "transparent"
                                border.width: placeholderEntry.visualFocus ? ExoTheme.focusRingWidth : 0
                                border.color: ExoTheme.text
                            }

                            contentItem: ColumnLayout {
                                spacing: 2

                                RowLayout {
                                    spacing: ExoTheme.spacingSm
                                    Layout.fillWidth: true

                                    Label {
                                        text: placeholderEntry.modelData.token
                                        textFormat: Text.PlainText
                                        color: ExoTheme.text
                                        font {
                                            family: ExoTheme.monoFamily
                                            pixelSize: ExoTheme.fontSecondary
                                        }
                                    }

                                    Label {
                                        text: placeholderEntry.modelData.label
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        horizontalAlignment: Text.AlignRight
                                        color: ExoTheme.textMuted
                                        Layout.fillWidth: true
                                        font {
                                            family: ExoTheme.sansFamily
                                            pixelSize: ExoTheme.fontCaption
                                        }
                                    }
                                }

                                Label {
                                    text: placeholderEntry.modelData.preview
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    color: ExoTheme.textSecondary
                                    Layout.fillWidth: true
                                    font {
                                        family: ExoTheme.monoFamily
                                        pixelSize: ExoTheme.fontCaption
                                    }
                                }
                            }

                            topPadding: ExoTheme.spacingXs
                            bottomPadding: ExoTheme.spacingXs
                            leftPadding: ExoTheme.spacingSm
                            rightPadding: ExoTheme.spacingSm
                        }
                    }
                }
            }
        }
    }

    // What the pattern above actually produces, folder included. The Widgets
    // settings page answered this and the first Quick version did not: an example
    // file name alone never says where the file lands.
    ExoSettingRow {
        label: qsTr("Saves as")
        // The resolved filename, which is a whole path and needs the room.
        controlWidth: ExoTheme.controlSlotWide
        stacked: root.stacked
        Layout.fillWidth: true

        Label {
            id: savesToLabel
            Layout.preferredWidth: ExoTheme.controlSlotWide
            Layout.minimumWidth: 0

            text: root.settings.savesToText
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            horizontalAlignment: root.stacked ? Text.AlignLeft : Text.AlignRight
            color: ExoTheme.textSecondary
            Layout.fillWidth: true
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontCaption
            }

            // The label's own `truncated`, not `parent`'s: the parent is the
            // setting row, which has no such property, so the tooltip never
            // opened.
            ToolTip.visible: pathHover.hovered && savesToLabel.truncated
            ToolTip.delay: 400
            ToolTip.text: root.settings.savesToText

            HoverHandler {
                id: pathHover
            }
        }
    }

    ExoSettingRow {
        label: qsTr("Output resolution")
        hint: qsTr("Downscaling re-encodes")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
            id: resolutionControl
            options: root.settings.resolutionOptions
            value: root.settings.resolutionMode
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Output resolution")
            onValueActivated: value => root.settings.resolutionMode = value
        }

        RowLayout {
            spacing: ExoTheme.spacingSm
            visible: root.settings.customResolutionActive
            Layout.fillWidth: true

            ExoNumberField {
                from: 0
                to: 15360
                stepSize: 2
                value: root.settings.customWidth
                errorText: root.settings.customResolutionValidation
                enabled: !root.settings.controlsLocked
                Layout.fillWidth: true
                Accessible.name: qsTr("Custom width")
                onValueCommitted: value => root.settings.customWidth = value
            }

            ExoNumberField {
                from: 0
                to: 8640
                stepSize: 2
                value: root.settings.customHeight
                errorText: root.settings.customResolutionValidation
                enabled: !root.settings.controlsLocked
                Layout.fillWidth: true
                Accessible.name: qsTr("Custom height")
                onValueCommitted: value => root.settings.customHeight = value
            }
        }
    }

    ExoSettingRow {
        label: qsTr("Split recording")
        hint: root.settings.splitSummary
        stacked: root.stacked
        controlWidth: ExoTheme.controlSlotSwitch
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.splitByTimeEnabled
            enabled: !root.settings.controlsLocked
            Accessible.name: qsTr("Split recording by time")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.splitByTimeEnabled = value
        }
    }

    ExoSettingRow {
        label: qsTr("Split interval")
        stacked: root.stacked
        visible: root.settings.splitByTimeEnabled
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.splitModeOptions
            value: root.settings.splitMode
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Split interval")
            onValueActivated: value => root.settings.splitMode = value
        }

        ExoNumberField {
            from: 1
            to: 1440
            suffix: qsTr("min")
            value: root.settings.splitCustomMinutes
            visible: root.settings.splitCustomIntervalActive
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Custom split interval")
            onValueCommitted: value => root.settings.splitCustomMinutes = value
        }
    }

    ExoSettingRow {
        label: qsTr("Split by size")
        stacked: root.stacked
        controlWidth: ExoTheme.controlSlotSwitch
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.splitBySizeEnabled
            enabled: !root.settings.controlsLocked
            Accessible.name: qsTr("Split recording by size")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.splitBySizeEnabled = value
        }
    }

    ExoSettingRow {
        label: qsTr("Segment size")
        stacked: root.stacked
        visible: root.settings.splitBySizeEnabled
        Layout.fillWidth: true

        ExoNumberField {
            from: 50
            to: 1048576
            stepSize: 50
            suffix: qsTr("MiB")
            value: root.settings.splitCustomSizeMb
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Segment size")
            onValueCommitted: value => root.settings.splitCustomSizeMb = value
        }
    }
}
