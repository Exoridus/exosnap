import QtQuick

// Determinate progress track. `value` is 0.0 .. 1.0 — a fraction, not a percent,
// so no surface has to remember which unit this one expects.
//
// Extracted from the Edit export panel once recovery and the crash report needed
// the same three lines: a rounded track, an accent fill, one height. The fill is
// animated because the sources are sampled (a remux reports every few frames),
// and an unanimated bar reads as stuttering rather than as progressing.
Rectangle {
    id: root

    required property real value

    implicitHeight: 6
    color: ExoTheme.surfaceRaised
    radius: 3

    Rectangle {
        width: parent.width * Math.max(0, Math.min(1, root.value))
        height: parent.height
        radius: parent.radius
        color: ExoTheme.accent

        Behavior on width {
            NumberAnimation {
                duration: 120
                easing.type: Easing.OutQuad
            }
        }
    }
}
