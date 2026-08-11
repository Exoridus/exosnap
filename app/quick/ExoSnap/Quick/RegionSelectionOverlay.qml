import QtQuick
import QtQuick.Controls

FocusScope {
    id: root

    required property RecordViewModelAdapter recordViewModel
    property point dragStart: Qt.point(0, 0)
    property rect selectionNormalized: Qt.rect(0.15, 0.15, 0.7, 0.7)
    readonly property rect selection: Qt.rect(selectionNormalized.x * width,
                                               selectionNormalized.y * height,
                                               selectionNormalized.width * width,
                                               selectionNormalized.height * height)

    activeFocusOnTab: true
    focus: visible
    Accessible.name: qsTr("Capture region selector")
    Accessible.description: qsTr("Drag with the mouse, or use arrow keys to move the region and Shift plus arrow keys to resize it.")
    Keys.onEscapePressed: event => {
        root.recordViewModel.requestSelectTarget(root.recordViewModel.selectedTargetIndex, 0)
        event.accepted = true
    }
    Keys.onPressed: event => {
        const step = (event.modifiers & Qt.ControlModifier) ? 0.01 : 0.02
        let x = root.selectionNormalized.x
        let y = root.selectionNormalized.y
        let w = root.selectionNormalized.width
        let h = root.selectionNormalized.height
        const resize = Boolean(event.modifiers & Qt.ShiftModifier)
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
        root.selectionNormalized = Qt.rect(x, y, w, h)
        event.accepted = true
    }

    Rectangle {
        anchors.fill: parent
        color: "#80000000"
    }

    Rectangle {
        x: root.selection.x
        y: root.selection.y
        width: root.selection.width
        height: root.selection.height
        color: "#10000000"
        border.width: 2
        border.color: ExoTheme.accent
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.CrossCursor
        onPressed: mouse => {
            root.dragStart = Qt.point(mouse.x, mouse.y)
            root.selectionNormalized = Qt.rect(mouse.x / Math.max(1, root.width),
                                               mouse.y / Math.max(1, root.height),
                                               1 / Math.max(1, root.width),
                                               1 / Math.max(1, root.height))
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const left = Math.max(0, Math.min(root.dragStart.x, mouse.x))
            const top = Math.max(0, Math.min(root.dragStart.y, mouse.y))
            const right = Math.min(width, Math.max(root.dragStart.x, mouse.x))
            const bottom = Math.min(height, Math.max(root.dragStart.y, mouse.y))
            root.selectionNormalized = Qt.rect(left / Math.max(1, root.width),
                                               top / Math.max(1, root.height),
                                               (right - left) / Math.max(1, root.width),
                                               (bottom - top) / Math.max(1, root.height))
        }
    }

    Rectangle {
        implicitWidth: instruction.implicitWidth + 24
        implicitHeight: instruction.implicitHeight + 16
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.lineStrong
        radius: ExoTheme.radiusSm
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
            font.pixelSize: 12
        }
    }

    ExoButton {
        text: qsTr("Use selected region")
        enabled: root.selection.width >= 8 && root.selection.height >= 8
        onClicked: root.recordViewModel.requestSelectRegion(root.selectionNormalized)
        anchors {
            right: parent.right
            bottom: parent.bottom
            margins: ExoTheme.spacingLg
        }
    }
}
