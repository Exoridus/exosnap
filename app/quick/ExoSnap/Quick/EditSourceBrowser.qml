pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import QtQuick.Dialogs

Item {
    id: root
    required property EditSessionAdapter session
    required property RecordViewModelAdapter recordings
    required property EditTimelineAdapter timeline
    property int currentTab: 0
    property bool pinned: false
    property bool pinPreferred: pinned
    property bool canPin: true
    property var selectedFolder: ({})
    property var replacementAsset: 0
    signal pinToggled()
    signal closeRequested(bool keyboardActivation)
    objectName: "editSourceBrowser"

    function focusTab(index: int): void {
        root.currentTab = (index + 3) % 3;
        [mediaTab, foldersTab, historyTab][root.currentTab].forceActiveFocus(Qt.TabFocusReason);
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 4
        RowLayout {
            Layout.fillWidth: true
            spacing: 2
            Flickable {
                id: tabsScroll
                objectName: "editSourceTabs"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredHeight: 30
                contentWidth: tabsRow.width
                contentHeight: height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.HorizontalFlick
                function reveal(tab: Item): void {
                    if (!tab)
                        return;
                    const target = tab.width >= width ? tab.x : tab.x < contentX ? tab.x : Math.max(contentX, tab.x + tab.width - width);
                    contentX = Math.max(0, Math.min(Math.max(0, contentWidth - width), target));
                }
                onWidthChanged: reveal([mediaTab, foldersTab, historyTab][root.currentTab])
                WheelHandler {
                    onWheel: event => {
                        tabsScroll.contentX = Math.max(0, Math.min(Math.max(0, tabsScroll.contentWidth - tabsScroll.width), tabsScroll.contentX - (event.pixelDelta.x || event.pixelDelta.y || event.angleDelta.x || event.angleDelta.y)));
                        event.accepted = true;
                    }
                }
                Row {
                    id: tabsRow
                    Keys.onLeftPressed: root.focusTab(root.currentTab - 1)
                    Keys.onRightPressed: root.focusTab(root.currentTab + 1)
                    ExoNavTab {
                        id: mediaTab
                        objectName: "editMediaTab"
                        text: qsTr("Project")
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
                        id: foldersTab
                        objectName: "editFoldersTab"
                        text: qsTr("Folders")
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
                        id: historyTab
                        objectName: "editHistoryTab"
                        text: qsTr("Recordings")
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
                id: add
                objectName: "editAddMedia"
                text: qsTr("Add media")
                glyph: ExoGlyph.Plus
                Layout.preferredWidth: 24
                onClicked: addMenu.open()
                ExoMenu {
                    id: addMenu
                    objectName: "editSourceAddMenu"
                    y: add.height
                    ExoMenuItem {
                        objectName: "editSourceAddFiles"
                        text: qsTr("Add files")
                        onTriggered: picker.open()
                    }
                    ExoMenuItem {
                        objectName: "editSourceAddFolder"
                        text: qsTr("Add folder")
                        onTriggered: folderPicker.open()
                    }
                }
            }
            EditActionButton {
                objectName: "editMediaPin"
                ToolTip.text: root.pinPreferred && !root.pinned ? qsTr("Pinned when space is available") : text
                Layout.preferredWidth: 24
                text: root.pinPreferred ? qsTr("Unpin") : qsTr("Pin")
                glyph: ExoGlyph.Pin
                enabled: root.pinPreferred || root.canPin
                checkable: true
                checked: root.pinPreferred
                onClicked: {
                    root.pinToggled();
                    checked = Qt.binding(() => root.pinPreferred);
                }
            }
            EditActionButton {
                id: closeAction
                objectName: "editMediaClose"
                Layout.preferredWidth: 24
                property bool keyboardActivation: false
                text: qsTr("Close Media")
                glyph: ExoGlyph.Close
                onClicked: root.closeRequested(closeAction.keyboardActivation)
                Keys.onPressed: event => {
                    if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                        closeAction.keyboardActivation = true;
                }
                PointHandler {
                    acceptedButtons: Qt.LeftButton
                    onActiveChanged: if (active) closeAction.keyboardActivation = false
                }
            }
        }
        ExoButton {
            objectName: "editPlaceImportedMedia"
            Layout.fillWidth: true
            visible: root.session.hasPendingImportedMedia
            text: qsTr("Place imported media")
            onClicked: root.session.placeImportedMedia()
        }
        ExoButton {
            objectName: "editCancelImport"
            Layout.fillWidth: true
            visible: root.session.importPending
            text: qsTr("Cancel import")
            onClicked: root.session.cancelPendingImports()
        }
        RowLayout {
            Layout.fillWidth: true
            visible: root.currentTab === 1 && !!root.selectedFolder.entryId
            ExoButton {
                objectName: "editFolderRefresh"
                Layout.fillWidth: true
                text: qsTr("Refresh")
                enabled: !!root.selectedFolder.directory
                onClicked: root.session.folderModel.refresh(root.selectedFolder.entryId)
            }
            ExoButton {
                objectName: "editFolderRemove"
                Layout.fillWidth: true
                text: qsTr("Remove root")
                visible: !!root.selectedFolder.root
                onClicked: {
                    root.session.folderModel.removeRoot(root.selectedFolder.entryId);
                    root.selectedFolder = ({});
                }
            }
        }
        Label {
            objectName: "editSourceEmptyTitle"
            text: root.currentTab === 0 ? qsTr("Drop media here") : root.currentTab === 1 ? qsTr("No folders added") : qsTr("No recordings yet")
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
            text: root.currentTab === 2 ? qsTr("Recent recordings appear here automatically.") : qsTr("Add files or a folder with +.")
            visible: sourceItems.count === 0
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
            model: root.currentTab === 0 ? root.session.mediaModel : root.currentTab === 1 ? root.session.folderModel : root.recordings.recentRecordingOptions
            ScrollBar.vertical: ExoScrollBar { policy: ScrollBar.AsNeeded }
            // The library owns its keys: Delete must not reach the timeline and
            // remove a clip selection the user can no longer see.
            Keys.onDeletePressed: event => event.accepted = true
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Backspace)
                    event.accepted = true;
            }
            delegate: Basic.ItemDelegate {
                id: sourceItem
                required property var modelData
                required property int index
                readonly property bool folderEntry: root.currentTab === 1
                readonly property string detail: folderEntry ? (modelData.error || (modelData.loading ? qsTr("Reading folder...") : ""))
                    : modelData.verificationReason || modelData.reason || (modelData.verifying ? qsTr("Verifying media...") : modelData.audioChoiceRequired ? qsTr("Choose an audio stream") : modelData.available === false ? qsTr("Media unavailable") : (modelData.metadata || ""))
                width: sourceItems.width
                height: folderEntry ? detail.length > 0 ? 60 : 38 : 66
                leftPadding: folderEntry ? 36 + Math.min(12, modelData.depth || 0) * 12 : thumbnail.visible ? 80 : 12
                readonly property bool insertable: !folderEntry || modelData.mediaCandidate === true
                rightPadding: (root.currentTab === 0 ? 38 : ExoTheme.spacingMd) + (insertable ? 30 : 0)
                highlighted: folderEntry ? root.selectedFolder.entryId === modelData.entryId : ListView.isCurrentItem
                text: modelData.name || modelData.label
                Accessible.name: text
                Accessible.description: sourceItem.ToolTip.text
                contentItem: Column {
                    spacing: 2
                    Label {
                        width: parent.width
                        text: sourceItem.text
                        elide: Text.ElideMiddle
                        color: ExoTheme.text
                        font.family: ExoTheme.sansFamily
                        font.pixelSize: ExoTheme.fontBody
                    }
                    Label {
                        width: parent.width
                        visible: sourceItem.detail.length > 0
                        text: sourceItem.detail
                        elide: Text.ElideRight
                        color: sourceItem.modelData.error || sourceItem.modelData.available === false ? ExoTheme.warning : ExoTheme.textSecondary
                        font.family: ExoTheme.sansFamily
                        font.pixelSize: ExoTheme.fontCaption
                    }
                }
                background: Rectangle {
                    color: sourceItem.down || sourceItem.highlighted ? ExoTheme.surfaceHover
                        : sourceItem.hovered || sourceItem.visualFocus ? ExoTheme.hoverTint(ExoTheme.surfaceRaised) : "transparent"
                    radius: ExoTheme.radiusSm
                    border.width: sourceItem.visualFocus ? 1 : 0
                    border.color: ExoTheme.accent
                }
                ExoGlyph {
                    x: sourceItem.leftPadding - 28
                    anchors.verticalCenter: parent.verticalCenter
                    width: 20
                    height: 20
                    visible: sourceItem.folderEntry
                    kind: sourceItem.modelData.directory ? ExoGlyph.Folder : ExoGlyph.AppWindow
                    color: ExoTheme.textSecondary
                    Accessible.ignored: true
                }
                Image {
                    id: thumbnail
                    x: 4
                    y: 8
                    width: 68
                    height: 48
                    source: !sourceItem.folderEntry && root.timeline.sourceRevision > 0 && sourceItem.modelData.sourceIdentity ? root.timeline.posterForIdentity(sourceItem.modelData.sourceIdentity, Math.ceil(48 * Screen.devicePixelRatio)) : ""
                    sourceSize: Qt.size(72 * Screen.devicePixelRatio, 48 * Screen.devicePixelRatio)
                    asynchronous: true
                    cache: false
                    visible: status === Image.Ready
                    fillMode: Image.PreserveAspectFit
                    Accessible.ignored: true
                }
                EditActionButton {
                    objectName: "editSourceInsert-" + sourceItem.index
                    anchors.right: parent.right
                    anchors.rightMargin: root.currentTab === 0 ? 34 : 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 28
                    visible: sourceItem.insertable
                    glyph: ExoGlyph.Plus
                    text: qsTr("Insert %1 at the playhead").arg(sourceItem.text)
                    onClicked: sourceItem.insert()
                }
                EditActionButton {
                    objectName: "editSourceActions-" + sourceItem.modelData.id
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 28
                    visible: root.currentTab === 0
                    glyph: ExoGlyph.Overflow
                    text: qsTr("Actions for %1").arg(sourceItem.text)
                    onClicked: sourceMenu.open()
                    ExoMenu {
                        id: sourceMenu
                        y: parent.height
                        ExoMenuItem {
                            objectName: "editSourceRelink-" + sourceItem.modelData.id
                            text: qsTr("Relink source…")
                            onTriggered: {
                                root.replacementAsset = sourceItem.modelData.id;
                                replacementPicker.open();
                            }
                        }
                    }
                }
                // Click and double click only choose; the document changes only
                // through Enter, the insert button or a drop.
                onClicked: {
                    sourceItems.currentIndex = index;
                    if (folderEntry) root.selectedFolder = modelData;
                }
                onDoubleClicked: {
                    sourceItems.currentIndex = index;
                    if (folderEntry && modelData.directory)
                        root.session.folderModel.toggle(modelData.entryId);
                }
                Keys.onReturnPressed: insert()
                Keys.onEnterPressed: insert()
                Keys.onRightPressed: if (folderEntry && modelData.directory && !modelData.expanded) root.session.folderModel.toggle(modelData.entryId)
                Keys.onLeftPressed: if (folderEntry && modelData.directory && modelData.expanded) root.session.folderModel.toggle(modelData.entryId)
                function insert(): void {
                    sourceItems.currentIndex = index;
                    const at = root.session.positionUs;
                    if (folderEntry) {
                        root.selectedFolder = modelData;
                        if (modelData.directory)
                            root.session.folderModel.toggle(modelData.entryId);
                        else if (modelData.mediaCandidate)
                            root.session.routeMediaDrop([modelData.url], true, at);
                    } else if (root.currentTab === 2) {
                        root.session.addHistoryUs(modelData.path, at);
                    } else {
                        root.session.appendAssetUs(modelData.id, at);
                    }
                }
                Drag.active: drag.active
                Drag.supportedActions: Qt.CopyAction
                Drag.dragType: Drag.Automatic
                Drag.mimeData: folderEntry ? {"text/uri-list": String(modelData.url)} : root.currentTab === 2 ? {
                    "application/x-exosnap-history": modelData.path
                } : {"application/x-exosnap-asset": String(modelData.id)}
                DragHandler { id: drag; target: null }
                ToolTip.visible: hovered || activeFocus
                ToolTip.text: (folderEntry ? String(modelData.url) : sourceItem.text + (modelData.path ? "\n" + modelData.path : "")) + (detail.length > 0 ? "\n" + detail : "")
            }
        }
    }
    FileDialog {
        id: picker
        title: qsTr("Add files")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Media files (*.mkv *.mp4 *.mov *.webm *.avi *.wav *.mp3 *.flac *.m4a *.aac *.ogg *.opus)"), qsTr("All files (*)")]
        onAccepted: root.session.importMediaBatch(selectedFiles)
    }
    FileDialog {
        id: replacementPicker
        title: qsTr("Choose replacement source")
        fileMode: FileDialog.OpenFile
        nameFilters: picker.nameFilters
        onAccepted: root.session.beginSourceReplacement(root.replacementAsset, selectedFile)
    }
    EditSourceReplacement {
        session: root.session
        parent: Overlay.overlay
    }
    FolderDialog {
        id: folderPicker
        title: qsTr("Add folder")
        onAccepted: {
            root.session.folderModel.addRoot(folderPicker.selectedFolder);
            root.currentTab = 1;
        }
    }
    DropArea {
        anchors.fill: parent
        onDropped: drop => {
            if (drop.hasUrls) {
                root.session.routeMediaDrop(drop.urls, false);
                drop.acceptProposedAction();
            }
        }
    }
}
