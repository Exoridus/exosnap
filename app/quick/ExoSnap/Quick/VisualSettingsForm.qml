pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

ColumnLayout {
    id: root

    required property SettingsAdapter settings
    required property string sourceId
    property bool stacked: false
    property bool recordScope: false
    property var rows: []
    property var sourceState: ({})

    function refresh(): void {
        root.rows = root.settings ? root.settings.visualSettingsRows(root.sourceId, root.recordScope) : [];
        root.sourceState = root.settings ? root.settings.visualSourceState(root.sourceId, root.recordScope) : ({});
    }

    onSettingsChanged: root.refresh()
    onSourceIdChanged: root.refresh()
    onRecordScopeChanged: root.refresh()
    Component.onCompleted: root.refresh()

    spacing: ExoTheme.spacingSm

    Connections {
        target: root.settings
        function onConfigChanged(): void { root.refresh(); }
        function onOptionsChanged(): void { root.refresh(); }
        function onControlsLockedChanged(): void { root.refresh(); }
    }

    Repeater {
        model: root.rows

        delegate: Loader {
            id: row
            required property var modelData

            Layout.fillWidth: true
            sourceComponent: row.modelData.kind === "action" ? actionRow : selectRow

            Component {
                id: selectRow
                ExoSettingRow {
                    label: row.modelData.label
                    stacked: root.stacked
                    ExoSelect {
                        options: row.modelData.choices
                        value: row.modelData.value
                        enabled: row.modelData.enabled
                        Accessible.name: row.modelData.label
                        Layout.fillWidth: true
                        onValueActivated: value => root.settings.selectVisualSetting(
                            row.modelData.sourceId, row.modelData.itemId, row.modelData.editRevision,
                            row.modelData.action, value, root.recordScope)
                    }
                }
            }

            Component {
                id: actionRow
                ExoButton {
                    text: row.modelData.label
                    enabled: row.modelData.enabled
                    onClicked: root.settings.dispatchVisualAction(
                        row.modelData.sourceId, row.modelData.itemId, row.modelData.runtime,
                        row.modelData.action, root.recordScope)
                }
            }
        }
    }
}
