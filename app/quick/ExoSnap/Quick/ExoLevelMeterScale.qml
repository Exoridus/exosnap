pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// The dBFS ruler that belongs under a full-width level meter. Without it a bar
// at two thirds is a proportion of nothing: the reading and the number beside it
// are the same statement, and only the ruler ties them together.
//
// The marks sit at the position the meter's own scale gives them, so a tick and
// the segment it labels can never drift apart.
Item {
    id: root

    // Meter floor, matching models::kMeterFloorDb. Positions are (db - floor) / -floor.
    readonly property real floorDb: -60
    readonly property var marks: [-60, -40, -20, -6, 0]
    property bool active: true

    // Tick (3 px) plus the label's own line height below it. Sized from the
    // label so a font substitution cannot clip the numbers.
    implicitHeight: 4 + sizer.height
    Accessible.ignored: true

    // Measures the tallest label the ruler can draw without adding one to the
    // scene: the marks are positioned, so they cannot size their own parent.
    TextMetrics {
        id: sizer

        text: "-60"
        font {
            family: ExoTheme.monoFamily
            pixelSize: 9
        }
    }

    function _position(db: real): real {
        return db > root.floorDb ? Math.min(1, (db - root.floorDb) / -root.floorDb) : 0;
    }

    Repeater {
        model: root.marks

        Item {
            id: mark

            required property int index
            required property real modelData

            readonly property real position: root._position(mark.modelData)
            // The end labels are pinned inside the meter instead of being centred
            // on their tick: centred, "-60" and "0" would hang half a label past
            // the card's own padding.
            readonly property bool atStart: mark.index === 0
            readonly property bool atEnd: mark.index === root.marks.length - 1

            x: mark.atStart ? 0
             : mark.atEnd ? root.width - mark.width
             : mark.position * root.width - mark.width / 2
            width: label.implicitWidth
            height: root.height

            Rectangle {
                x: mark.atStart ? 0 : mark.atEnd ? mark.width - 1 : mark.width / 2
                width: 1
                height: 3
                color: ExoTheme.lineStrong
            }

            Label {
                id: label

                y: 4
                text: mark.modelData === 0 ? "0" : String(mark.modelData)
                textFormat: Text.PlainText
                color: root.active ? ExoTheme.textDim : ExoTheme.line
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: 9
                }
            }
        }
    }
}
