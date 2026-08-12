import QtQuick

// The one chevron in the design system. Selection controls, disclosures and
// popovers all point at the same glyph, so "open downwards" reads the same
// everywhere.
//
// Drawn rather than typeset: the wordmark font has no chevron, and the fallback
// glyph (▾) renders at a different weight and baseline on every machine. Canvas
// is the technique the window-chrome buttons and the overlays already use.
Canvas {
    id: root

    // 0 = down, 90 = left, 180 = up, 270 = right. Rotating the item rather than
    // redrawing keeps a spinning disclosure arrow free of a repaint per frame.
    property int direction: 0
    property color tone: ExoTheme.textMuted
    property real thickness: 1.6

    implicitWidth: 12
    implicitHeight: 12
    rotation: root.direction
    antialiasing: true

    onToneChanged: root.requestPaint()
    onThicknessChanged: root.requestPaint()

    onPaint: {
        const ctx = root.getContext("2d");
        ctx.reset();
        ctx.strokeStyle = root.tone;
        ctx.lineWidth = root.thickness;
        ctx.lineCap = "round";
        ctx.lineJoin = "round";

        // A 12-unit box with the vertex low enough that the glyph reads as a
        // direction and not as a wide "v" of text.
        const w = root.width;
        const h = root.height;
        const halfSpan = w * 0.29;
        const top = h * 0.41;
        const bottom = h * 0.62;

        ctx.beginPath();
        ctx.moveTo(w / 2 - halfSpan, top);
        ctx.lineTo(w / 2, bottom);
        ctx.lineTo(w / 2 + halfSpan, top);
        ctx.stroke();
    }
}
