import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Name entry for "Save as new…" and "Rename". The accept button stays disabled
// while the adapter's shared uniqueness rule rejects the typed name, so the
// dialog can never submit a name the registry would refuse.
Dialog {
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

    title: root.renaming ? qsTr("Rename preset") : qsTr("Save preset as")
    modal: true
    anchors.centerIn: root.parent
    standardButtons: Dialog.Ok | Dialog.Cancel

    Component.onCompleted: root.standardButton(Dialog.Ok).enabled = Qt.binding(() => !root.nameRejected)

    onAccepted: {
        if (root.renaming) {
            root.settings.renamePreset(nameField.text);
        } else {
            root.settings.savePresetAs(nameField.text);
        }
    }

    ColumnLayout {
        spacing: ExoTheme.spacingSm

        ExoTextField {
            id: nameField

            placeholderText: qsTr("Preset name")
            Layout.preferredWidth: 280
            Accessible.name: qsTr("Preset name")
        }

        Label {
            text: qsTr("Enter a name that is not already in use.")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            visible: root.nameRejected
            color: ExoTheme.warning
            Layout.preferredWidth: 280
            font {
                family: ExoTheme.sansFamily
                pixelSize: 11
            }
        }
    }
}
