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

    // Depleting ring: 1.0 = full, 0.0 = empty. Fed by the coordinator's
    // millisecond clock at its own 100 ms tick, NOT derived from the digit.
    //
    // Both earlier versions failed on this. Reading `remainingSeconds`
    // directly made the ring jump in three whole steps. Interpolating between
    // those steps then traded that stutter for a lag: an animation shorter than
    // the tick finished early and parked, one longer than the tick was cut off
    // when the overlay closed and left a visible arc standing at the moment the
    // recording started. The unrounded value has none of that -- it is simply
    // where the countdown actually is -- and needs no animation at all.
    //
    // The fallback matters: `durationSeconds` is 0 before the first tick lands.
    readonly property real progress: root.durationSeconds > 0 ? root.countdownProgress : 1.0

    // 1.0 down to 0.0, from RecordViewModelAdapter.countdownProgress.
    property real countdownProgress: 1.0

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

    // Named rather than left to Qt's default: an untitled QWindow inherits the
    // application display name, and five overlays titled "ExoSnap" made the main
    // window impossible to identify by owner pid and title. See OverlayRecording.
    title: qsTr("ExoSnap Overlay — Countdown")
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
            // `overlayAccent`, not `accent`: the circle is near-black in both
            // appearances, and the LIGHT resolution of an accent measures
            // 3.2-3.9:1 on it. A fixed-dark surface takes the dark resolution of
            // whichever accent the user chose.
            ShapePath {
                strokeColor: ExoTheme.overlayAccent
                strokeWidth: 3
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap

                PathAngleArc {
                    centerX: circle.width / 2
                    centerY: circle.height / 2
                    radiusX: root.ringRadius
                    radiusY: root.ringRadius
                    startAngle: -90
                    sweepAngle: 360 * root.progress
                    moveToStart: true
                }
            }
        }

        Text {
            anchors.centerIn: parent
            text: String(Math.max(1, root.remainingSeconds))
            textFormat: Text.PlainText
            color: ExoTheme.overlayAccent
            font {
                family: ExoTheme.monoFamily
                pixelSize: 52
                weight: Font.Medium
            }
        }
    }
}
