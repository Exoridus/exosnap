import QtQuick
import QtQuick.Controls

FocusScope {
    id: root

    required property RecordViewModelAdapter recordViewModel

    // Physical size of the anchored monitor, which owns the recorded region.
    // The selector window can use a different logical size under display scaling.
    property size sourcePixelSize: Qt.size(0, 0)

    // An empty rect means "nothing selected yet": Draw custom starts there, a
    // preset fills it with an editable starting rectangle.
    property rect selectionNormalized: Qt.rect(0, 0, 0, 0)
    readonly property bool hasSelection: selectionNormalized.width > 0 && selectionNormalized.height > 0
    readonly property rect selection: Qt.rect(selectionNormalized.x * width,
                                                selectionNormalized.y * height,
                                                selectionNormalized.width * width,
                                                selectionNormalized.height * height)
    // False while the user is still composing the rectangle; true from the
    // countdown onward, when the capture owns the region and the editing
    // affordances hide and lock.
    readonly property bool editingLocked: recordViewModel.regionEditingLocked

    // -1 none; 0..3 the four corners (TL, TR, BL, BR); 4 moving the rectangle.
    property int dragMode: -1
    property point dragStart: Qt.point(0, 0)
    property rect dragOriginRect: Qt.rect(0, 0, 0, 0)

    readonly property real minWidth: sourcePixelSize.width > 0 ? Math.min(1, 64 / sourcePixelSize.width) : 1
    readonly property real minHeight: sourcePixelSize.height > 0 ? Math.min(1, 64 / sourcePixelSize.height) : 1

    activeFocusOnTab: true
    focus: visible
    Accessible.name: qsTr("Capture region selector")
    Accessible.description: qsTr("Drag with the mouse, or use arrow keys to move the region and Shift plus arrow keys to resize it.")

    // The Region tab routes its choice through the adapter rather than through
    // QML-to-QML wiring, so the picker and this overlay share only the C++
    // boundary.
    Connections {
        target: root.recordViewModel

        function onChanged(): void {
            if (!root.recordViewModel.regionSelectionNeeded)
                root.beginDraw()
        }

        function onRegionPresetRequested(key: string): void {
            root.applyPreset(key)
        }
    }

    function beginDraw(): void {
        selectionNormalized = Qt.rect(0, 0, 0, 0)
    }

    function applyPreset(key: string): void {
        const rows = recordViewModel.regionPresetOptions
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            if (row.key !== key)
                continue
            if (row.draw) {
                beginDraw()
            } else {
                if (sourcePixelSize.width <= 0 || sourcePixelSize.height <= 0)
                    return
                // The ratio is in physical pixels; normalized x and y use
                // different denominators on a non-square monitor.
                const monitorAspect = sourcePixelSize.width / sourcePixelSize.height
                let w = 0.7
                let h = w * monitorAspect / row.aspect
                if (h > 0.7) {
                    h = 0.7
                    w = h * row.aspect / monitorAspect
                }
                selectionNormalized = Qt.rect((1 - w) / 2, (1 - h) / 2, w, h)
            }
            return
        }
    }

    function setSelectionEdges(left: real, top: real, right: real, bottom: real): void {
        const w = Math.min(1, Math.max(minWidth, Math.abs(right - left)))
        const h = Math.min(1, Math.max(minHeight, Math.abs(top - bottom)))
        const x = Math.max(0, Math.min(1 - w, Math.min(left, right)))
        const y = Math.max(0, Math.min(1 - h, Math.min(top, bottom)))
        selectionNormalized = Qt.rect(x, y, w, h)
    }

    function moveBy(dx: real, dy: real): void {
        const x = Math.max(0, Math.min(1 - selectionNormalized.width, selectionNormalized.x + dx))
        const y = Math.max(0, Math.min(1 - selectionNormalized.height, selectionNormalized.y + dy))
        selectionNormalized = Qt.rect(x, y, selectionNormalized.width, selectionNormalized.height)
    }

    function resizeFromHandle(handle: int, pos: point): void {
        const px = pos.x / Math.max(1, width)
        const py = pos.y / Math.max(1, height)
        const origin = dragOriginRect
        let left = origin.x
        let top = origin.y
        let right = origin.x + origin.width
        let bottom = origin.y + origin.height
        if (handle === 0) {
            left = px
            top = py
        } else if (handle === 1) {
            right = px
            top = py
        } else if (handle === 2) {
            left = px
            bottom = py
        } else {
            right = px
            bottom = py
        }
        setSelectionEdges(left, top, right, bottom)
    }

    Keys.onEscapePressed: event => {
        if (editingLocked)
            return
        recordViewModel.requestSelectTarget(recordViewModel.selectedTargetIndex, 0)
        event.accepted = true
    }
    Keys.onReturnPressed: event => {
        if (editingLocked || !hasSelection)
            return
        recordViewModel.requestSelectRegion(selectionNormalized)
        event.accepted = true
    }
    Keys.onPressed: event => {
        if (editingLocked || !hasSelection)
            return
        const step = (event.modifiers & Qt.ControlModifier) ? 0.01 : 0.02
        let x = selectionNormalized.x
        let y = selectionNormalized.y
        let w = selectionNormalized.width
        let h = selectionNormalized.height
        const resize = Boolean(event.modifiers & Qt.ShiftModifier)
        if (event.key === Qt.Key_Left)
            resize ? w = Math.max(minWidth, w - step) : x = Math.max(0, x - step)
        else if (event.key === Qt.Key_Right)
            resize ? w = Math.min(1 - x, w + step) : x = Math.min(1 - w, x + step)
        else if (event.key === Qt.Key_Up)
            resize ? h = Math.max(minHeight, h - step) : y = Math.max(0, y - step)
        else if (event.key === Qt.Key_Down)
            resize ? h = Math.min(1 - y, h + step) : y = Math.min(1 - h, y + step)
        else
            return
        selectionNormalized = Qt.rect(x, y, w, h)
        event.accepted = true
    }

    Rectangle {
        width: parent.width
        height: root.hasSelection ? root.selection.y : parent.height
        color: "#80000000"
    }

    Rectangle {
        y: root.selection.y
        width: root.selection.x
        height: root.selection.height
        visible: root.hasSelection
        color: "#80000000"
    }

    Rectangle {
        x: root.selection.x + root.selection.width
        y: root.selection.y
        width: root.width - x
        height: root.selection.height
        visible: root.hasSelection
        color: "#80000000"
    }

    Rectangle {
        y: root.selection.y + root.selection.height
        width: parent.width
        height: root.height - y
        visible: root.hasSelection
        color: "#80000000"
    }

    MouseArea {
        anchors.fill: parent
        enabled: !root.editingLocked
        cursorShape: Qt.CrossCursor
        onPressed: mouse => {
            root.dragMode = -1
            root.dragStart = Qt.point(mouse.x, mouse.y)
            root.setSelectionEdges(mouse.x / root.width, mouse.y / root.height,
                                   mouse.x / root.width, mouse.y / root.height)
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const left = Math.max(0, Math.min(root.dragStart.x, mouse.x))
            const top = Math.max(0, Math.min(root.dragStart.y, mouse.y))
            const right = Math.min(root.width, Math.max(root.dragStart.x, mouse.x))
            const bottom = Math.min(root.height, Math.max(root.dragStart.y, mouse.y))
            root.setSelectionEdges(left / Math.max(1, root.width), top / Math.max(1, root.height),
                                   right / Math.max(1, root.width), bottom / Math.max(1, root.height))
        }
    }

    Rectangle {
        x: root.selection.x
        y: root.selection.y
        width: root.selection.width
        height: root.selection.height
        visible: root.hasSelection
        color: "transparent"
        border.width: 2
        border.color: ExoTheme.accent

        MouseArea {
            anchors.fill: parent
            enabled: !root.editingLocked
            cursorShape: Qt.SizeAllCursor
            property point lastPos: Qt.point(0, 0)
            onPressed: mouse => {
                root.dragMode = 4
                lastPos = Qt.point(mouse.x, mouse.y)
            }
            onPositionChanged: mouse => {
                if (root.dragMode !== 4)
                    return
                root.moveBy((mouse.x - lastPos.x) / Math.max(1, root.width),
                            (mouse.y - lastPos.y) / Math.max(1, root.height))
                lastPos = Qt.point(mouse.x, mouse.y)
            }
        }
    }

    component ResizeHandle: Rectangle {
        id: handle

        required property int index

        width: 12
        height: 12
        radius: 3
        color: ExoTheme.accent
        border.width: 2
        border.color: ExoTheme.surfaceRaised
        visible: root.hasSelection && !root.editingLocked
        // Corners in TL, TR, BL, BR order.
        x: (handle.index === 0 || handle.index === 2)
           ? root.selection.x - 6 : root.selection.x + root.selection.width - 6
        y: (handle.index === 0 || handle.index === 1)
           ? root.selection.y - 6 : root.selection.y + root.selection.height - 6

        MouseArea {
            anchors.fill: parent
            anchors.margins: -6
            enabled: !root.editingLocked
            cursorShape: (handle.index === 0 || handle.index === 3) ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
            onPressed: mouse => {
                root.dragMode = handle.index
                root.dragOriginRect = root.selectionNormalized
            }
            onPositionChanged: mouse => {
                if (root.dragMode !== handle.index)
                    return
                root.resizeFromHandle(handle.index, mapToItem(root, mouse.x, mouse.y))
            }
        }
    }

    ResizeHandle {
        index: 0
    }
    ResizeHandle {
        index: 1
    }
    ResizeHandle {
        index: 2
    }
    ResizeHandle {
        index: 3
    }

    Rectangle {
        id: dimensionLabel

        visible: root.hasSelection && root.sourcePixelSize.width > 0 && !root.editingLocked
        width: dimensionText.implicitWidth + 16
        height: dimensionText.implicitHeight + 10
        radius: ExoTheme.radiusSm
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.lineStrong
        // Above the rectangle's top-left edge, dropping inside it when there is
        // no room above.
        x: Math.max(4, Math.min(root.width - width - 4, root.selection.x))
        y: root.selection.y - height - 6 < 4
           ? root.selection.y + 6 : root.selection.y - height - 6

        Label {
            id: dimensionText

            anchors.centerIn: parent
            text: qsTr("%1 × %2 px")
                      .arg(Math.round(root.selectionNormalized.width * root.sourcePixelSize.width))
                      .arg(Math.round(root.selectionNormalized.height * root.sourcePixelSize.height))
            textFormat: Text.PlainText
            color: ExoTheme.text
            font.family: ExoTheme.monoFamily
            font.pixelSize: ExoTheme.fontCaption
        }
    }

    Rectangle {
        implicitWidth: instruction.implicitWidth + 24
        implicitHeight: instruction.implicitHeight + 16
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.lineStrong
        radius: ExoTheme.radiusSm
        visible: !root.hasSelection && !root.editingLocked
        anchors {
            top: parent.top
            horizontalCenter: parent.horizontalCenter
            topMargin: ExoTheme.spacingLg
        }

        Label {
            id: instruction

            text: qsTr("Drag to select · Arrow keys move · Shift + arrows resize")
            textFormat: Text.PlainText
            color: ExoTheme.text
            anchors.centerIn: parent
            font.family: ExoTheme.sansFamily
            font.pixelSize: ExoTheme.fontCaption
        }
    }

    ExoButton {
        text: qsTr("Use selected region")
        enabled: root.hasSelection && !root.editingLocked && root.selection.width >= 8 && root.selection.height >= 8
        onClicked: root.recordViewModel.requestSelectRegion(root.selectionNormalized)
        anchors {
            right: parent.right
            bottom: parent.bottom
            margins: ExoTheme.spacingLg
        }
    }
}
