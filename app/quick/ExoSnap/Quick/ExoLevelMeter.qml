import QtQuick

// Compact mono level meter. `level` is a pre-computed 0..1 dock level; the
// adapter forwards the same value the Record page shows, so this item performs
// no signal maths of its own.
Item {
    id: root

    required property real level
    property bool active: true

    implicitWidth: 60
    implicitHeight: 6
    Accessible.ignored: true

    Rectangle {
        anchors.fill: parent
        color: ExoTheme.surfaceHover
        radius: height / 2
    }

    Rectangle {
        width: Math.max(0, Math.min(1, root.level)) * parent.width
        height: parent.height
        color: root.active ? ExoTheme.accent : ExoTheme.textDim
        radius: height / 2
    }
}
