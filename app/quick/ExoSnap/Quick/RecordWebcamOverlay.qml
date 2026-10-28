pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FocusScope {
    id: webcamOverlay

    // The stored placement, normalized to the content rectangle the parent
    // spans. The overlay edits a draft of it and asks for the result through
    // rectRequested; the draft follows this again once no gesture holds it.
    property rect modelRect: Qt.rect(0, 0, 1, 1)
    property bool editable: false
    property bool error: false
    property bool idlePreview: true
    property bool selected: activeFocus
    property bool outputVisible: true
    property real toolbarTopInset: 0
    property string overlayName: qsTr("Webcam overlay")
    // The item gestures are measured in. It must not move with the overlay.
    property Item stage: parent

    signal rectRequested(rect rect)
    signal selectionRequested()
    signal outputVisibilityRequested(bool visible)
    signal removeRequested()

    property rect draftRect: Qt.rect(0, 0, 1, 1)
    // Corner index (0 top-left, clockwise) of the grip being
    // held or hovered, -1 for none.
    property int resizeCorner: -1
    property int hoverCorner: -1

    x: parent.width * draftRect.x
    y: parent.height * draftRect.y
    width: parent.width * draftRect.width
    height: parent.height * draftRect.height
    z: selected ? 2 : 1
    activeFocusOnTab: webcamOverlay.editable
    Accessible.name: webcamOverlay.overlayName
    Accessible.description: qsTr("Drag to move. Use arrow keys to move and Shift plus arrow keys to resize.")
    onActiveFocusChanged: {
        if (activeFocus && editable)
            selectionRequested()
    }
    Keys.onPressed: event => {
        if (!webcamOverlay.editable)
            return
        if (event.key === Qt.Key_Delete && webcamOverlay.selected) {
            webcamOverlay.removeRequested()
            event.accepted = true
            return
        }
        const step = Boolean(event.modifiers & Qt.ControlModifier) ? 0.01 : 0.02
        const resize = Boolean(event.modifiers & Qt.ShiftModifier)
        let x = webcamOverlay.draftRect.x
        let y = webcamOverlay.draftRect.y
        let w = webcamOverlay.draftRect.width
        let h = webcamOverlay.draftRect.height
        if (event.key === Qt.Key_Left)
            resize ? w = Math.max(0.05, w - step) : x = Math.max(0, x - step)
        else if (event.key === Qt.Key_Right)
            resize ? w = Math.min(1 - x, w + step) : x = Math.min(1 - w, x + step)
        else if (event.key === Qt.Key_Up)
            resize ? h = Math.max(0.05, h - step) : y = Math.max(0, y - step)
        else if (event.key === Qt.Key_Down)
            resize ? h = Math.min(1 - y, h + step) : y = Math.min(1 - h, y + step)
        else
            return
        const nextRect = Qt.rect(x, y, w, h)
        webcamOverlay.rectRequested(nextRect)
        event.accepted = true
    }

    Rectangle {
        color: ExoTheme.surface
        radius: ExoTheme.radiusSm
        visible: webcamOverlay.idlePreview && webcamOverlay.error
        anchors.fill: parent
    }

    Binding {
        target: webcamOverlay
        property: "draftRect"
        value: webcamOverlay.modelRect
        when: !webcamDrag.pressed && webcamOverlay.resizeCorner < 0
        restoreMode: Binding.RestoreNone
    }

    Label {
        text: qsTr("Webcam preview unavailable")
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        // The theme rung, not the overlay one: this label
        // only ever appears in the idle preview, where the
        // panel behind it is `ExoTheme.surface`.
        color: ExoTheme.warningText
        visible: webcamOverlay.idlePreview && webcamOverlay.error
        anchors.fill: parent
        font.family: ExoTheme.sansFamily
        font.pixelSize: ExoTheme.fontCaption
    }

    Rectangle {
        id: webcamFrameChrome

        color: Qt.rgba(0, 0, 0, 0)
        border.width: 1
        border.color: webcamOverlay.error ? ExoTheme.error : ExoTheme.overlayAccent
        radius: ExoTheme.radiusSm
        // `pressed` as well as `containsMouse`: a drag that
        // leaves the frame's own bounds is still a drag, and
        // chrome that blinks out mid-gesture reads as a dropped
        // grab.
        visible: webcamOverlay.selected
                 || webcamDrag.pressed || webcamOverlay.resizeCorner >= 0
        anchors.fill: parent
    }

    // The four corners of the editable frame. What was here was a
    // single filled square at the bottom right, which reads as a
    // stray box rather than as a frame with corners; brackets
    // state all four without covering the picture.
    Repeater {
        model: 4

        delegate: Item {
            id: webcamCorner

            required property int index

            readonly property bool atRight: webcamCorner.index === 1 || webcamCorner.index === 2
            readonly property bool atBottom: webcamCorner.index >= 2
            // Every corner is a resize grip; the one under the
            // pointer or in hand takes the accent.
            readonly property bool engaged: webcamOverlay.resizeCorner === webcamCorner.index
                                            || (webcamOverlay.resizeCorner < 0
                                                && webcamOverlay.hoverCorner === webcamCorner.index)
            readonly property color ink: webcamCorner.engaged
                ? ExoTheme.accent
                : (webcamOverlay.error ? ExoTheme.error : ExoTheme.overlayAccent)

            width: 14
            height: 14
            visible: webcamFrameChrome.visible
            x: webcamCorner.atRight ? parent.width - width : 0
            y: webcamCorner.atBottom ? parent.height - height : 0

            Rectangle {
                width: parent.width
                height: 2
                color: webcamCorner.ink
                y: webcamCorner.atBottom ? parent.height - height : 0
            }

            Rectangle {
                width: 2
                height: parent.height
                color: webcamCorner.ink
                x: webcamCorner.atRight ? parent.width - width : 0
            }
        }
    }

    MouseArea {
        id: webcamDrag

        property point pressPoint
        property rect pressRect

        anchors.fill: parent
        enabled: webcamOverlay.editable
        hoverEnabled: true
        cursorShape: enabled ? Qt.SizeAllCursor : Qt.ArrowCursor
        // Both points live in webcamOverlay.stage coordinates, never in
        // this MouseArea's: the overlay MOVES as draftRect
        // changes, so a delta measured against the item itself
        // shrinks by exactly the amount already applied. The drag
        // then crawls behind the pointer and stalls once it
        // catches up, without ever losing the grab -- which is
        // what makes it look like lag rather than like a bug.
        onPressed: mouse => {
            webcamOverlay.forceActiveFocus()
            webcamOverlay.selectionRequested()
            pressPoint = webcamDrag.mapToItem(webcamOverlay.stage, mouse.x, mouse.y)
            pressRect = webcamOverlay.draftRect
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const at = webcamDrag.mapToItem(webcamOverlay.stage, mouse.x, mouse.y)
            const dx = (at.x - pressPoint.x) / Math.max(1, webcamOverlay.stage.width)
            const dy = (at.y - pressPoint.y) / Math.max(1, webcamOverlay.stage.height)
            webcamOverlay.draftRect = Qt.rect(
                        Math.max(0, Math.min(1 - pressRect.width, pressRect.x + dx)),
                        Math.max(0, Math.min(1 - pressRect.height, pressRect.y + dy)),
                        pressRect.width, pressRect.height)
        }
        onReleased: webcamOverlay.rectRequested(webcamOverlay.draftRect)
    }

    // Hit areas only; the corner brackets above are what the
    // eye reads. Each grip resizes from its own corner and
    // keeps the opposite one where it is.
    Repeater {
        model: 4

        delegate: Item {
            id: webcamGrip

            required property int index

            readonly property int sx: webcamGrip.index === 1 || webcamGrip.index === 2 ? 1 : -1
            readonly property int sy: webcamGrip.index >= 2 ? 1 : -1

            width: 14
            height: 14
            x: webcamGrip.sx > 0 ? parent.width - width : 0
            y: webcamGrip.sy > 0 ? parent.height - height : 0

            MouseArea {
                id: gripArea

                property point pressPoint
                property rect pressRect

                anchors.fill: parent
                enabled: webcamOverlay.editable
                hoverEnabled: true
                cursorShape: webcamGrip.sx === webcamGrip.sy ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
                onContainsMouseChanged: {
                    if (containsMouse)
                        webcamOverlay.hoverCorner = webcamGrip.index
                    else if (webcamOverlay.hoverCorner === webcamGrip.index)
                        webcamOverlay.hoverCorner = -1
                }
                // webcamOverlay.stage coordinates for the same reason the
                // drag uses them: the grip is anchored to a frame
                // that moves and grows under it.
                onPressed: mouse => {
                    mouse.accepted = true
                    webcamOverlay.forceActiveFocus()
                    webcamOverlay.selectionRequested()
                    pressPoint = gripArea.mapToItem(webcamOverlay.stage, mouse.x, mouse.y)
                    pressRect = webcamOverlay.draftRect
                    webcamOverlay.resizeCorner = webcamGrip.index
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const at = gripArea.mapToItem(webcamOverlay.stage, mouse.x, mouse.y)
                    const dw = webcamGrip.sx * (at.x - pressPoint.x) / Math.max(1, webcamOverlay.stage.width)
                    const dh = webcamGrip.sy * (at.y - pressPoint.y) / Math.max(1, webcamOverlay.stage.height)
                    const roomX = webcamGrip.sx > 0 ? 1 - pressRect.x - pressRect.width : pressRect.x
                    const roomY = webcamGrip.sy > 0 ? 1 - pressRect.y - pressRect.height : pressRect.y
                    const minDelta = 0.05 - Math.min(pressRect.width, pressRect.height)
                    const delta = Math.max(minDelta, Math.min(Math.min(roomX, roomY), Math.max(dw, dh)))
                    const w = pressRect.width + delta
                    const h = pressRect.height + delta
                    webcamOverlay.draftRect = Qt.rect(
                                webcamGrip.sx > 0 ? pressRect.x : pressRect.x + pressRect.width - w,
                                webcamGrip.sy > 0 ? pressRect.y : pressRect.y + pressRect.height - h,
                                w, h)
                }
                onReleased: mouse => {
                    mouse.accepted = true
                    // Request first: clearing the corner re-arms the
                    // Binding that copies the model rect back into
                    // draftRect, which would then send the old size.
                    webcamOverlay.rectRequested(webcamOverlay.draftRect)
                    webcamOverlay.resizeCorner = -1
                }
                onCanceled: webcamOverlay.resizeCorner = -1
            }
        }
    }

    Rectangle {
        id: toolbar
        objectName: "overlayToolbar"
        z: 10
        width: 72
        height: 32
        x: Math.max(4 - webcamOverlay.x,
                    Math.min((webcamOverlay.width - width) / 2,
                             webcamOverlay.stage.width - webcamOverlay.x - width - 4))
        y: webcamOverlay.y >= webcamOverlay.toolbarTopInset + height + 6 ? -height - 6
           : webcamOverlay.height < height + 28 ? Math.max(webcamOverlay.height + 6, webcamOverlay.toolbarTopInset - webcamOverlay.y)
           : Math.max(18, webcamOverlay.toolbarTopInset - webcamOverlay.y)
        visible: webcamOverlay.selected && webcamOverlay.editable
        color: ExoTheme.overlaySurfaceRaised
        border.color: ExoTheme.overlayLine
        radius: ExoTheme.radiusSm

        RowLayout {
            anchors.fill: parent
            anchors.margins: 2
            spacing: 2
            ExoButton {
                objectName: "overlayOutputButton"
                Layout.fillWidth: true
                Layout.fillHeight: true
                compact: true
                glyph: webcamOverlay.outputVisible ? ExoGlyph.Eye : ExoGlyph.EyeClosed
                leftPadding: 4
                rightPadding: 4
                text: webcamOverlay.outputVisible ? qsTr("Hide from recording") : qsTr("Include in recording")
                Accessible.description: webcamOverlay.overlayName
                ToolTip.visible: hovered
                ToolTip.text: text
                onClicked: webcamOverlay.outputVisibilityRequested(!webcamOverlay.outputVisible)
            }
            ExoButton {
                objectName: "overlayRemoveButton"
                Layout.fillWidth: true
                Layout.fillHeight: true
                compact: true
                glyph: ExoGlyph.Close
                leftPadding: 4
                rightPadding: 4
                text: qsTr("Remove overlay")
                Accessible.description: webcamOverlay.overlayName
                ToolTip.visible: hovered
                ToolTip.text: text
                onClicked: webcamOverlay.removeRequested()
            }
        }
    }
}
