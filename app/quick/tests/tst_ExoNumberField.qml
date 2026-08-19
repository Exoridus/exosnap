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

    // A negative-range field (mirroring the mic gain/threshold fields), since
    // the throwing case needs a lone "-" to be a value the validator accepts
    // as an intermediate state at all.
    Component {
        id: negativeFieldComponent

        ExoNumberField {
            from: -12
            to: 12
            suffix: "dB"
            value: 3
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

    function test_blur_after_select_all_delete_keeps_the_value() {
        let field = createTemporaryObject(fieldComponent, testCase);
        verify(field);

        // Mirrors selecting the whole displayed text (suffix included) and
        // deleting it -- valueFromText then sees an empty string.
        field.forceActiveFocus();
        tryCompare(field, "activeFocus", true);
        field.contentItem.selectAll();
        field.contentItem.remove(field.contentItem.selectionStart, field.contentItem.selectionEnd);
        field.focus = false;
        testCase.forceActiveFocus();

        tryCompare(field, "activeFocus", false);
        compare(field.value, 160);
    }

    function test_blur_with_a_lone_minus_sign_keeps_the_value() {
        let field = createTemporaryObject(negativeFieldComponent, testCase);
        verify(field);

        // Mirrors selecting the whole displayed text and typing a lone "-",
        // a valid intermediate state for a field whose range goes negative.
        // Number.fromLocaleString throws on a lone "-" instead of returning
        // NaN, so without the guard this escaped the isFinite fallback and
        // the field snapped to `from` (the exact bug this component exists
        // to fix) with a JS error logged besides.
        field.forceActiveFocus();
        tryCompare(field, "activeFocus", true);
        field.contentItem.selectAll();
        field.contentItem.remove(field.contentItem.selectionStart, field.contentItem.selectionEnd);
        field.contentItem.insert(0, "-");
        field.focus = false;
        testCase.forceActiveFocus();

        tryCompare(field, "activeFocus", false);
        compare(field.value, 3);
    }
}
