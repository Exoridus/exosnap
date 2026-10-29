pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property SettingsAdapter settings
    required property bool stacked
    property bool cacheCleared: false
    ExoSettingRow {
        label: qsTr("Editor previews")
        hint: root.cacheCleared ? qsTr("Preview cache cleared. Visible previews regenerate as needed.") : qsTr("Cached thumbnails and waveforms. Source recordings are kept.")
        stacked: root.stacked
        Layout.fillWidth: true
        ExoButton {
            objectName: "settingsClearMediaCaches"
            text: qsTr("Clear preview cache")
            Accessible.name: text
            Layout.alignment: Qt.AlignRight
            onClicked: root.settings.clearMediaCaches()
        }
    }
    Connections {
        target: root.settings
        function onMediaCachesCleared(): void {
            root.cacheCleared = true;
        }
    }
}
