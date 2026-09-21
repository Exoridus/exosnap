import QtQuick
import QtQuick.Controls
import QtTest

import ExoSnap.Quick.TestControls

// The row's secondary line is fed by two properties that mean different things.
// `hint` is an explanation and excludes `info`; `readout` is the value the
// setting currently resolves to and excludes neither, because a quantizer and a
// paragraph about the quality scale cannot restate each other. The product spec
// requires the CQ row to carry both at once, so the pair has to stay legal.
TestCase {
    id: testCase

    name: "ExoSettingRow"
    when: windowShown
    width: 520
    height: 200
    visible: true

    Component {
        id: rowComponent

        ExoSettingRow {
            width: 480
            label: "Constant quality (CQ)"

            Switch {}
        }
    }

    function findSecondaryLine(item) {
        if (!item)
            return null;
        if (item.objectName === "secondaryLine")
            return item;
        const kids = item.children;
        for (let i = 0; i < kids.length; ++i) {
            const found = testCase.findSecondaryLine(kids[i]);
            if (found)
                return found;
        }
        return null;
    }

    function test_secondary_line_source_data() {
        return [
            {
                tag: "hint alone",
                props: { hint: "Speed versus quality inside the encoder" },
                text: "Speed versus quality inside the encoder",
                shown: true
            },
            {
                tag: "readout alone",
                props: { readout: "AV1 qindex 65 of 255" },
                text: "AV1 qindex 65 of 255",
                shown: true
            },
            {
                tag: "readout beside info",
                props: { readout: "AV1 qindex 65 of 255", info: "ExoSnap's own quality scale, 1 to 51." },
                text: "AV1 qindex 65 of 255",
                shown: true
            },
            {
                tag: "info alone",
                props: { info: "ExoSnap's own quality scale, 1 to 51." },
                text: "",
                shown: false
            },
            {
                tag: "neither",
                props: {},
                text: "",
                shown: false
            }
        ];
    }

    function test_secondary_line_source(data) {
        let row = createTemporaryObject(rowComponent, testCase, data.props);
        verify(row);
        let line = testCase.findSecondaryLine(row);
        verify(line, "the row publishes a secondary line");
        compare(line.text, data.text);
        compare(line.visible, data.shown);
    }

    // `hint` wins when both are set, so a row that means to show a value never
    // silently loses an explanation it also declared.
    function test_hint_outranks_readout() {
        let row = createTemporaryObject(rowComponent, testCase, {
            hint: "Target bitrate for VBR/CBR",
            readout: "AV1 qindex 65 of 255"
        });
        verify(row);
        compare(testCase.findSecondaryLine(row).text, "Target bitrate for VBR/CBR");
    }

    // The pair the product spec requires: the CQ row explains the quality scale
    // in a popover and still names the quantizer the scale resolves to. The row
    // rejects `hint` beside `info`, so this is the only way to carry both.
    function test_readout_and_info_are_accepted() {
        let row = createTemporaryObject(rowComponent, testCase, {
            readout: "AV1 qindex 65 of 255",
            info: "ExoSnap's own quality scale, 1 to 51."
        });
        verify(row);
        compare(testCase.findSecondaryLine(row).text, "AV1 qindex 65 of 255");
    }
}
