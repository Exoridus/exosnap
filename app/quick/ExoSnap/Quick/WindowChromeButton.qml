pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// One of the three window buttons in the frameless title bar.
//
// 46 × 40, filling its whole cell with no corner radius. That is not a style
// choice: a rounded button leaves an unclickable wedge in the very screen corner
// the user throws the pointer at, which is the fastest way to reach Close.
//
// Its own file rather than an inline component because QML only allows inline
// components in a file's root object, and this belongs inside the title bar's
// row.
AbstractButton {
    id: root

    // "minimize" | "maximize" | "restore" | "close"
    property string kind: "minimize"
    // Close only. Hovering it is the one window button that turns a full colour
    // rather than a surface tint.
    property bool danger: false

    implicitWidth: 46
    implicitHeight: 40
    hoverEnabled: true
    Accessible.role: Accessible.Button

    background: Rectangle {
        color: root.danger && root.hovered ? ExoTheme.error
             : root.hovered ? ExoTheme.surfaceHover
             : "transparent"
    }

    contentItem: Canvas {
        id: glyphCanvas

        readonly property color tone: root.danger && root.hovered ? ExoTheme.errorInk : ExoTheme.textSecondary
        readonly property string kind: root.kind

        onToneChanged: requestPaint()
        onKindChanged: requestPaint()

        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.strokeStyle = glyphCanvas.tone;
            ctx.lineWidth = 1;

            // Half-pixel offsets: a 1 px stroke centred on an integer coordinate
            // straddles two device pixels and renders as a 2 px grey smear.
            const cx = Math.round(width / 2) + 0.5;
            const cy = Math.round(height / 2) + 0.5;

            if (glyphCanvas.kind === "minimize") {
                ctx.beginPath();
                ctx.moveTo(cx - 5, cy);
                ctx.lineTo(cx + 5, cy);
                ctx.stroke();
            } else if (glyphCanvas.kind === "maximize") {
                ctx.strokeRect(cx - 5, cy - 5, 10, 10);
            } else if (glyphCanvas.kind === "restore") {
                // Front pane plus the visible corner of the one behind it.
                ctx.strokeRect(cx - 5, cy - 3, 8, 8);
                ctx.beginPath();
                ctx.moveTo(cx - 2, cy - 3);
                ctx.lineTo(cx - 2, cy - 5);
                ctx.lineTo(cx + 5, cy - 5);
                ctx.lineTo(cx + 5, cy + 2);
                ctx.lineTo(cx + 3, cy + 2);
                ctx.stroke();
            } else {
                ctx.beginPath();
                ctx.moveTo(cx - 5, cy - 5);
                ctx.lineTo(cx + 5, cy + 5);
                ctx.moveTo(cx + 5, cy - 5);
                ctx.lineTo(cx - 5, cy + 5);
                ctx.stroke();
            }
        }
    }
}
