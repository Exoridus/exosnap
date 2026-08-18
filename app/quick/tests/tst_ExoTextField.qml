import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// QCR-413. A text field being edited owns its draft.
//
// The Settings adapters publish one aggregate change signal, and it fires for
// reasons that have nothing to do with the field the user is in: a capability
// delivery from a background probe, a preset sanitize, any other control on the
// page. With `text: adapter.value` that signal re-evaluates the binding and
// replaces whatever has been typed so far — typing does NOT break a QML binding
// on `text`, because Qt only drops a binding on an imperative JS write.
//
// The QtObject below is that adapter: one property, one change signal.
TestCase {
    id: testCase

    name: "ExoTextField"
    when: windowShown
    width: 400
    height: 120
    visible: true

    Component {
        id: fieldComponent

        Item {
            property alias field: field
            property alias backing: backing

            width: 400
            height: 60

            QtObject {
                id: backing

                property string canonicalValue: "recording-%date%"
            }

            ExoTextField {
                id: field

                width: 300
                value: backing.canonicalValue

                onCommitted: value => backing.canonicalValue = value
            }
        }
    }

    function test_the_canonical_value_is_shown_when_nothing_is_being_edited() {
        let fixture = createTemporaryObject(fieldComponent, testCase);
        verify(fixture);

        compare(fixture.field.text, "recording-%date%");

        // An external change with no editor in the field is still adopted —
        // that is the whole point of showing a backing value.
        fixture.backing.canonicalValue = "clip-%date%";
        compare(fixture.field.text, "clip-%date%");
    }

    function test_a_backing_refresh_does_not_overwrite_an_edit_in_progress() {
        let fixture = createTemporaryObject(fieldComponent, testCase);
        verify(fixture);

        fixture.field.forceActiveFocus();
        fixture.field.text = "";
        keyClick(Qt.Key_S);
        keyClick(Qt.Key_E);
        keyClick(Qt.Key_S);
        compare(fixture.field.text, "ses");

        // A capability delivery lands mid-word.
        fixture.backing.canonicalValue = "clip-%date%";

        compare(fixture.field.text, "ses");
        compare(fixture.field.activeFocus, true);
    }

    function test_committing_publishes_the_draft_and_takes_back_the_canonical_value() {
        let fixture = createTemporaryObject(fieldComponent, testCase);
        verify(fixture);

        fixture.field.forceActiveFocus();
        fixture.field.text = "session-%n%";
        fixture.field.editingFinished();

        compare(fixture.backing.canonicalValue, "session-%n%");
        compare(fixture.field.text, "session-%n%");
    }

    // What a rejected or normalised value looks like: the adapter keeps its own
    // answer, and the field has to show that rather than the draft.
    function test_a_value_the_backing_refuses_is_replaced_by_what_it_kept() {
        let fixture = createTemporaryObject(fieldComponent, testCase);
        verify(fixture);
        fixture.field.onCommitted.connect(function () {
            // A sanitizer that rewrites what it was given.
            fixture.backing.canonicalValue = "sanitised";
        });

        fixture.field.forceActiveFocus();
        fixture.field.text = "  spaces  ";
        fixture.field.editingFinished();

        compare(fixture.field.text, "sanitised");
    }

    // Leaving the field without committing anything. editingFinished fires on
    // focus loss too, so the canonical value is what stays on screen either way.
    function test_losing_focus_restores_the_canonical_value() {
        let fixture = createTemporaryObject(fieldComponent, testCase);
        verify(fixture);

        fixture.field.forceActiveFocus();
        fixture.field.text = "half-typed";
        fixture.field.focus = false;
        fixture.forceActiveFocus();

        tryCompare(fixture.field, "activeFocus", false);
        compare(fixture.field.text, fixture.backing.canonicalValue);
    }
}
