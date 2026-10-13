import QtQuick

Item {
    id: root

    required property RecordingWorkspaceState workspaceState
    default property alias content: previewViewport.data
    readonly property rect previewBounds: Qt.rect(previewViewport.x, previewViewport.y,
                                                  previewViewport.width, previewViewport.height)

    objectName: "recordingWorkspaceSurface"
    focus: true
    Keys.onEscapePressed: event => {
        event.accepted = root.workspaceState.closeTopPanel();
    }

    Binding { target: root.workspaceState; property: "workspaceWidth"; value: root.width }
    Binding { target: root.workspaceState; property: "workspaceHeight"; value: root.height }

    Item {
        id: previewViewport
        objectName: "recordingWorkspacePreview"
        anchors.fill: parent
        anchors.leftMargin: root.workspaceState.sourceState === "Pinned" ? root.workspaceState.sourceWidth : 0
        anchors.rightMargin: root.workspaceState.profileState === "Pinned" ? root.workspaceState.profileWidth : 0
    }
}
