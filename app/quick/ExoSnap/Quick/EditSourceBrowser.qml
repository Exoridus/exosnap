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
    property int currentTab: 0
    objectName: "editSourceBrowser"

    function focusTab(index: int): void {
        root.currentTab = (index + 3) % 3;
        [historyTab, mediaTab, transitionsTab][root.currentTab].forceActiveFocus(Qt.TabFocusReason);
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 4
        Flickable {
            id: tabsScroll
            objectName: "editSourceTabs"
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            contentWidth: tabsRow.width
            contentHeight: height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.HorizontalFlick
            function reveal(tab: Item): void {
                if (!tab) return;
                contentX = Math.max(0, Math.min(Math.max(0, contentWidth - width),
                    tab.x < contentX ? tab.x : Math.max(contentX, tab.x + tab.width - width)));
            }
            onWidthChanged: reveal([historyTab, mediaTab, transitionsTab][root.currentTab])
            WheelHandler {
                onWheel: event => {
                    tabsScroll.contentX = Math.max(0, Math.min(Math.max(0, tabsScroll.contentWidth - tabsScroll.width),
                        tabsScroll.contentX - (event.pixelDelta.x || event.pixelDelta.y || event.angleDelta.x || event.angleDelta.y)));
                    event.accepted = true;
                }
            }
            Row {
                id: tabsRow
                spacing: 0
                Keys.onLeftPressed: root.focusTab(root.currentTab - 1)
                Keys.onRightPressed: root.focusTab(root.currentTab + 1)
                ExoNavTab {
                    id: historyTab
                    objectName: "editHistoryTab"
                    text: qsTr("History")
                    compact: true
                    selected: root.currentTab === 0
                    leftPadding: 4
                    rightPadding: 4
                    implicitHeight: 30
                    onActiveFocusChanged: if (activeFocus) tabsScroll.reveal(this)
                    onSelectedChanged: if (selected) tabsScroll.reveal(this)
                    Accessible.role: Accessible.PageTab
                    onClicked: root.currentTab = 0
                }
                ExoNavTab {
                    id: mediaTab
                    objectName: "editMediaTab"
                    text: qsTr("Media")
                    compact: true
                    selected: root.currentTab === 1
                    leftPadding: 4
                    rightPadding: 4
                    implicitHeight: 30
                    onActiveFocusChanged: if (activeFocus) tabsScroll.reveal(this)
                    onSelectedChanged: if (selected) tabsScroll.reveal(this)
                    Accessible.role: Accessible.PageTab
                    onClicked: root.currentTab = 1
                }
                ExoNavTab {
                    id: transitionsTab
                    objectName: "editTransitionsTab"
                    text: qsTr("Transitions")
                    compact: true
                    selected: root.currentTab === 2
                    leftPadding: 4
                    rightPadding: 4
                    implicitHeight: 30
                    onActiveFocusChanged: if (activeFocus) tabsScroll.reveal(this)
                    onSelectedChanged: if (selected) tabsScroll.reveal(this)
                    Accessible.role: Accessible.PageTab
                    onClicked: root.currentTab = 2
                }
            }
        }
        EditActionButton {
            objectName: "editAddMedia"
            text: qsTr("Import media")
            glyph: ExoGlyph.Plus
            visible: root.currentTab === 1
            onClicked: picker.open()
        }
        Label {
            objectName: "editSourceEmptyTitle"
            text: root.currentTab === 0 ? qsTr("No recordings yet")
                  : root.currentTab === 1 ? qsTr("Drop media here")
                  : qsTr("Transitions will appear here when available.")
            visible: sourceItems.count === 0
            color: ExoTheme.textSecondary
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.topMargin: ExoTheme.spacingSm
            Layout.leftMargin: ExoTheme.spacingSm
            Layout.rightMargin: ExoTheme.spacingSm
        }
        Label {
            objectName: "editSourceEmptyHelp"
            text: root.currentTab === 0 ? qsTr("Recent recordings appear here automatically.")
                                       : qsTr("Or use the Add Media button.")
            visible: sourceItems.count === 0 && root.currentTab !== 2
            color: ExoTheme.textMuted
            font.pixelSize: ExoTheme.fontCaption
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.leftMargin: ExoTheme.spacingSm
            Layout.rightMargin: ExoTheme.spacingSm
        }
        ListView {
            id: sourceItems
            objectName: "editSourceItems"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            reuseItems: true
            model: root.currentTab === 0 ? root.recordings.recentRecordingOptions
                   : root.currentTab === 1 ? root.session.media : []
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
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
                    if (root.currentTab === 0) root.session.addHistory(modelData.path);
                    else root.session.appendAsset(modelData.id);
                }
                Drag.active: drag.active
                Drag.supportedActions: Qt.CopyAction
                Drag.dragType: Drag.Automatic
                Drag.mimeData: root.currentTab === 0 ? { "application/x-exosnap-history": modelData.path }
                                                      : { "application/x-exosnap-asset": String(modelData.id) }
                DragHandler { id: drag; target: null }
                ToolTip.visible: hovered || activeFocus
                ToolTip.text: qsTr("Double-click or press Enter to add")
            }
        }
    }
    FileDialog {
        id: picker
        title: qsTr("Import media")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Video files (*.mkv *.mp4 *.mov *.webm *.avi)"), qsTr("All files (*)")]
        onAccepted: root.session.importMediaBatch(selectedFiles)
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
