import QtQuick

// Harness-only stand-in for the live preview (--visual-test): colour bars, a
// grey ramp, a grid and a centre mark. Deterministic, so two captures of the
// same state compare equal, and free of anything on the developer's screen.
// The label is a test-pattern marking, not product copy, and is not translated.
Canvas {
    id: root

    property real radius: 0
    property string fontFamily: ""

    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onVisibleChanged: if (visible) requestPaint()

    onPaint: {
        const ctx = getContext("2d")
        const w = width
        const h = height
        ctx.reset()
        if (w <= 0 || h <= 0)
            return

        ctx.beginPath()
        ctx.roundedRect(0, 0, w, h, radius, radius)
        ctx.clip()

        ctx.fillStyle = "#16161A"
        ctx.fillRect(0, 0, w, h)

        const bars = ["#C0C0C0", "#C0C000", "#00C0C0", "#00C000", "#C000C0", "#C00000", "#0000C0"]
        const barsHeight = Math.round(h * 0.62)
        for (let i = 0; i < bars.length; ++i) {
            const x0 = Math.round(i * w / bars.length)
            const x1 = Math.round((i + 1) * w / bars.length)
            ctx.fillStyle = bars[i]
            ctx.fillRect(x0, 0, x1 - x0, barsHeight)
        }

        const steps = 8
        const rampTop = barsHeight
        const rampHeight = Math.round(h * 0.18)
        for (let s = 0; s < steps; ++s) {
            const x0 = Math.round(s * w / steps)
            const x1 = Math.round((s + 1) * w / steps)
            const level = Math.round(255 * s / (steps - 1))
            ctx.fillStyle = Qt.rgba(level / 255, level / 255, level / 255, 1)
            ctx.fillRect(x0, rampTop, x1 - x0, rampHeight)
        }

        ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.18)
        ctx.lineWidth = 1
        const columns = 16
        const rows = 9
        ctx.beginPath()
        for (let c = 1; c < columns; ++c) {
            const x = Math.round(c * w / columns) + 0.5
            ctx.moveTo(x, 0)
            ctx.lineTo(x, h)
        }
        for (let r = 1; r < rows; ++r) {
            const y = Math.round(r * h / rows) + 0.5
            ctx.moveTo(0, y)
            ctx.lineTo(w, y)
        }
        ctx.stroke()

        const cx = w / 2
        const cy = h / 2
        const ring = Math.min(w, h) * 0.32
        ctx.strokeStyle = "#FFFFFF"
        ctx.lineWidth = 2
        ctx.beginPath()
        ctx.arc(cx, cy, ring, 0, Math.PI * 2)
        ctx.moveTo(cx - ring * 0.2, cy)
        ctx.lineTo(cx + ring * 0.2, cy)
        ctx.moveTo(cx, cy - ring * 0.2)
        ctx.lineTo(cx, cy + ring * 0.2)
        ctx.stroke()

        const labelHeight = Math.max(28, Math.round(h * 0.09))
        const labelWidth = Math.min(w * 0.6, labelHeight * 7)
        ctx.fillStyle = "#101014"
        ctx.fillRect(cx - labelWidth / 2, cy + ring * 0.35, labelWidth, labelHeight)
        ctx.fillStyle = "#FFFFFF"
        ctx.font = "600 " + Math.round(labelHeight * 0.5) + "px \"" + fontFamily + "\""
        ctx.textAlign = "center"
        ctx.textBaseline = "middle"
        ctx.fillText("TEST CARD 1920x1080", cx, cy + ring * 0.35 + labelHeight / 2)
    }
}
