pragma ComponentBehavior: Bound

import QtQuick

// Draggable quick-control pill: pause/resume, stop, capture frame.
// Ported from app/ui/overlay/QuickControlPillWindow.cpp.
//
// This is the one capture-excluded overlay that is NOT click-through: it is
// interactive by design (ADR 0016), so it deliberately omits
// Qt.WindowTransparentForInput. Capture exclusion still applies unchanged — the
// controls must not burn into the recording either.
Window {
    id: root

    // ── Business inputs ──────────────────────────────────────────────────────
    // Single gate, resolved in C++ (OverlayAdapter::quickControlsActive) from
    // the "Show quick controls" setting AND the live capture state. The port
    // carried these as two separate properties because the Widgets class had two
    // setters; keeping them apart here would put the AND in the delegate.
    property bool overlayActive: false
    property bool paused: false
    property bool expanded: true

    // The RECORDED monitor, like the other four overlays — resolved in C++ from
    // the live capture target (OverlayAdapter::recordedMonitorGeometry), never
    // from QML screen enumeration.
    //
    // The Widgets class put this pill on the primary display because it had no
    // setMonitorGeometry() at all; the port carried that forward as an open
    // product question. It is settled now: controls belong on the screen the user
    // is looking at, which during a capture is the one being captured. The pill
    // is capture-excluded, so putting it there costs the recording nothing.
    property rect monitorGeometry: Qt.rect(0, 0, 0, 0)

    readonly property rect effectiveGeometry: root.monitorGeometry.width > 0
                                              && root.monitorGeometry.height > 0
                                              ? root.monitorGeometry
                                              : Qt.rect(Screen.virtualX, Screen.virtualY, Screen.width, Screen.height)

    // ── Overlay tokens (Widgets class, verbatim) ─────────────────────────────
    readonly property color pillBackground: "#CC0C0C0E"  // rgba(12,12,14,0.8)
    readonly property color pillBorder: "#29FFFFFF"      // rgba(255,255,255,0.16)
    readonly property color gripTone: "#80FFFFFF"        // rgba(255,255,255,0.5)
    readonly property color buttonBackground: "#0FFFFFFF"  // rgba(255,255,255,0.06)
    readonly property color buttonBorder: "#1AFFFFFF"      // rgba(255,255,255,0.1)
    readonly property color buttonGlyph: "#E6FFFFFF"       // rgba(255,255,255,0.9)
    // Stop is rec-styled: the coral tone, tinted for fill and border.
    readonly property color stopBackground: Qt.alpha(ExoTheme.error, 0.18)
    readonly property color stopBorder: Qt.alpha(ExoTheme.error, 0.5)

    readonly property int pad: 8
    readonly property int gripWidth: 28
    readonly property int buttonSize: 44
    readonly property int buttonGap: 8

    signal pauseResumeRequested()
    signal stopRequested()
    signal captureFrameRequested()

    // See OverlayRecording.qml: an inherited transient parent would take the
    // pill down with the app window — and the pill exists precisely for sessions
    // where the app window is out of the way.
    transientParent: null

    // No Qt.WindowTransparentForInput here: this window takes mouse input.
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus

    color: "transparent"

    visible: exclusion.granted && root.overlayActive

    width: root.pad + root.gripWidth + root.pad
           + (root.expanded ? root.buttonGap + 3 * root.buttonSize + 2 * root.buttonGap : 0)
    height: root.pad + root.buttonSize + root.pad

    // Bottom-centre by default. Dragging the grip assigns x/y directly, which
    // replaces these bindings — intentional: once the user has placed the pill,
    // it stays where they put it.
    x: root.effectiveGeometry.x + (root.effectiveGeometry.width - width) / 2
    y: root.effectiveGeometry.y + root.effectiveGeometry.height - height - 32

    CaptureExclusion {
        id: exclusion

        target: root
    }

    // All pill glyphs are vector-drawn, matching the Widgets original: they are
    // sized to the 18 px nominal glyph box rather than a font's cap height.
    component PillGlyph: Canvas {
        id: glyph

        // "pause" | "resume" | "stop" | "camera" | "grip"
        property string kind: "pause"
        property color tone: root.buttonGlyph

        width: 18
        height: 18
        onKindChanged: requestPaint()
        onToneChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = glyph.tone
            ctx.fillStyle = glyph.tone
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.lineWidth = 1.6

            const cx = width / 2
            const cy = height / 2
            const s = 9  // half of the 18 px nominal glyph box

            if (glyph.kind === "pause") {
                const bw = s * 0.30
                const bh = s * 0.80
                const gap = s * 0.22
                ctx.beginPath()
                ctx.roundedRect(cx - gap / 2 - bw, cy - bh, bw, bh * 2, 1.5, 1.5)
                ctx.fill()
                ctx.beginPath()
                ctx.roundedRect(cx + gap / 2, cy - bh, bw, bh * 2, 1.5, 1.5)
                ctx.fill()
            } else if (glyph.kind === "resume") {
                ctx.beginPath()
                ctx.moveTo(cx - s * 0.3, cy - s * 0.6)
                ctx.lineTo(cx + s * 0.6, cy)
                ctx.lineTo(cx - s * 0.3, cy + s * 0.6)
                ctx.closePath()
                ctx.fill()
            } else if (glyph.kind === "stop") {
                const side = s * 0.85
                ctx.beginPath()
                ctx.roundedRect(cx - side / 2, cy - side / 2, side, side, 3, 3)
                ctx.fill()
            } else if (glyph.kind === "camera") {
                ctx.beginPath()
                ctx.roundedRect(cx - s * 0.65, cy - s * 0.4, s * 1.3, s, 3, 3)
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(cx, cy + s * 0.1, s * 0.35, 0, 2 * Math.PI, false)
                ctx.stroke()
                ctx.beginPath()
                ctx.roundedRect(cx - s * 0.25, cy - s * 0.53, s * 0.5, s * 0.25, 2, 2)
                ctx.stroke()
            } else {
                // Grip: three short horizontal lines.
                ctx.lineWidth = 1.8
                for (let i = -1; i <= 1; ++i) {
                    ctx.beginPath()
                    ctx.moveTo(cx - s * 0.5, cy + i * s * 0.45)
                    ctx.lineTo(cx + s * 0.5, cy + i * s * 0.45)
                    ctx.stroke()
                }
            }
        }
    }

    component PillButton: Rectangle {
        id: button

        property string glyphKind: "pause"
        property bool recStyled: false

        signal activated()

        width: root.buttonSize
        height: root.buttonSize
        radius: 12
        color: button.recStyled ? root.stopBackground : root.buttonBackground
        border.width: 1
        border.color: button.recStyled ? root.stopBorder : root.buttonBorder

        Accessible.role: Accessible.Button
        Accessible.onPressAction: button.activated()

        PillGlyph {
            anchors.centerIn: parent
            kind: button.glyphKind
            tone: button.recStyled ? ExoTheme.error : root.buttonGlyph
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: button.activated()
        }
    }

    Rectangle {
        anchors.fill: parent
        color: root.pillBackground
        border.width: 1
        border.color: root.pillBorder
        radius: 16

        Item {
            id: grip

            x: root.pad
            width: root.gripWidth
            height: parent.height

            PillGlyph {
                anchors.centerIn: parent
                kind: "grip"
                tone: root.gripTone
            }

            MouseArea {
                id: gripArea

                // The grab point in virtual-desktop coordinates. Tracking it
                // globally keeps the delta correct while the window itself moves
                // underneath the cursor.
                property point pressGlobal: Qt.point(0, 0)
                // Accumulated travel — the released-position delta converges back
                // to zero once the window has caught up with the cursor, so it
                // cannot tell a drag from a click on its own.
                property real travelled: 0
                property bool dragging: false

                anchors.fill: parent
                cursorShape: gripArea.dragging ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                acceptedButtons: Qt.LeftButton
                onPressed: mouse => {
                    gripArea.pressGlobal = Qt.point(mouse.x + root.x + grip.x, mouse.y + root.y + grip.y)
                    gripArea.travelled = 0
                    gripArea.dragging = true
                }
                onPositionChanged: mouse => {
                    if (!gripArea.dragging)
                        return
                    const dx = mouse.x + root.x + grip.x - gripArea.pressGlobal.x
                    const dy = mouse.y + root.y + grip.y - gripArea.pressGlobal.y
                    gripArea.travelled += Math.abs(dx) + Math.abs(dy)
                    root.x += dx
                    root.y += dy
                }
                onReleased: {
                    gripArea.dragging = false
                    // A press that never really moved is a click on the grip:
                    // collapse or expand instead of nudging the pill by a pixel.
                    if (gripArea.travelled <= 4)
                        root.expanded = !root.expanded
                }
            }
        }

        Row {
            anchors {
                left: grip.right
                leftMargin: root.buttonGap
                verticalCenter: parent.verticalCenter
            }
            spacing: root.buttonGap
            visible: root.expanded

            PillButton {
                glyphKind: root.paused ? "resume" : "pause"
                Accessible.name: root.paused ? qsTr("Resume recording") : qsTr("Pause recording")
                onActivated: root.pauseResumeRequested()
            }

            PillButton {
                glyphKind: "stop"
                recStyled: true
                Accessible.name: qsTr("Stop recording")
                onActivated: root.stopRequested()
            }

            PillButton {
                glyphKind: "camera"
                Accessible.name: qsTr("Capture frame")
                onActivated: root.captureFrameRequested()
            }
        }
    }
}
