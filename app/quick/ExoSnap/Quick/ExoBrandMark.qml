import QtQuick
import QtQuick.Shapes

// The ExoSnap mark: an aperture, drawn rather than loaded.
//
// `qrc:/brand/exosnap-logo.svg` hardcodes #9BD9D2, which was written for a dark
// band. Put in the title bar it appears on every page of every theme, and on
// `light-paper` a pale mint mark on an off-white band is very nearly invisible.
// The geometry here is the SVG's, coordinate for coordinate (a 32-unit box: a
// 14.5 ring at 45 % opacity, a 6.2 ring, a 2.4 dot) — only the colour changes,
// from one fixed value to the theme's accent. Same mark, legible in all four.
//
// The asset stays: it is still the source the .ico window-icon variants are
// generated from.
Item {
    id: root

    property color color: ExoTheme.accent

    // Authored on the SVG's 32-unit grid and scaled, so the ring weights stay in
    // proportion at any size the caller asks for.
    readonly property real unit: Math.min(root.width, root.height) / 32

    implicitWidth: 18
    implicitHeight: 18

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: Qt.alpha(root.color, 0.45)
            strokeWidth: 1.5 * root.unit
            fillColor: "transparent"

            PathAngleArc {
                centerX: 16 * root.unit
                centerY: 16 * root.unit
                radiusX: 14.5 * root.unit
                radiusY: 14.5 * root.unit
                startAngle: 0
                sweepAngle: 360
            }
        }

        ShapePath {
            strokeColor: root.color
            strokeWidth: 1.6 * root.unit
            fillColor: "transparent"

            PathAngleArc {
                centerX: 16 * root.unit
                centerY: 16 * root.unit
                radiusX: 6.2 * root.unit
                radiusY: 6.2 * root.unit
                startAngle: 0
                sweepAngle: 360
            }
        }

        ShapePath {
            strokeWidth: 0
            fillColor: root.color

            PathAngleArc {
                centerX: 16 * root.unit
                centerY: 16 * root.unit
                radiusX: 2.4 * root.unit
                radiusY: 2.4 * root.unit
                startAngle: 0
                sweepAngle: 360
            }
        }
    }
}
