import QtQuick
import QtQuick.Shapes
import ExoSnap.Updater

// The updater's own glyphs, drawn as shapes over the shared theme colours.
// Check / cross / warning / dot / layers come from the application's ExoGlyph
// set where one exists; the three below are updater-only roles and stay local so
// the app's icon set does not grow marks only this window uses.
Item {
    id: root

    // "download" | "shield" | "spinner"
    required property string glyph
    property color color: ExoTheme.text

    implicitWidth: 18
    implicitHeight: 18

    Shape {
        anchors.fill: parent
        visible: root.glyph === "download"
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: root.color
            strokeWidth: 1.6
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            startX: root.width / 2
            startY: root.height * 0.12
            PathLine { x: root.width / 2; y: root.height * 0.62 }
            PathLine { x: root.width * 0.26; y: root.height * 0.40 }
            PathMove { x: root.width * 0.26; y: root.height * 0.40 }
            PathLine { x: root.width / 2; y: root.height * 0.62 }
            PathLine { x: root.width * 0.74; y: root.height * 0.40 }
            PathMove { x: root.width * 0.14; y: root.height * 0.82 }
            PathLine { x: root.width * 0.86; y: root.height * 0.82 }
        }
    }

    Shape {
        anchors.fill: parent
        visible: root.glyph === "shield"
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: root.color
            strokeWidth: 1.4
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            startX: root.width / 2
            startY: root.height * 0.08
            PathLine { x: root.width * 0.82; y: root.height * 0.26 }
            PathLine { x: root.width * 0.82; y: root.height * 0.52 }
            PathQuad {
                x: root.width / 2
                y: root.height * 0.94
                controlX: root.width * 0.74
                controlY: root.height * 0.80
            }
            PathQuad {
                x: root.width * 0.18
                y: root.height * 0.52
                controlX: root.width * 0.26
                controlY: root.height * 0.80
            }
            PathLine { x: root.width * 0.18; y: root.height * 0.26 }
            PathLine { x: root.width / 2; y: root.height * 0.08 }
        }
    }

    Shape {
        anchors.fill: parent
        visible: root.glyph === "spinner"
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: ExoTheme.line
            strokeWidth: 2
            fillColor: "transparent"
            PathAngleArc {
                centerX: root.width / 2
                centerY: root.height / 2
                radiusX: root.width / 2 - 1
                radiusY: root.height / 2 - 1
                startAngle: 0
                sweepAngle: 360
            }
        }

        ShapePath {
            strokeColor: root.color
            strokeWidth: 2
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: root.width / 2
                centerY: root.height / 2
                radiusX: root.width / 2 - 1
                radiusY: root.height / 2 - 1
                startAngle: 0
                sweepAngle: 100
            }
        }

        RotationAnimator on rotation {
            running: root.visible
            loops: Animation.Infinite
            from: 0
            to: 360
            duration: 900
        }
        transformOrigin: Item.Center
    }
}
