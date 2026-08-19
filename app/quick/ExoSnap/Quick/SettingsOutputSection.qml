import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Output")
    subtitle: root.settings.savesToText

    ExoSettingRow {
        label: qsTr("Destination folder")
        hint: qsTr("Where recordings are saved")
        warning: root.settings.folderValidation
        stacked: root.stacked
        controlWidth: 320
        Layout.fillWidth: true

        RowLayout {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            ExoTextField {
                value: root.settings.outputFolder
                enabled: !root.settings.controlsLocked
                Layout.fillWidth: true
                Accessible.name: qsTr("Destination folder")
                onCommitted: value => root.settings.outputFolder = value
            }

            ExoButton {
                text: qsTr("Browse…")
                enabled: !root.settings.controlsLocked
                onClicked: folderDialog.open()
            }
        }
    }

    SettingsOutputFolderDialog {
        id: folderDialog

        settings: root.settings
    }

    ExoSettingRow {
        label: qsTr("Filename pattern")
        hint: qsTr("Example: %1").arg(root.settings.exampleFilename)
        warning: root.settings.patternValidation
        stacked: root.stacked
        Layout.fillWidth: true

        ExoTextField {
            value: root.settings.namingPattern
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Filename pattern")
            onCommitted: value => root.settings.namingPattern = value
        }
    }

    Flow {
        spacing: ExoTheme.spacingXs
        Layout.fillWidth: true

        Repeater {
            model: root.settings.filenameTokens

            Rectangle {
                id: tokenChip

                required property string modelData

                implicitWidth: tokenLabel.implicitWidth + 2 * ExoTheme.spacingSm
                implicitHeight: 22
                color: ExoTheme.surfaceRaised
                border.width: 1
                border.color: ExoTheme.line
                radius: ExoTheme.radiusSm

                Label {
                    id: tokenLabel

                    text: tokenChip.modelData
                    textFormat: Text.PlainText
                    anchors.centerIn: parent
                    color: ExoTheme.textSecondary
                    font {
                        family: ExoTheme.monoFamily
                        pixelSize: ExoTheme.fontCaption
                    }
                }
            }
        }
    }

    ExoSettingRow {
        label: qsTr("Output resolution")
        hint: qsTr("Downscale to save size · re-encodes")
        warning: root.settings.customResolutionValidation
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
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
        controlWidth: 60
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
        controlWidth: 60
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
            suffix: qsTr("MB")
            value: root.settings.splitCustomSizeMb
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Segment size")
            onValueCommitted: value => root.settings.splitCustomSizeMb = value
        }
    }
}
