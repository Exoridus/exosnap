import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// QCR-709. The capability matrix row is a one-line row by contract: label on the
// left, evidence on the right. The value label used to declare `wrapMode` and no
// width policy at all — no fillWidth, no maximum — so it was never handed a width
// to wrap inside and a too-long value simply overran the card. Today's longest
// string fits at the 860 px minimum window, so nothing was visibly wrong; a
// longer string or a localization is what this asserts against. Geometry, so it
// is asserted rather than judged on a screenshot.
Item {
    id: root

    width: 400
    height: 200

    Component {
        id: rowComponent

        DeviceCapabilityRow {
            width: 300
            label: qsTr("Rate control")
            chips: []
            firstRow: true
        }
    }

    TestCase {
        name: "DeviceCapabilityRowTests"
        when: windowShown

        function test_valueThatFitsIsNeitherElidedNorSqueezed() {
            let row = createTemporaryObject(rowComponent, root, { valueText: "CQ · VBR" })
            verify(!!row, "Component exists")
            waitForRendering(root)

            let value = findChild(row, "capabilityRowValue")
            verify(!!value, "value label exists")
            compare(value.truncated, false, "a value with room to spare must render in full")
            compare(value.lineCount, 1)
        }

        function test_longValueElidesInsteadOfWrappingOrOverrunning() {
            let row = createTemporaryObject(rowComponent, root, {
                valueText: "CQ · VBR · CBR · system-wide · and a great deal more evidence than the row has room for"
            })
            verify(!!row, "Component exists")
            waitForRendering(root)

            let value = findChild(row, "capabilityRowValue")
            verify(!!value, "value label exists")
            compare(value.lineCount, 1, "the row is one line by contract — a long value must not wrap")
            compare(value.truncated, true, "a value the row cannot fit must be elided")
            verify(value.x + value.width <= row.width + 1,
                   "the value must stay inside the row rather than overrunning the card")
        }
    }
}
