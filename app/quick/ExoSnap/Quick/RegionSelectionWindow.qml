import QtQuick

Window {
    id: root

    required property RecordViewModelAdapter recordViewModel
    property rect monitorGeometry: Qt.rect(0, 0, 0, 0)
    readonly property var targetScreen: {
        for (const candidate of Application.screens) {
            if (candidate.virtualX === monitorGeometry.x && candidate.virtualY === monitorGeometry.y)
                return candidate
        }
        return null
    }

    objectName: "quickOverlayRegionSelection"
    title: qsTr("Select a region")
    transientParent: null
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"

    // Win32 monitor sizes are physical pixels; Qt window sizes are logical pixels.
    screen: targetScreen
    x: targetScreen ? targetScreen.virtualX : 0
    y: targetScreen ? targetScreen.virtualY : 0
    width: targetScreen ? targetScreen.width : 0
    height: targetScreen ? targetScreen.height : 0
    visible: recordViewModel.regionSelectionNeeded && targetScreen !== null
             && monitorGeometry.width > 0 && monitorGeometry.height > 0

    onVisibleChanged: {
        if (visible)
            requestActivate()
    }

    onClosing: close => {
        close.accepted = false
        recordViewModel.requestSelectTarget(recordViewModel.selectedTargetIndex, 0)
    }

    CaptureExclusion {
        target: root
    }

    RegionSelectionOverlay {
        recordViewModel: root.recordViewModel
        sourcePixelSize: Qt.size(root.monitorGeometry.width, root.monitorGeometry.height)
        anchors.fill: parent
    }
}
