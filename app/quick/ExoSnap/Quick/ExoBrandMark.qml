import QtQuick
import QtQuick.Shapes

// The ExoSnap mark: an aperture, drawn rather than loaded.
//
// `qrc:/brand/exosnap-logo.svg` carries one fixed accent, which was written for
// a dark band. Put in the title bar the mark appears on every page of every
// theme, and on a light appearance a pale mint mark on an off-white band is very
// nearly invisible. Drawn, it takes the theme's accent instead. Same mark,
// legible in both.
//
// The coordinates are not repeated here: BrandMarkGeometry publishes the
// canonical ones (ui/brand/BrandMark.h), which the runtime shell renderer and
// the build-time icon generator read as well.
Item {
    id: root

    property color color: ExoTheme.accent

    // Authored on the canonical grid and scaled, so the ring weights stay in
    // proportion at any size the caller asks for.
    readonly property real unit: Math.min(root.width, root.height) / BrandMarkGeometry.grid

    implicitWidth: 18
    implicitHeight: 18

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: Qt.alpha(root.color, BrandMarkGeometry.outerOpacity)
            strokeWidth: BrandMarkGeometry.outerStroke * root.unit
            fillColor: "transparent"

            PathAngleArc {
                centerX: BrandMarkGeometry.center * root.unit
                centerY: BrandMarkGeometry.center * root.unit
                radiusX: BrandMarkGeometry.outerRadius * root.unit
                radiusY: BrandMarkGeometry.outerRadius * root.unit
                startAngle: 0
                sweepAngle: 360
            }
        }

        ShapePath {
            strokeColor: root.color
            strokeWidth: BrandMarkGeometry.innerStroke * root.unit
            fillColor: "transparent"

            PathAngleArc {
                centerX: BrandMarkGeometry.center * root.unit
                centerY: BrandMarkGeometry.center * root.unit
                radiusX: BrandMarkGeometry.innerRadius * root.unit
                radiusY: BrandMarkGeometry.innerRadius * root.unit
                startAngle: 0
                sweepAngle: 360
            }
        }

        ShapePath {
            strokeWidth: 0
            fillColor: root.color

            PathAngleArc {
                centerX: BrandMarkGeometry.center * root.unit
                centerY: BrandMarkGeometry.center * root.unit
                radiusX: BrandMarkGeometry.dotRadius * root.unit
                radiusY: BrandMarkGeometry.dotRadius * root.unit
                startAngle: 0
                sweepAngle: 360
            }
        }
    }
}
