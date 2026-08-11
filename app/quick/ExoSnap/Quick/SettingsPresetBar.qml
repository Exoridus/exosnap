import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The preset strip is a TOOLBAR, not a configuration section: it selects which
// set of settings the cards below are showing. Building it as a card with a
// label/control row left a full page width of dead space between the two, so it
// is its own slim band with the controls next to the thing they act on.
Rectangle {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    implicitHeight: bar.implicitHeight + 2 * ExoTheme.cardPaddingCompact
    color: ExoTheme.surface
    border.width: 1
    border.color: ExoTheme.line
    radius: ExoTheme.radiusLg

    GridLayout {
        id: bar

        columns: root.stacked ? 1 : 2
        columnSpacing: ExoTheme.spacingMd
        rowSpacing: ExoTheme.spacingSm
        anchors {
            fill: parent
            margins: ExoTheme.cardPaddingCompact
        }

        RowLayout {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: root.stacked

            Label {
                text: qsTr("Preset")
                textFormat: Text.PlainText
                color: ExoTheme.text
                Layout.rightMargin: ExoTheme.spacingXs
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSectionTitle
                    weight: Font.DemiBold
                }
            }

            ExoSelect {
                options: root.settings.presetOptions
                value: root.settings.selectedPresetId
                enabled: !root.settings.controlsLocked
                Layout.preferredWidth: 200
                Layout.fillWidth: root.stacked
                Accessible.name: qsTr("Active preset")
                onValueActivated: value => root.settings.selectPreset(value)
            }

            ExoButton {
                text: qsTr("Save as new…")
                enabled: !root.settings.controlsLocked
                onClicked: nameDialog.openFor(false)
            }

            ExoButton {
                glyph: ExoGlyph.Overflow
                quiet: true
                Accessible.name: qsTr("More preset actions")
                onClicked: overflowMenu.popup()
            }
        }

        Label {
            text: root.settings.presetStatusText
            textFormat: Text.PlainText
            elide: Text.ElideRight
            horizontalAlignment: root.stacked ? Text.AlignLeft : Text.AlignRight
            color: root.settings.presetDirty ? ExoTheme.warning : ExoTheme.textMuted
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontSecondary
            }
        }
    }

    ExoMenu {
        id: overflowMenu

        ExoMenuItem {
            text: qsTr("Rename…")
            enabled: !root.settings.presetBuiltIn && !root.settings.controlsLocked
            onTriggered: nameDialog.openFor(true)
        }

        ExoMenuItem {
            text: qsTr("Reset changes")
            enabled: root.settings.presetDirty && !root.settings.controlsLocked
            onTriggered: root.settings.resetChanges()
        }

        ExoMenuItem {
            text: qsTr("Delete")
            enabled: !root.settings.presetBuiltIn && !root.settings.controlsLocked
            onTriggered: root.settings.deletePreset()
        }

        MenuSeparator {}

        ExoMenuItem {
            text: qsTr("Export…")
            onTriggered: presetFileDialog.openFor(true)
        }

        ExoMenuItem {
            text: qsTr("Import…")
            enabled: !root.settings.controlsLocked
            onTriggered: presetFileDialog.openFor(false)
        }
    }

    SettingsPresetNameDialog {
        id: nameDialog

        settings: root.settings
    }

    SettingsPresetFileDialog {
        id: presetFileDialog

        settings: root.settings
    }
}
