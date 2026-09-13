import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// One invariant, applied to every component that draws caller-supplied text:
// a label that is not allowed to elide must fit the width it was given.
//
// The pseudo-localization harness already renders these surfaces with text grown
// by 40 % and brackets at both ends, which is how a truncation is SEEN -- a
// capture missing its closing bracket is a capture in which the layout cut the
// string. Nothing asserted it, so a regression was only ever caught by a person
// looking at a render, and only on the surfaces a scenario happened to cover.
//
// What this checks is the mechanical half. A label with elide set to ElideNone
// and contentWidth greater than its width is drawn clipped or overflowing
// depending on whether an ancestor clips, and neither is a layout anyone chose.
// Elision itself is not the failure: several components elide deliberately, and
// those have their own tests saying so.
TestCase {
    id: testCase

    name: "TextGeometryInvariants"
    when: windowShown
    width: 420
    height: 320
    visible: true

    // The same growth factor the pseudo-localization translator applies, with
    // the same bracketing, so a failure here and a failure in a render are the
    // same failure.
    function pseudo(source) {
        if (source.length === 0)
            return source;
        let padding = "";
        const wanted = Math.ceil(source.length * 0.4);
        while (padding.length < wanted)
            padding += "š";
        return "«" + source + padding + "»";
    }

    function collectText(item, found) {
        if (item === null || item === undefined)
            return found;
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i];
            if (child === null || child === undefined)
                continue;
            // contentWidth is the marker of a text-rendering item: Text and
            // Label both carry it, and nothing else in these components does.
            if (child.visible && child.contentWidth !== undefined && child.text !== undefined
                    && String(child.text).length > 0)
                found.push(child);
            collectText(child, found);
        }
        return found;
    }

    function assertFits(root, label) {
        const texts = collectText(root, []);
        verify(texts.length > 0, label + ": no text items were found, so nothing was checked");
        for (let i = 0; i < texts.length; ++i) {
            const text = texts[i];
            verify(text.width > 0 && text.height > 0,
                   label + ": a visible label collapsed to " + text.width + "x" + text.height);
            if (text.elide === Text.ElideNone) {
                // One pixel of slack: contentWidth is a float and the layout
                // rounds, so an exact comparison reports a rendering that is
                // correct on screen.
                verify(text.contentWidth <= text.width + 1,
                       label + ": '" + text.text + "' needs " + text.contentWidth
                       + " px in " + text.width + " px and is not allowed to elide");
            }
        }
    }

    Component {
        id: noticeComponent

        ExoNotice {
            width: 360
            text: testCase.sample
        }
    }

    Component {
        id: badgeComponent

        ExoBadge {
            width: 90
            text: testCase.sample
        }
    }

    Component {
        id: statusTileComponent

        ExoStatusTile {
            width: 200
            title: testCase.sample
            value: testCase.sampleValue
            sub: testCase.sample
        }
    }

    Component {
        id: codecChipComponent

        DeviceCodecChip {
            label: testCase.sample
            available: true
        }
    }

    Component {
        id: buttonComponent

        ExoButton {
            // A width the label has to fit into. Left to its implicit width a
            // button grows with its text and the invariant has nothing to say;
            // the toolbar and dialog rows that place these do not let them grow.
            width: 120
            text: testCase.sample
        }
    }

    property string sample: "Ready"
    property string sampleValue: "0"

    function componentsUnderTest() {
        return [
            { factory: noticeComponent, label: "ExoNotice" },
            { factory: badgeComponent, label: "ExoBadge" },
            { factory: statusTileComponent, label: "ExoStatusTile" },
            { factory: codecChipComponent, label: "DeviceCodecChip" },
            { factory: buttonComponent, label: "ExoButton" }
        ];
    }

    function runInvariant(text, value) {
        testCase.sample = text;
        testCase.sampleValue = value;
        const cases = componentsUnderTest();
        for (let i = 0; i < cases.length; ++i) {
            const item = createTemporaryObject(cases[i].factory, testCase);
            verify(item !== null, cases[i].label + " did not instantiate");
            // No waitForRendering: these components are not shown, and waiting
            // for a frame that never comes costs the suite its whole timeout.
            // Layout is what is measured here, and Qt Quick polishes it on the
            // first read of a geometry property.
            assertFits(item, cases[i].label);
        }
    }

    function test_ordinary_text_fits() {
        runInvariant("Ready", "60 fps");
    }

    function test_pseudo_localized_text_fits() {
        // The growth a German string of the same meaning is expected to bring.
        runInvariant(pseudo("Ready"), pseudo("60 fps"));
    }

    function test_a_long_string_is_elided_rather_than_clipped() {
        // Not a layout anyone designed for, and precisely why it has to be the
        // component's own decision: whatever it does with a string this long,
        // the label it drew must still be one it could draw.
        runInvariant(
            "Recording to a removable volume that stopped responding during the session",
            "1920x1080 at 240 fps, AV1, 120 Mbit/s");
    }

    function test_the_invariant_holds_with_more_text_than_the_layout_expects() {
        // The closest a component test gets to a scaled display without driving
        // one: what a larger face does to these layouts is make the text wider
        // than the space reserved for it, which is what a longer string does too.
        runInvariant(pseudo(pseudo("Ready")), pseudo(pseudo("60 fps")));
    }
}
