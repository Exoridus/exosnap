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

    // Harness-only: raises one of this bar's dialogs for a --visual-test capture.
    // Both are behind a menu item, so nothing that avoids synthesizing input can
    // otherwise photograph them.
    function openHarnessDialog(name: string): bool {
        if (name === "preset-delete") {
            deleteDialog.open();
            return true;
        }
        if (name === "preset-rename") {
            nameDialog.openFor(true);
            return true;
        }
        if (name === "preset-save-as") {
            nameDialog.openFor(false);
            return true;
        }
        return false;
    }

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

            // Beside the selector it qualifies, not at the far end of the bar:
            // "unsaved changes" 900 px away from the preset it is about is a
            // statement the eye has to go looking for.
            Rectangle {
                visible: root.settings.presetDirty
                implicitWidth: dirtyLabel.implicitWidth + 2 * ExoTheme.spacingSm
                implicitHeight: 22
                color: "transparent"
                border.width: 1
                border.color: ExoTheme.warningText
                radius: ExoTheme.radiusSm
                Layout.alignment: Qt.AlignVCenter

                Label {
                    id: dirtyLabel

                    text: root.settings.presetStatusText
                    textFormat: Text.PlainText
                    anchors.centerIn: parent
                    color: ExoTheme.warningText
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: ExoTheme.fontCaption
                    }
                }
            }

            ExoButton {
                text: qsTr("Save as new…")
                enabled: !root.settings.controlsLocked
                onClicked: nameDialog.openFor(false)
            }

            ExoButton {
                glyph: ExoGlyph.Overflow
                Accessible.name: qsTr("More preset actions")
                onClicked: overflowMenu.popup()
            }
        }

        RowLayout {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            Item {
                Layout.fillWidth: true
            }

            // Its own fixed gap rather than the bar's item spacing: a label and
            // the one switch it names read as a single control. It rides the
            // preset toolbar rather than a page heading because the page has no
            // heading -- the selected navigation tab already names it, and a
            // 22 px "Settings" under a tab reading "Settings" is the same word
            // twice. Diagnostics states the same pairing in its own header.
            Row {
                spacing: ExoTheme.spacingSm
                Layout.alignment: Qt.AlignVCenter

                Label {
                    text: qsTr("Expert mode")
                    textFormat: Text.PlainText
                    color: root.settings.expertMode ? ExoTheme.accent : ExoTheme.textSecondary
                    anchors.verticalCenter: parent.verticalCenter
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: ExoTheme.fontBody
                    }
                }

                ExoSwitch {
                    checked: root.settings.expertMode
                    Accessible.name: qsTr("Expert mode")
                    anchors.verticalCenter: parent.verticalCenter
                    onToggledByUser: value => root.settings.expertMode = value
                }
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
            onTriggered: deleteDialog.open()
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

    // QCR-505. "Delete" sat one click away in the overflow menu and destroyed a
    // custom preset outright — the same distance as "Rename…", which opens a
    // dialog, and directly under "Reset changes", which is recoverable.
    //
    // Confirmation rather than the backlog's preferred Undo, and the reason is
    // the store rather than a preference: `DeleteSelected()` is a registry
    // mutation followed by a persist and a re-apply of whatever preset the
    // selection falls back to. Restoring it would mean a new registry path
    // (re-insert AT its old index, under its old id, without disturbing the
    // selection that moved), a persisted undo window, and a notification action
    // that has to stay valid across a preset switch and an application quit.
    // That is the "new persistence/history architecture" the brief says not to
    // build for one menu item — and it would still leave the user's own
    // recovery window at whatever the toast dwell happens to be.
    //
    // The existing shared dialog is keyboard-operable and defaults to Cancel,
    // so a stray Return cannot delete anything.
    ExoConfirmDialog {
        id: deleteDialog

        title: qsTr("Delete preset")
        bodyText: qsTr("“%1” will be removed. Recording settings switch to the preset that takes its place.")
                  .arg(root.settings.selectedPresetName)
        proceedText: qsTr("Delete preset")
        onAccepted: root.settings.deletePreset()
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
