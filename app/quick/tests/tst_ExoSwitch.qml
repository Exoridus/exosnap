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
