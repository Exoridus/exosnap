import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import ExoSnap.Updater

// The 120 px circular progress indicator. While a run is in flight it draws a
// tinted arc over a faint track with the whole percent in the centre; on a
// terminal result it drops the number for a tinted glyph. Before anything has
// been measured it is indeterminate: track and a turning arc, no number --
// a "0 percent" would be a value nothing has produced.
Item {
    id: ring

    required property real value
    required property bool indeterminate
    required property string tone
    required property string glyph
    required property int percent
    required property string description

    implicitWidth: 120
    implicitHeight: 120

    readonly property color toneColor: {
        switch (ring.tone) {
        case "success":
            return ExoTheme.success;
        case "warning":
            return ExoTheme.warning;
        case "error":
            return ExoTheme.error;
        case "neutral":
            return ExoTheme.textMuted;
        default:
            return ExoTheme.accent;
        }
    }

    Accessible.role: Accessible.ProgressBar
    Accessible.name: ring.description

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: ExoTheme.surfaceRaised
            strokeWidth: 8
            fillColor: "transparent"
            PathAngleArc {
                centerX: ring.width / 2
                centerY: ring.height / 2
                radiusX: ring.width / 2 - 6
                radiusY: ring.height / 2 - 6
                startAngle: 0
                sweepAngle: 360
            }
        }

        ShapePath {
            strokeColor: ring.toneColor
            strokeWidth: 8
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: ring.width / 2
                centerY: ring.height / 2
                radiusX: ring.width / 2 - 6
                radiusY: ring.height / 2 - 6
                startAngle: -90
                // Zero sweep draws nothing, which is how the arc hides while the
                // ring is indeterminate or showing a terminal glyph.
                sweepAngle: !ring.indeterminate && ring.glyph === "" && ring.value > 0
                            ? Math.max(0, Math.min(360, ring.value * 360))
                            : 0
            }
        }
    }

    UpdaterGlyph {
        anchors.centerIn: parent
        width: 108
        height: 108
        visible: ring.indeterminate
        glyph: "spinner"
        color: ring.toneColor
    }

    ExoGlyph {
        anchors.centerIn: parent
        width: 34
        height: 34
        visible: ring.glyph !== ""
        kind: ring.glyph === "check" ? ExoGlyph.Check
              : ring.glyph === "warning" ? ExoGlyph.Warning
              : ring.glyph === "cross" ? ExoGlyph.Close
              : ExoGlyph.Invalid
        color: ring.toneColor
    }

    Column {
        anchors.centerIn: parent
        visible: !ring.indeterminate && ring.glyph === ""
        spacing: 0

        Label {
            objectName: "updaterRingPercent"
            anchors.horizontalCenter: parent.horizontalCenter
            text: ring.percent
            textFormat: Text.PlainText
            color: ExoTheme.text
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontValueLarge
                weight: Font.DemiBold
            }
        }

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("percent")
            textFormat: Text.PlainText
            color: ExoTheme.textMuted
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
            }
        }
    }
}
