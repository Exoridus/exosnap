import QtQuick
import QtQuick.Controls.Basic

ProgressBar {
    id: root
    property real progress: 0
    property string accessibleDescription
    implicitHeight: 4
    from: 0
    to: 1
    value: Math.max(0, Math.min(1, root.progress))
    Accessible.name: root.accessibleDescription
    background: Rectangle { radius: 2; color: ExoTheme.surfaceRaised }
    contentItem: Item {
        Rectangle {
            width: root.indeterminate ? 0 : parent.width * root.visualPosition
            height: parent.height
            radius: 2
            color: ExoTheme.accent
        }
    }
}
