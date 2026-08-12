import QtQuick
import QtQuick.Dialogs

FolderDialog {
    id: root

    required property SettingsAdapter settings

    title: qsTr("Choose output folder")

    onAccepted: root.settings.setOutputFolderFromUrl(root.selectedFolder)
}
