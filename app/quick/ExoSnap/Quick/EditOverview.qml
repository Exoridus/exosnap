pragma ComponentBehavior: Bound
import QtQuick

Rectangle {
    id: root
    required property EditSessionAdapter session
    required property EditPlayerAdapter player
    required property EditViewportAdapter view
    required property EditInteractionController interaction
    required property Item canvasFocusItem
    property bool captured: false
    property bool spaceCapture: false
    implicitHeight: 12
    color: ExoTheme.surfaceRaised
    clip: true
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Timeline overview")
    Accessible.description: qsTr("Drag the window to pan or its edges to zoom. Double-click fits the timeline.")
    Canvas {
        id: density
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const context = getContext("2d");
            context.clearRect(0, 0, width, height);
            const rows = Math.max(1, root.session.tracks.length);
            const extent = root.view.navigationEndUs;
            const segments = root.session.overviewSegments;
            context.fillStyle = ExoTheme.textSecondary;
            for (const segment of segments) {
                context.globalAlpha = segment.enabled ? 0.5 : 0.16;
                context.fillRect(segment.startUs / extent * width, segment.trackIndex / rows * height,
                                 (segment.endUs - segment.startUs) / extent * width, height / rows);
            }
            context.globalAlpha = 1;
        }
        Connections {
            target: root.session
            function onOverviewChanged(): void { density.requestPaint(); }
        }
        Connections {
            target: root.view
            function onViewChanged(): void {
                if (density.lastExtent !== root.view.navigationEndUs) {
                    density.lastExtent = root.view.navigationEndUs;
                    density.requestPaint();
                }
            }
        }
        property real lastExtent: 0
        Component.onCompleted: lastExtent = root.view.navigationEndUs
        Connections {
            target: ExoTheme
            function onTextSecondaryChanged(): void { density.requestPaint(); }
        }
    }
    Rectangle {
        objectName: "editOverviewWindow"
        x: root.view.startUs / root.view.navigationEndUs * root.width
        width: (root.view.endUs - root.view.startUs) / root.view.navigationEndUs * root.width
        height: root.height
        color: Qt.rgba(ExoTheme.accent.r, ExoTheme.accent.g, ExoTheme.accent.b, 0.15)
        border.color: ExoTheme.accent
    }
    Rectangle {
        x: (root.player.loopRegion.startUs || 0) / root.view.navigationEndUs * root.width
        width: ((root.player.loopRegion.endUs || 0) - (root.player.loopRegion.startUs || 0)) / root.view.navigationEndUs * root.width
        height: 2
        color: ExoTheme.accent
        opacity: 0.45
        visible: root.player.loopEnabled
    }
    Rectangle {
        x: (root.session.editRange.startUs || 0) / root.view.navigationEndUs * root.width
        width: ((root.session.editRange.endUs || 0) - (root.session.editRange.startUs || 0)) / root.view.navigationEndUs * root.width
        height: 2
        anchors.bottom: parent.bottom
        color: ExoTheme.accent
        visible: root.session.selectionMode === EditSessionAdapter.RangeSelection
    }
    Rectangle {
        x: root.session.positionUs / root.view.navigationEndUs * root.width
        width: 1
        height: root.height
        color: ExoTheme.text
    }
    function finishCapture(): void {
        const space = root.spaceCapture;
        root.captured = false;
        root.spaceCapture = false;
        root.view.endOverviewOperation();
        if (space)
            root.interaction.endPan();
    }
    Connections {
        target: root.interaction
        function onGestureChanged(): void {
            if (root.spaceCapture && !root.interaction.pointerCaptured)
                root.finishCapture();
        }
    }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onPressed: mouse => {
            if (!root.session.preflightPendingDraft()) {
                mouse.accepted = false;
                return;
            }
            root.canvasFocusItem.forceActiveFocus(Qt.MouseFocusReason);
            root.spaceCapture = root.interaction.spaceHeld;
            if (root.spaceCapture)
                root.interaction.beginPan(Qt.point(mouse.x, mouse.y), false);
            root.captured = root.view.beginOverviewOperation(mouse.x, width, 6, root.spaceCapture);
        }
        onPositionChanged: mouse => {
            if (!pressed || !root.captured)
                return;
            if (root.spaceCapture) {
                root.interaction.updatePan(Qt.point(mouse.x, mouse.y));
                if (!root.interaction.spaceHeld || !root.interaction.panActive)
                    return;
            }
            root.view.updateOverviewOperation(mouse.x, width);
        }
        onReleased: root.finishCapture()
        onCanceled: root.finishCapture()
        onDoubleClicked: {
            root.finishCapture();
            root.view.fitTimeline();
        }
    }
    Component.onDestruction: if (root.captured) root.finishCapture()
}
