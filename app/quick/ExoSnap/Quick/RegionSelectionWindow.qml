import QtQuick

Window {
    id: root

    required property RecordViewModelAdapter recordViewModel
    property rect monitorGeometry: Qt.rect(0, 0, 0, 0)
    property size previewSize: Qt.size(0, 0)
    readonly property var targetScreen: {
        if (exclusion.renderOnlyPreview)
            return Application.screens.length > 0 ? Application.screens[0] : null
        for (const candidate of Application.screens) {
            if (candidate.virtualX === monitorGeometry.x && candidate.virtualY === monitorGeometry.y)
                return candidate
        }
        return null
    }
    readonly property bool compositionRequested: recordViewModel.regionSelectionNeeded && targetScreen !== null
                                                && (exclusion.renderOnlyPreview
                                                    ? previewSize.width > 0 && previewSize.height > 0
                                                    : monitorGeometry.width > 0 && monitorGeometry.height > 0)

    function prepareVisualSelection(selection: rect): bool {
        if (!exclusion.renderOnlyPreview || !root.compositionRequested)
            return false
        if (!Number.isFinite(selection.x) || !Number.isFinite(selection.y)
                || !Number.isFinite(selection.width) || !Number.isFinite(selection.height)
                || selection.x < 0 || selection.y < 0 || selection.width < 0 || selection.height < 0
                || selection.x + selection.width > 1 || selection.y + selection.height > 1)
            return false
        selectionSurface.selectionNormalized = selection
        const applied = selectionSurface.selectionNormalized
        return applied.x === selection.x && applied.y === selection.y
                && applied.width === selection.width && applied.height === selection.height
    }

    objectName: "quickOverlayRegionSelection"
    title: qsTr("Select a region")
    transientParent: null
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"

    // Win32 monitor sizes are physical pixels; Qt window sizes are logical pixels.
    screen: targetScreen
    x: exclusion.renderOnlyPreview ? 0 : targetScreen ? targetScreen.virtualX : 0
    y: exclusion.renderOnlyPreview ? 0 : targetScreen ? targetScreen.virtualY : 0
    width: exclusion.renderOnlyPreview ? previewSize.width : targetScreen ? targetScreen.width : 0
    height: exclusion.renderOnlyPreview ? previewSize.height : targetScreen ? targetScreen.height : 0
    visible: (exclusion.granted || exclusion.renderOnlyPreview) && root.compositionRequested

    onVisibleChanged: {
        if (visible && !exclusion.renderOnlyPreview)
            requestActivate()
    }

    onClosing: close => {
        close.accepted = false
        if (!exclusion.renderOnlyPreview)
            recordViewModel.requestSelectTarget(recordViewModel.selectedTargetIndex, 0)
    }

    CaptureExclusion {
        id: exclusion
        target: root
    }

    RegionSelectionOverlay {
        id: selectionSurface
        recordViewModel: root.recordViewModel
        sourcePixelSize: exclusion.renderOnlyPreview ? root.previewSize
                                                   : Qt.size(root.monitorGeometry.width, root.monitorGeometry.height)
        anchors.fill: parent
    }
}
