pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    required property AudioRoutingAdapter routing
    required property var track
    objectName: "audioTrack-" + track.id
    property bool renaming: false
    property bool cancelRename: false
    readonly property bool editable: !routing.readOnly
    implicitHeight: content.implicitHeight + 2 * ExoTheme.spacingMd
    signal sourcesRequested(Item anchor, string trackId)
    signal routeRequested(Item anchor, string trackId, string routeId)
    signal menuRequested(Item anchor, string trackId)

    function beginRename(): void {
        cancelRename = false;
        renaming = true;
        nameField.forceActiveFocus();
        nameField.selectAll();
    }
    function move(delta: int): void {
        if (editable && track.position + delta > 0 && track.position + delta <= routing.tracks.length)
            routing.reorderTrackById(track.id, delta, routing.revision);
    }
    activeFocusOnTab: true
    Accessible.role: Accessible.ListItem
    Accessible.name: qsTr("Track %1: %2").arg(track.position).arg(track.name)
    Accessible.description: qsTr("Alt+Up or Alt+Down changes track order.")
    Keys.onPressed: event => {
        if ((event.modifiers & Qt.AltModifier) && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
            root.move(event.key === Qt.Key_Up ? -1 : 1);
            event.accepted = true;
        }
    }
    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: dropArea.containsDrag || root.activeFocus ? ExoTheme.accent : ExoTheme.line
    }
    DropArea {
        id: dropArea
        anchors.fill: parent
        keys: ["exosnap-audio-track"]
        onDropped: drop => {
            if (root.editable && drop.source && drop.source.track.id !== root.track.id) {
                drop.acceptProposedAction();
                root.routing.moveTrackTo(drop.source.track.id, root.track.position - 1, root.routing.revision);
            }
        }
    }
    RowLayout {
        id: content
        anchors {
            left: parent.left
            right: parent.right
            verticalCenter: parent.verticalCenter
        }
        spacing: ExoTheme.spacingSm
        ColumnLayout {
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: ExoTheme.controlHeightCompact
            spacing: ExoTheme.spacingXs
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: String(root.track.position).padStart(2, "0")
                color: ExoTheme.textDim
                font.pixelSize: ExoTheme.fontSecondary
            }
            ExoButton {
                id: grip
                objectName: "audioTrackGrip"
                compact: true
                quiet: true
                glyph: ExoGlyph.Grip
                enabled: root.editable
                text: qsTr("Reorder %1").arg(root.track.name)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Drag to reorder, or use Alt+Up / Alt+Down")
                onClicked: root.forceActiveFocus()
                DragHandler {
                    id: dragHandler
                    target: null
                    onActiveChanged: {
                        if (active) {
                            dragToken.Drag.active = true;
                        } else {
                            dragToken.Drag.drop();
                            dragToken.Drag.active = false;
                        }
                    }
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: ExoTheme.spacingSm
            RowLayout {
                Layout.fillWidth: true
                spacing: ExoTheme.spacingSm
                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.maximumWidth: implicitWidth
                    visible: !root.renaming
                    text: root.track.name
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: root.track.recordEnabled ? ExoTheme.text : ExoTheme.textDim
                    font.weight: Font.DemiBold
                    font.strikeout: !root.track.recordEnabled
                }
                ExoTextField {
                    id: nameField
                    objectName: "audioTrackName"
                    Layout.fillWidth: true
                    visible: root.renaming
                    enabled: root.editable
                    value: root.track.name
                    Accessible.name: qsTr("Track name")
                    onCommitted: value => {
                        if (!root.renaming || root.cancelRename)
                            return;
                        root.renaming = false;
                        root.routing.setTrackName(root.track.id, value, root.routing.revision);
                    }
                    Keys.onEscapePressed: event => {
                        root.cancelRename = true;
                        root.renaming = false;
                        root.forceActiveFocus();
                        event.accepted = true;
                    }
                }
                Label {
                    visible: root.track.recordEnabled && root.track.empty
                    text: qsTr("No source")
                    color: ExoTheme.warning
                    font.pixelSize: ExoTheme.fontSecondary
                }
                Item {
                    Layout.fillWidth: true
                    visible: !root.renaming
                }
                ExoGlyph {
                    visible: root.track.recordEnabled && !root.track.empty && root.track.defaultMonitored
                    kind: ExoGlyph.Speaker
                    color: ExoTheme.accent
                    Layout.preferredWidth: 14
                    Layout.preferredHeight: 14
                    Accessible.name: qsTr("Default playback track")
                }
            }
            Flow {
                id: routeFlow
                Layout.fillWidth: true
                spacing: ExoTheme.spacingXs
                Repeater {
                    model: root.track.routes
                    delegate: ExoButton {
                        id: chip
                        objectName: "audioRoute-" + modelData.id
                        required property var modelData
                        compact: true
                        width: Math.min(implicitWidth, routeFlow.width)
                        text: modelData.name
                        leadingGlyph: modelData.bindingKind === 1 ? ExoGlyph.Mic : ExoGlyph.Speaker
                        enabled: root.editable
                        ToolTip.visible: hovered
                        ToolTip.text: modelData.name + (modelData.muted || modelData.sourceMuted || !modelData.participation || !modelData.enabled ? qsTr(" (muted or disabled)") : "")
                        Accessible.name: qsTr("Gain and assignment for %1 on %2").arg(modelData.name).arg(root.track.name)
                        onClicked: root.routeRequested(chip, root.track.id, modelData.id)
                    }
                }
                ExoButton {
                    id: addSource
                    objectName: "audioTrackSources"
                    text: qsTr("Source")
                    leadingGlyph: ExoGlyph.Plus
                    compact: true
                    quiet: true
                    enabled: root.editable
                    Accessible.name: qsTr("Choose sources for %1").arg(root.track.name)
                    onClicked: root.sourcesRequested(addSource, root.track.id)
                }
            }
        }
        ExoButton {
            id: menuButton
            objectName: "audioTrackMenu"
            Layout.alignment: Qt.AlignTop
            glyph: ExoGlyph.Overflow
            text: qsTr("Actions for %1").arg(root.track.name)
            compact: true
            quiet: true
            enabled: root.editable
            onClicked: root.menuRequested(menuButton, root.track.id)
        }
    }
    Item {
        id: dragToken
        width: 1
        height: 1
        x: grip.mapToItem(root, dragHandler.centroid.position.x, 0).x
        y: grip.mapToItem(root, 0, dragHandler.centroid.position.y).y
        Drag.source: root
        Drag.keys: ["exosnap-audio-track"]
        Drag.supportedActions: Qt.MoveAction
    }
}
