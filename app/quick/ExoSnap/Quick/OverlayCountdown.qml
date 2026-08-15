import QtQuick
import QtQuick.Effects
import QtQuick.Shapes

// Pre-roll countdown, centred on the recorded monitor.
// Ported from app/ui/overlay/CountdownOverlayWindow.cpp — capture-excluded and
// click-through.
Window {
    id: root

    // ── Business inputs (the lead wires these to the countdown controller) ────
    property rect monitorGeometry: Qt.rect(0, 0, 0, 0)
    property int remainingSeconds: 0
    property int durationSeconds: 0
    property bool countdownActive: false

    readonly property rect effectiveGeometry: root.monitorGeometry.width > 0 && root.monitorGeometry.height > 0
                                              ? root.monitorGeometry
                                              : Qt.rect(Screen.virtualX, Screen.virtualY, Screen.width, Screen.height)

    // Depleting ring: 1.0 = full, 0.0 = empty.
    readonly property real progress: root.durationSeconds > 0
                                     ? Math.max(0.0, Math.min(1.0, root.remainingSeconds / root.durationSeconds))
                                     : 1.0

    // What the arc actually draws. The controller counts in whole seconds, so
    // binding the arc straight to `progress` made the ring jump once a second
    // in three big steps -- a progress indicator that is only ever right at the
    // instant it moves. Interpolating across exactly one second turns the same
    // source into a continuously depleting ring; the digit keeps showing whole
    // seconds, because that is what it means.
    property real displayedProgress: root.progress

    Behavior on displayedProgress {
        NumberAnimation {
            // Deliberately LONGER than the one-second tick that feeds it. At
            // exactly 1000 ms the interpolation finishes before the next tick
            // arrives — the source is a whole-second counter with ordinary timer
            // jitter, never a metronome — and the ring visibly parks for the
            // remainder. Overlapping the tick means the next value always
            // interrupts a still-moving animation, so the ring never stops
            // before the countdown does. The cost is a few degrees of lag
            // against the digit, which no one can see; a ring that stutters,
            // everyone can.
            duration: 1150
            easing.type: Easing.Linear
        }
    }

    // ── Overlay tokens ────────────────────────────────────────────────────────
    // Verbatim from the Widgets class — the circle sits on captured content, so
    // it carries its own darker surface. The digit and ring use the shared
    // caution/amber tone.
    readonly property color circleBackground: "#BD0C0C0E"  // rgba(12,12,14,0.74)
    readonly property color circleBorder: "#29FFFFFF"      // rgba(255,255,255,0.16)
    readonly property color ringTrack: "#1FFFFFFF"         // rgba(255,255,255,0.12)

    readonly property int circleSize: 124
    readonly property int shadowMargin: 18
    readonly property real ringRadius: 57

    // See OverlayRecording.qml: an inherited transient parent would take the
    // overlay down with the app window.
    transientParent: null

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
           | Qt.WindowDoesNotAcceptFocus | Qt.WindowTransparentForInput
    color: "transparent"

    visible: exclusion.granted && root.countdownActive && root.remainingSeconds > 0

    width: root.circleSize + 2 * root.shadowMargin
    height: root.circleSize + 2 * root.shadowMargin
    x: root.effectiveGeometry.x + (root.effectiveGeometry.width - width) / 2
    y: root.effectiveGeometry.y + (root.effectiveGeometry.height - height) / 2

    CaptureExclusion {
        id: exclusion

        target: root
    }

    Rectangle {
        id: circle

        width: root.circleSize
        height: root.circleSize
        radius: width / 2
        anchors.centerIn: parent
        color: root.circleBackground
        border.width: 1
        border.color: root.circleBorder

        // A real shadow, in one pass. It used to be a second Rectangle behind
        // this one: a hard-edged 50 % black disc 20 px wider than the circle,
        // ported from the Widgets class as a cheap stand-in for a blur. Over
        // arbitrary desktop content that does not read as a shadow at all — it
        // reads as an opaque grey plate the countdown is sitting on, which is
        // exactly what a capture-excluded overlay must not look like.
        //
        // Rendered through the item's own layer, so the effect draws this
        // circle rather than duplicating it, and `autoPaddingEnabled` grows the
        // layer to fit the blur instead of clipping it at the circle's edge.
        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            // Literal rather than a token on the window root: a `layer.effect`
            // component is instantiated in its own scope, so an id from the
            // enclosing file does not resolve inside it.
            shadowColor: "#B3000000" // rgba(0, 0, 0, 0.7)
            shadowVerticalOffset: 4
            shadowBlur: 1.0
            // Small on purpose: the window is only 20 px wider than the circle
            // on each side, and the docs are explicit that a smaller blurMax is
            // the cheapest way to keep a shadow affordable.
            blurMax: 16
        }

        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer

            // Track: the full circle, always present under the depleting arc.
            ShapePath {
                strokeColor: root.ringTrack
                strokeWidth: 3
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap

                PathAngleArc {
                    centerX: circle.width / 2
                    centerY: circle.height / 2
                    radiusX: root.ringRadius
                    radiusY: root.ringRadius
                    startAngle: -90
                    sweepAngle: 360
                    moveToStart: true
                }
            }

            // Progress: starts at 12 o'clock and depletes clockwise. PathAngleArc
            // measures 0 degrees at 3 o'clock with positive angles clockwise —
            // the opposite sign convention to the QPainter original.
            // Accent, not caution amber. A countdown is a normal workflow
            // transition the user asked for — nothing about it needs attention,
            // and amber spent on it is amber a real warning cannot claim back.
            // Safe on this ground in both appearances: the contrast gate asserts
            // the accent as an indicator on the overlays' near-black pill.
            ShapePath {
                strokeColor: ExoTheme.accent
                strokeWidth: 3
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap

                PathAngleArc {
                    centerX: circle.width / 2
                    centerY: circle.height / 2
                    radiusX: root.ringRadius
                    radiusY: root.ringRadius
                    startAngle: -90
                    sweepAngle: 360 * root.displayedProgress
                    moveToStart: true
                }
            }
        }

        Text {
            anchors.centerIn: parent
            text: String(Math.max(1, root.remainingSeconds))
            textFormat: Text.PlainText
            color: ExoTheme.accent
            font {
                family: ExoTheme.monoFamily
                pixelSize: 52
                weight: Font.Medium
            }
        }
    }
}
