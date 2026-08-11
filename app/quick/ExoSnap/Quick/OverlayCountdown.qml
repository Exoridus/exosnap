import QtQuick
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

    // ── Overlay tokens ────────────────────────────────────────────────────────
    // Verbatim from the Widgets class — the circle sits on captured content, so
    // it carries its own darker surface. The digit and ring use the shared
    // caution/amber tone.
    readonly property color circleBackground: "#BD0C0C0E"  // rgba(12,12,14,0.74)
    readonly property color circleBorder: "#29FFFFFF"      // rgba(255,255,255,0.16)
    readonly property color ringTrack: "#1FFFFFFF"         // rgba(255,255,255,0.12)
    readonly property color shadowTone: "#80000000"        // rgba(0,0,0,0.5)

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

    // Flat dark ellipse instead of a real blur, the same approximation the
    // Widgets class makes: the overlay must stay cheap to composite while the
    // encoder has the GPU.
    Rectangle {
        width: root.circleSize + 20
        height: root.circleSize + 20
        radius: width / 2
        color: root.shadowTone
        anchors {
            centerIn: parent
            verticalCenterOffset: 6
        }
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
            ShapePath {
                strokeColor: ExoTheme.warning
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
            color: ExoTheme.warning
            font {
                family: ExoTheme.monoFamily
                pixelSize: 52
                weight: Font.Medium
            }
        }
    }
}
