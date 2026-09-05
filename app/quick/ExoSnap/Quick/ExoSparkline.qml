import QtQuick
import QtQuick.Shapes

// A 24 px trend line under a tile's value: the last N snapshots, no area fill.
// The line's own colour is the caller's job (it follows the value tint), so this
// component draws geometry only.
Item {
    id: root

    property var values: []
    // NaN when the owning check has no threshold - the budget line is omitted
    // rather than drawn at zero, which would read as "the limit is zero".
    property real budget: NaN
    property color lineColor: ExoTheme.textMuted

    implicitWidth: 160
    implicitHeight: 24

    readonly property bool hasBudget: !isNaN(root.budget)
    readonly property real _peak: {
        let peak = root.hasBudget ? root.budget : 0;
        for (let i = 0; i < root.values.length; ++i)
            peak = Math.max(peak, root.values[i]);
        // 10% headroom so the highest point never touches the top edge, and a
        // floor so an all-zero series does not divide by zero.
        return Math.max(1e-6, peak * 1.1);
    }

    readonly property var _points: {
        const points = [];
        const count = root.values.length;
        for (let i = 0; i < count; ++i) {
            const x = count > 1 ? (root.width * i) / (count - 1) : root.width;
            const y = root.height - (root.values[i] / root._peak) * root.height;
            points.push(Qt.point(x, y));
        }
        return points;
    }

    readonly property real _budgetY: root.height - (root.budget / root._peak) * root.height

    // Read by tst_ExoSparkline to prove one polyline segment is emitted per
    // value without depending on Shape internals.
    readonly property int pointCount: root._points.length

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeWidth: root.hasBudget ? 1 : 0
            strokeColor: root.hasBudget ? ExoTheme.warning : "transparent"
            strokeStyle: ShapePath.DashLine
            dashPattern: [3, 3]
            fillColor: "transparent"
            startX: 0
            startY: root.hasBudget ? root._budgetY : 0

            PathLine {
                x: root.width
                y: root.hasBudget ? root._budgetY : 0
            }
        }

        ShapePath {
            strokeWidth: root.values.length > 0 ? 1.2 : 0
            strokeColor: root.values.length > 0 ? root.lineColor : "transparent"
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            PathPolyline {
                path: root._points
            }
        }
    }
}
