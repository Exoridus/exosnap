pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    required property AudioRoutingAdapter routing
    required property var track
    property string errorText: ""
    property bool showMeter: false
    property bool renaming: false
    property bool cancelRename: false
    readonly property bool editable: !routing.readOnly
    readonly property var liveMeter: routing.trackMeters[track.id]
    readonly property bool meterAvailable: showMeter && !!liveMeter && liveMeter.available
    readonly property string meterValueText: !meterAvailable ? "" : liveMeter.rmsDbfs === Number.NEGATIVE_INFINITY ? qsTr("−∞ dBFS") : qsTr("%1 dBFS").arg(Number(liveMeter.rmsDbfs).toLocaleString(Qt.locale(), 'f', 1))
    objectName: "audioTrack-" + track.id
    implicitHeight: content.implicitHeight + 2 * ExoTheme.spacingMd
    signal sourcesRequested(Item anchor, string trackId)
    signal routeRequested(Item anchor, string trackId, string routeId)
    signal menuRequested(Item anchor, string trackId)
    signal operationRequested(string target, var operation)

    function beginRename(): void {
        cancelRename = false;
        renaming = true;
        nameField.forceActiveFocus();
        nameField.selectAll();
    }
    function move(delta: int): void {
        if (editable && track.position + delta > 0 && track.position + delta <= routing.tracks.length)
            operationRequested(track.id, () => routing.reorderTrackById(track.id, delta, routing.revision));
    }
    activeFocusOnTab: true
    Accessible.role: Accessible.ListItem
    Accessible.name: qsTr("Track %1: %2").arg(track.position).arg(track.name)
    Accessible.description: errorText !== "" ? errorText : qsTr("Alt+Up or Alt+Down changes track order.")
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
                const sourceId = drop.source.track.id;
                drop.acceptProposedAction();
                root.operationRequested(root.track.id, () => root.routing.moveTrackTo(sourceId, root.track.position - 1, root.routing.revision));
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
            id: gutter
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: ExoTheme.controlHeightCompact
            spacing: ExoTheme.spacingXs
            Label {
                id: number
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 24
                text: String(root.track.position).padStart(2, "0")
                color: ExoTheme.textDim
                font.family: ExoTheme.monoFamily
                font.pixelSize: ExoTheme.fontSecondary
                horizontalAlignment: Text.AlignHCenter
            }
            ToolButton {
                id: grip
                objectName: "audioTrackGrip"
                visible: root.routing.tracks.length > 1
                enabled: root.editable
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 22
                Layout.preferredHeight: ExoTheme.controlHeightCompact
                text: qsTr("Reorder %1").arg(root.track.name)
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Drag to reorder, or use Alt+Up / Alt+Down")
                onClicked: root.forceActiveFocus()
                contentItem: ExoGlyph {
                    kind: ExoGlyph.Grip
                    color: ExoTheme.textDim
                    implicitWidth: 14
                    implicitHeight: 14
                }
                background: Rectangle {
                    color: "transparent"
                    radius: ExoTheme.radiusSm
                    border.width: grip.visualFocus ? 1 : 0
                    border.color: ExoTheme.accent
                }
                HoverHandler {
                    cursorShape: grip.enabled ? (dragHandler.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor) : Qt.ArrowCursor
                }
                DragHandler {
                    id: dragHandler
                    enabled: grip.enabled
                    target: null
                    cursorShape: active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
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
                id: header
                Layout.fillWidth: true
                spacing: ExoTheme.spacingSm
                Label {
                    id: nameLabel
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    visible: !root.renaming
                    text: root.track.name
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: root.track.recordEnabled ? ExoTheme.text : ExoTheme.textDim
                    font.weight: Font.DemiBold
                    font.strikeout: !root.track.recordEnabled
                    Layout.maximumWidth: root.track.recordEnabled && root.track.empty ? implicitWidth : Number.POSITIVE_INFINITY
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
                        root.operationRequested(root.track.id, () => root.routing.setTrackName(root.track.id, value, root.routing.revision));
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
                    visible: root.track.recordEnabled && root.track.empty && !root.renaming
                }
                Rectangle {
                    id: meter
                    objectName: "audioTrackMeter"
                    visible: root.meterAvailable
                    Layout.preferredWidth: 72
                    Layout.preferredHeight: 4
                    radius: 2
                    color: ExoTheme.line
                    Accessible.role: Accessible.Indicator
                    Accessible.name: qsTr("Active track RMS")
                    Accessible.description: root.meterValueText
                    Rectangle {
                        height: parent.height
                        width: root.meterAvailable ? parent.width * Math.max(0, Math.min(1, (root.liveMeter.rmsDbfs + 60) / 60)) : 0
                        radius: parent.radius
                        color: ExoTheme.accent
                    }
                    HoverHandler {
                        id: meterHover
                    }
                    ToolTip.visible: meterHover.hovered
                    ToolTip.text: qsTr("Active recording output RMS: %1").arg(root.meterValueText)
                }
                ExoButton {
                    id: menuButton
                    objectName: "audioTrackMenu"
                    glyph: ExoGlyph.Overflow
                    text: qsTr("Actions for %1").arg(root.track.name)
                    compact: true
                    quiet: true
                    enabled: root.editable
                    onClicked: root.menuRequested(menuButton, root.track.id)
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
            Label {
                objectName: "audioTrackError"
                Layout.fillWidth: true
                visible: root.errorText !== ""
                text: root.errorText
                color: ExoTheme.error
                wrapMode: Text.WordWrap
                Accessible.role: Accessible.AlertMessage
                Accessible.name: text
            }
        }
    }
    Rectangle {
        id: dragToken
        parent: Overlay.overlay ? Overlay.overlay : root
        visible: dragHandler.active
        z: 100
        width: Math.min(240, dragName.implicitWidth + 2 * ExoTheme.spacingMd)
        height: ExoTheme.controlHeightCompact
        x: grip.mapToItem(parent, dragHandler.centroid.position.x, dragHandler.centroid.position.y).x + 12
        y: grip.mapToItem(parent, dragHandler.centroid.position.x, dragHandler.centroid.position.y).y + 12
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.lineStrong
        radius: ExoTheme.radiusSm
        Label {
            id: dragName
            anchors.fill: parent
            anchors.margins: ExoTheme.spacingSm
            text: qsTr("%1  %2").arg(root.track.position).arg(root.track.name)
            textFormat: Text.PlainText
            color: ExoTheme.text
            font.pixelSize: ExoTheme.fontSecondary
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        Drag.source: root
        Drag.keys: ["exosnap-audio-track"]
        Drag.supportedActions: Qt.MoveAction
    }
}
