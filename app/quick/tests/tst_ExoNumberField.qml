import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// Unit decoration must never change a value on focus loss or become editable.
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

    SignalSpy {
        id: resetSpy
        signalName: "resetRequested"
    }

    function test_default_reset_is_keyboard_accessible_and_does_not_break_value_binding() {
        const field = createTemporaryObject(negativeFieldComponent, testCase);
        field.resetEnabled = true;
        field.value = Qt.binding(() => 3);
        resetSpy.target = field;
        resetSpy.clear();
        field.forceActiveFocus();
        keyClick(Qt.Key_F10, Qt.ShiftModifier);
        const menu = findChild(field, "numberFieldResetMenu");
        verify(menu);
        tryCompare(menu, "opened", true);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Return);
        compare(resetSpy.count, 1);
        tryCompare(menu, "visible", false);
        compare(field.value, 3);
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

    function test_decimal_entry_locale_units_validation_and_keyboard_step() {
        const field = createTemporaryObject(negativeFieldComponent, testCase);
        verify(field);
        field.decimals = 3;
        field.stepSize = 0.5;
        field.locale = Qt.locale("de_DE");
        field.forceActiveFocus();
        field.contentItem.selectAll();
        field.contentItem.remove(field.contentItem.selectionStart, field.contentItem.selectionEnd);
        field.contentItem.insert(0, "-3,125");
        keyClick(Qt.Key_Return);
        compare(field.value, -3.125);
        keyClick(Qt.Key_Up);
        compare(field.value, -2.625);
        field.contentItem.selectAll();
        field.contentItem.remove(field.contentItem.selectionStart, field.contentItem.selectionEnd);
        field.contentItem.insert(0, "99");
        keyClick(Qt.Key_Return);
        verify(field.value >= field.from && field.value <= field.to);
        compare(field.valueFromText("garbage", field.locale), field.value);
        compare(field.valueFromText("1,5", field.locale), 1.5);
        field.focus = false;
        testCase.forceActiveFocus();
        compare(field.value >= field.from && field.value <= field.to, true);
    }
}
