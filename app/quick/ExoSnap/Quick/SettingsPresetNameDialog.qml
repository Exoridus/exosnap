import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Popup {
    id: root

    required property SettingsAdapter settings
    property bool renaming: false

    readonly property bool nameRejected: root.settings.presetNameRejected(
                                             nameField.text, root.renaming ? root.settings.selectedPresetId : "")

    function openFor(rename: bool): void {
        root.renaming = rename;
        nameField.text = rename ? root.settings.selectedPresetName : "";
        root.open();
    }

    function submit(): void {
        if (root.nameRejected)
            return;
        const name = nameField.text.trim();
        if (root.renaming)
            root.settings.renamePreset(name);
        else
            root.settings.savePresetAs(name);
        root.close();
    }

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: 380
    padding: ExoTheme.spacingLg
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    onOpened: nameField.forceActiveFocus()

    background: Rectangle {
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.lineStrong
        radius: ExoTheme.radiusLg
    }

    contentItem: ColumnLayout {
        spacing: ExoTheme.spacingMd

        Label {
            text: root.renaming ? qsTr("Rename preset") : qsTr("Save preset as")
            color: ExoTheme.text
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontTitle
                weight: Font.DemiBold
            }
        }

        ExoTextField {
            id: nameField

            placeholderText: qsTr("Preset name")
            Layout.fillWidth: true
            Accessible.name: qsTr("Preset name")
            Keys.onReturnPressed: root.submit()
        }

        Label {
            text: qsTr("Enter a name that is not already in use.")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            visible: root.nameRejected
            color: ExoTheme.warningText
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
            }
        }

        RowLayout {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            ExoButton {
                text: qsTr("Cancel")
                onClicked: root.close()
            }

            Item { Layout.fillWidth: true }

            ExoButton {
                text: root.renaming ? qsTr("Rename preset") : qsTr("Save preset")
                tone: "primary"
                enabled: !root.nameRejected
                onClicked: root.submit()
            }
        }
    }
}
