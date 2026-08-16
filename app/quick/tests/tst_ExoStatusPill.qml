import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// QCR-407. The pill configures ElideRight, so it has to have something to elide
// against: a Label with no width limit is exactly as wide as its text, and
// `elide` on it is a no-op. In the title band that made a long state string
// incompressible, and the navigation tabs beside it paid for the extra width.
TestCase {
    id: testCase

    name: "ExoStatusPill"
    when: windowShown
    width: 400
    height: 120
    visible: true

    Component {
        id: pillComponent

        ExoStatusPill {
            text: "Ready"
            tone: "success"
        }
    }

    function findLabel(pill) {
        // Row → [dot, Label]. The label is the child that has text.
        for (let i = 0; i < pill.children.length; ++i) {
            let row = pill.children[i];
            for (let j = 0; j < row.children.length; ++j) {
                if (row.children[j].text !== undefined)
                    return row.children[j];
            }
        }
        return null;
    }

    function test_short_text_is_not_elided() {
        let pill = createTemporaryObject(pillComponent, testCase);
        verify(pill);
        let label = findLabel(pill);
        verify(label);

        // Left to its own devices the pill is as wide as it asked to be, and the
        // label gets its full implicit width.
        compare(pill.width, pill.implicitWidth);
        compare(label.width, label.implicitWidth);
        compare(label.truncated, false);
    }

    function test_a_narrow_pill_elides_instead_of_overflowing() {
        let pill = createTemporaryObject(pillComponent, testCase);
        verify(pill);
        pill.text = "Finalizing the recording — writing the last segment";
        let label = findLabel(pill);
        verify(label);

        let wanted = pill.implicitWidth;
        verify(wanted > 120);

        // What a layout does when the band runs out of room.
        pill.width = 120;

        compare(label.truncated, true);
        verify(label.width <= 120);
        // The text ends inside the pill rather than running out of it — the dot,
        // the gap and both insets are still accounted for.
        verify(label.x + label.width <= pill.width);
    }

    function test_widening_the_pill_restores_the_full_text() {
        let pill = createTemporaryObject(pillComponent, testCase);
        verify(pill);
        pill.text = "Finalizing the recording — writing the last segment";
        let label = findLabel(pill);
        verify(label);

        pill.width = 120;
        compare(label.truncated, true);

        pill.width = pill.implicitWidth;
        compare(label.truncated, false);
    }

    // Nothing is lost to a screen reader: the pill's accessible name is the
    // whole string whatever the label is showing.
    function test_the_accessible_name_keeps_the_full_text() {
        let pill = createTemporaryObject(pillComponent, testCase);
        verify(pill);
        pill.text = "Finalizing the recording — writing the last segment";
        pill.width = 100;

        compare(pill.Accessible.name, "Finalizing the recording — writing the last segment");
    }

    // The preview badge variant has larger insets and an uppercased label; its
    // implicit width has to follow both, or the pill clips its own text at rest.
    function test_the_on_surface_variant_still_fits_its_own_text() {
        let pill = createTemporaryObject(pillComponent, testCase);
        verify(pill);
        pill.onSurface = true;
        pill.text = "Recording";
        let label = findLabel(pill);
        verify(label);

        compare(label.truncated, false);
        verify(pill.implicitWidth >= label.implicitWidth);
    }
}
