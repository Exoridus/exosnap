import QtQuick

// Compact mono level meter. `level` is a pre-computed 0..1 dock level; the
// adapter forwards the same value the Record page shows, so this item performs
// no signal maths of its own.
Item {
    id: root

    required property real level
    property bool active: true

    // A floor, not a size: the hosting row hands this the width its siblings do
    // not claim. At the old fixed 60 px the bar read as a disabled decoration
    // rather than as a meter, and it left a dead gap in the middle of the row.
    implicitWidth: 80
    implicitHeight: 8
    Accessible.ignored: true

    // The empty track itself says whether the source is live: at silence an
    // active meter and an inactive one would otherwise be the same grey bar, and
    // "on but quiet" is not the same statement as "off". A minimum fill sliver
    // would have said it too, and would have been a lie about the signal.
    Rectangle {
        anchors.fill: parent
        color: root.active ? ExoTheme.surfaceHover : ExoTheme.surface
        border.width: 1
        // Both states keep an edge. Dropping the border for the inactive one made
        // the track the same colour as the card behind it, so a disabled source
        // had no meter at all rather than a quiet one.
        border.color: root.active ? ExoTheme.lineStrong : ExoTheme.line
        radius: height / 2
    }

    Rectangle {
        width: Math.max(0, Math.min(1, root.level)) * parent.width
        height: parent.height
        color: root.active ? ExoTheme.accent : ExoTheme.textDim
        radius: height / 2
    }
}
