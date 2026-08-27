import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// QCR-412 (verify-first). The knob's `x` is a binding on `checked`, animated by
// an XAnimator inside a Behavior.
//
// Animators run on the render thread and are known to interact badly with
// property bindings when they are driven imperatively — the suspicion was that
// the knob could be left at an absolute position a later layout or size change
// no longer agrees with. What these assert is the outcome that matters: after
// every toggle, and after the control's own geometry moves, the knob is where
// the binding says it should be.
TestCase {
    id: testCase

    name: "ExoSwitch"
    when: windowShown
    width: 300
    height: 120
    visible: true

    Component {
        id: switchComponent

        ExoSwitch {
            text: "Mix into previous track"
        }
    }

    function knobOf(control) {
        return control.indicator.children[0];
    }

    function restingX(control, checked) {
        let knob = knobOf(control);
        return checked ? control.indicator.width - knob.width - 3 : 3;
    }

    function test_the_knob_settles_at_both_ends() {
        let control = createTemporaryObject(switchComponent, testCase);
        verify(control);
        let knob = knobOf(control);
        verify(knob);

        compare(knob.x, restingX(control, false));

        control.checked = true;
        tryCompare(knob, "x", restingX(control, true));

        control.checked = false;
        tryCompare(knob, "x", restingX(control, false));
    }

    function test_repeated_toggles_do_not_drift() {
        let control = createTemporaryObject(switchComponent, testCase);
        verify(control);
        let knob = knobOf(control);

        for (let i = 0; i < 6; ++i) {
            control.checked = !control.checked;
            tryCompare(knob, "x", restingX(control, control.checked));
        }
    }

    // The animation is what could have left an absolute value behind. Toggling
    // again before it lands must still resolve to the binding's answer.
    function test_a_toggle_mid_animation_still_lands_on_the_binding() {
        let control = createTemporaryObject(switchComponent, testCase);
        verify(control);
        let knob = knobOf(control);

        control.checked = true;
        control.checked = false;
        control.checked = true;

        tryCompare(knob, "x", restingX(control, true));
    }

    // A width change is what a responsive layout does. If the animator had
    // broken the binding, the knob would stay at the old absolute position.
    function test_the_knob_follows_a_geometry_change() {
        let control = createTemporaryObject(switchComponent, testCase);
        verify(control);
        let knob = knobOf(control);

        control.checked = true;
        tryCompare(knob, "x", restingX(control, true));

        control.indicator.width = 64;
        tryCompare(knob, "x", restingX(control, true));
        compare(knob.x, 64 - knob.width - 3);
    }

    // Blocked is a state, and a blocked switch that is ON still has to read as
    // ON. The previous treatment dropped the accent fill for the plain recessed
    // surface, which made a locked-on switch identical to a locked-off one --
    // during a recording, when "is my system audio being captured?" is the
    // question the row exists to answer.
    function test_a_blocked_switch_still_says_whether_it_is_on() {
        let on = createTemporaryObject(switchComponent, testCase, { checked: true, enabled: false });
        let off = createTemporaryObject(switchComponent, testCase, { checked: false, enabled: false });
        verify(on);
        verify(off);

        verify(!Qt.colorEqual(on.indicator.color, off.indicator.color),
               "a blocked ON switch must not paint the same track as a blocked OFF one");
        verify(!Qt.colorEqual(on.indicator.border.color, off.indicator.border.color),
               "a blocked ON switch keeps the accent hairline");
    }

    // And it still has to read as blocked. The knob is what carries that: full
    // ink on a live control, the dimmest rung on a blocked one, in both checked
    // states -- otherwise "off" and "off and unusable" are the same picture.
    function test_a_blocked_switch_dims_its_knob_in_both_states() {
        let live = createTemporaryObject(switchComponent, testCase, { checked: false, enabled: true });
        let blockedOff = createTemporaryObject(switchComponent, testCase, { checked: false, enabled: false });
        let blockedOn = createTemporaryObject(switchComponent, testCase, { checked: true, enabled: false });
        verify(live);
        verify(blockedOff);
        verify(blockedOn);

        verify(!Qt.colorEqual(knobOf(live).color, knobOf(blockedOff).color),
               "off and blocked-off must not paint the same knob");
        compare(knobOf(blockedOn).color, knobOf(blockedOff).color);
    }

    // Toggled while the control is not being rendered — the case an Animator
    // genuinely cannot animate. It still has to arrive at the right place once
    // the control is shown again.
    function test_a_toggle_while_hidden_is_correct_when_shown() {
        let control = createTemporaryObject(switchComponent, testCase);
        verify(control);
        let knob = knobOf(control);

        control.visible = false;
        control.checked = true;
        control.visible = true;

        tryCompare(knob, "x", restingX(control, true));
    }
}
