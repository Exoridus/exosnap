import QtQuick
import QtQuick.Dialogs

// Native export/import picker. The chosen URL goes straight to the adapter,
// which converts it to a filesystem path -- QML never assembles one.
FileDialog {
    id: root

    required property SettingsAdapter settings
    property bool saving: true

    function openFor(save: bool): void {
        root.saving = save;
        root.open();
    }

    title: root.saving ? qsTr("Export preset") : qsTr("Import presets")
    nameFilters: [qsTr("Preset files (*.toml)")]
    fileMode: root.saving ? FileDialog.SaveFile : FileDialog.OpenFile
    defaultSuffix: "toml"

    onAccepted: {
        if (root.saving) {
            root.settings.exportPresetToUrl(root.selectedFile);
        } else {
            root.settings.importPresetsFromUrl(root.selectedFile);
        }
    }
}
