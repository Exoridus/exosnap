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
    // The bottom of the box. Zero for a series that never goes negative, which
    // is what every rate plots; a signed series (A/V drift) needs its own floor,
    // or every negative sample maps far below the tile and the line vanishes.
    readonly property real _floor: {
        let low = 0;
        for (let i = 0; i < root.values.length; ++i)
            low = Math.min(low, root.values[i]);
        return low;
    }
    readonly property real _peak: {
        let peak = root.hasBudget ? root.budget : 0;
        for (let i = 0; i < root.values.length; ++i)
            peak = Math.max(peak, root.values[i]);
        return peak;
    }
    // 10% headroom so the highest point never touches the top edge, and a floor
    // so an all-zero series does not divide by zero.
    readonly property real _span: Math.max(1e-6, (root._peak - root._floor) * 1.1)

    // Where a value sits in the box, top-down. Read by tst_ExoSparkline, which
    // pins the mapping rather than the point count.
    function valueY(value: real): real {
        return root.height - ((value - root._floor) / root._span) * root.height;
    }

    readonly property var _points: {
        const points = [];
        const count = root.values.length;
        for (let i = 0; i < count; ++i) {
            const x = count > 1 ? (root.width * i) / (count - 1) : root.width;
            points.push(Qt.point(x, root.valueY(root.values[i])));
        }
        return points;
    }

    readonly property real _budgetY: root.valueY(root.budget)

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
