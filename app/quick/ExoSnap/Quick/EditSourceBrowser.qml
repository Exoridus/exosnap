pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Item {
    id: root
    required property EditSessionAdapter session
    required property RecordViewModelAdapter recordings
    required property EditTimelineAdapter timeline
    ColumnLayout {
        anchors.fill: parent
        spacing: ExoTheme.spacingSm
    TabBar {
        id: tabs
        Layout.fillWidth: true
        TabButton { text: qsTr("History") }
        TabButton { text: qsTr("Media") }
        TabButton { text: qsTr("Transitions") }
    }
    ExoButton {
        text: qsTr("Import media")
        visible: tabs.currentIndex === 1
        compact: true
        onClicked: picker.open()
    }
    Label {
        text: qsTr("Transitions are not available yet.")
        visible: tabs.currentIndex === 2
        color: ExoTheme.textSecondary
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    ListView {
        id: sourceItems
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        reuseItems: true
        model: tabs.currentIndex === 0 ? root.recordings.recentRecordingOptions
               : tabs.currentIndex === 1 ? root.session.media : []
        ScrollBar.vertical: ScrollBar {}
        delegate: ItemDelegate {
            id: sourceItem
            required property var modelData
            width: sourceItems.width
            height: 66
            leftPadding: thumbnail.visible ? 80 : 12
            text: (modelData.name || modelData.label) + "\n"
                  + (modelData.available ? (modelData.metadata || "") + " " + root.session.formatClock(modelData.durationMs || 0) : qsTr("Media unavailable"))
            Accessible.name: text
            Image {
                id: thumbnail
                x: 4
                y: 8
                width: 68
                height: 48
                source: sourceItem.modelData.path === root.timeline.sourcePath ? root.timeline.posterSource : ""
                visible: status === Image.Ready
                fillMode: Image.PreserveAspectFit
                Accessible.ignored: true
            }
            onDoubleClicked: add()
            Keys.onReturnPressed: add()
            function add(): void {
                if (tabs.currentIndex === 0) root.session.addHistory(modelData.path);
                else root.session.appendAsset(modelData.id);
            }
            Drag.active: drag.active
            Drag.supportedActions: Qt.CopyAction
            Drag.dragType: Drag.Automatic
            Drag.mimeData: tabs.currentIndex === 0 ? { "application/x-exosnap-history": modelData.path }
                                                  : { "application/x-exosnap-asset": String(modelData.id) }
            DragHandler { id: drag; target: null }
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Double-click or press Enter to add")
        }
    }
    }
    FileDialog {
        id: picker
        title: qsTr("Import media")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Video files (*.mkv *.mp4 *.mov *.webm *.avi)"), qsTr("All files (*)")]
        onAccepted: { root.session.importMediaBatch(selectedFiles); }
    }
    DropArea {
        anchors.fill: parent
        onDropped: drop => {
            if (drop.hasUrls) {
                root.session.importMediaBatch(drop.urls);
                drop.acceptProposedAction();
            }
        }
    }
}
