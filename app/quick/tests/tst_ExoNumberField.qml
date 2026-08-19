import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// Every ExoNumberField with a non-empty suffix appends it to the displayed
// text (textFromValue), which SpinBox then has to parse back on blur
// (valueFromText). The suffix is what breaks the default parser -- without
// this coverage a regression here silently resets every suffixed field
// (bitrate, frame rate, split interval, segment size, DSP thresholds) to
// its minimum the moment the user tabs away.
TestCase {
    id: testCase

    name: "ExoNumberField"
    when: windowShown
    width: 200
    height: 80
    visible: true

    Component {
        id: fieldComponent

        ExoNumberField {
            from: 32
            to: 510
            stepSize: 8
            suffix: "kbps"
            value: 160
        }
    }

    function test_blur_after_a_stepper_click_keeps_the_stepped_value() {
        let field = createTemporaryObject(fieldComponent, testCase);
        verify(field);

        // A real click on the up stepper, mirroring the live report -- the
        // bug is specifically about the value SpinBox itself just wrote into
        // its display text, not about a property assigned from the test.
        mouseClick(field.up.indicator, field.up.indicator.width / 2, field.up.indicator.height / 2);
        compare(field.value, 168);

        field.forceActiveFocus();
        tryCompare(field, "activeFocus", true);
        field.focus = false;
        testCase.forceActiveFocus();

        tryCompare(field, "activeFocus", false);
        compare(field.value, 168);
    }
}
