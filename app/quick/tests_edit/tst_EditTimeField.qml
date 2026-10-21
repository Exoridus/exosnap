import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: testCase
    name: "EditTimeField"
    when: windowShown
    visible: true
    width: 640
    height: 240
    property int commits: 0
    property real appliedValue: 0
    property var appliedTarget: 0

    Item { id: canvas; width: 600; height: 200 }
    Component {
        id: fieldComponent
        EditTimeField {
            session: testSession
            width: 240
            valueUs: 12345
            targetId: 0
            canvasFocusItem: canvas
            applyValue: (value, target) => {
                testCase.commits++;
                testCase.appliedValue = value;
                testCase.appliedTarget = target;
                return true;
            }
        }
    }

    function init() {
        commits = 0;
        appliedValue = 0;
        appliedTarget = 0;
        canvas.forceActiveFocus();
    }
    function field(properties) {
        const item = createTemporaryObject(fieldComponent, testCase, properties || {});
        verify(!!item);
        item.forceActiveFocus(Qt.TabFocusReason);
        tryCompare(item, "activeFocus", true);
        return item;
    }
    function typeText(text) {
        for (let index = 0; index < text.length; ++index)
            keyClick(text[index]);
    }
    function test_unchanged_timecode_does_not_quantize_legacy_microseconds() {
        const item = field({format: 0});
        verify(item.draftActive);
        keyClick(Qt.Key_Return);
        compare(commits, 0);
        compare(item.valueUs, 12345);
        verify(canvas.activeFocus);
    }
    function test_invalid_enter_and_tab_keep_draft_until_escape() {
        const item = field();
        item.selectAll();
        typeText("0.0000001s");
        keyClick(Qt.Key_Return);
        compare(commits, 0);
        compare(item.text, "0.0000001s");
        verify(item.errorText.length > 0);
        verify(item.activeFocus);
        keyClick(Qt.Key_Tab);
        verify(item.activeFocus);
        verify(item.draftActive);
        keyClick(Qt.Key_Escape);
        compare(commits, 0);
        compare(item.errorText, "");
        verify(!item.draftActive);
        verify(canvas.activeFocus);
    }
    function test_relative_draft_keeps_captured_base_during_background_updates() {
        const item = field();
        item.selectAll();
        typeText("+1s");
        item.valueUs = 9000000;
        compare(item.text, "+1s");
        keyClick(Qt.Key_Return);
        compare(commits, 1);
        compare(appliedValue, 1012345);
        compare(appliedTarget, 0);
        verify(canvas.activeFocus);
    }
    function test_rejected_numeric_apply_retains_editable_text() {
        const item = field({applyValue: () => false});
        item.selectAll();
        typeText("2s");
        keyClick(Qt.Key_Return);
        compare(item.text, "2s");
        verify(item.activeFocus);
        verify(item.draftActive);
        verify(item.errorText.length > 0);
        keyClick(Qt.Key_Escape);
    }
    function test_linked_policy_change_preflights_captured_sample_draft() {
        const original = testSession.linkedSelection;
        testSession.linkedSelection = false;
        let appliedLinked = true;
        const item = field({format: 2, sampleContext: true, applyValue: value => {
            testCase.commits++;
            testCase.appliedValue = value;
            appliedLinked = testSession.linkedSelection;
            return true;
        }});
        item.selectAll();
        typeText("48001");
        testSession.linkedSelection = true;
        compare(commits, 1, "A policy change commits the draft under the captured prior policy");
        compare(appliedLinked, false);
        compare(appliedValue, 1000021);
        compare(testSession.linkedSelection, true);
        testSession.linkedSelection = original;
    }
    function test_invalid_sample_draft_refuses_linked_policy_change() {
        const original = testSession.linkedSelection;
        testSession.linkedSelection = false;
        const item = field({format: 2, sampleContext: true});
        item.selectAll();
        typeText("invalid");
        testSession.linkedSelection = true;
        compare(testSession.linkedSelection, false, "An invalid draft keeps its captured editing policy");
        verify(item.activeFocus && item.draftActive && item.errorText.length > 0);
        compare(commits, 0);
        keyClick(Qt.Key_Escape);
        testSession.linkedSelection = original;
    }
    function test_bare_seconds_follow_display_format_independently_of_audio_grid() {
        const item = field({sampleContext: true, format: 1, valueUs: 1000000});
        item.selectAll();
        typeText("2");
        keyClick(Qt.Key_Return);
        compare(commits, 1);
        compare(appliedValue, 2000000);
    }
    function test_bare_samples_follow_display_format_independently_of_frame_grid() {
        const item = field({sampleContext: false, format: 2, valueUs: 0});
        item.selectAll();
        typeText("48000");
        keyClick(Qt.Key_Return);
        compare(commits, 1);
        compare(appliedValue, 1000000);
    }
    function test_audio_only_default_is_readable_seconds_and_keeps_exact_draft() {
        const original = testChannelSession.positionUs;
        const item = field({session: testChannelSession, valueUs: 1234567, applyValue: (value, target) => {
            const accepted = testChannelSession.applyTimeValue(target, "playhead", value);
            if (accepted) {
                testCase.commits++;
                testCase.appliedValue = value;
            }
            return accepted;
        }});
        try {
            compare(item.text, "1.234567");
            compare(item.effectiveFormat, 1);
            compare(item.context.format, 1);
            item.selectAll();
            typeText("1,5");
            keyClick(Qt.Key_Return);
            compare(commits, 1);
            compare(appliedValue, 1500000);
            compare(testChannelSession.positionUs, 1500000);
            verify(canvas.activeFocus);
            const exact = testChannelSession.parseTimeValue("1,500001s", 0, false, 1);
            verify(exact.valid);
            compare(exact.valueUs, 1500001);
        } finally {
            item.cancelPendingDraft("");
            testChannelSession.requestSeekUs(original);
        }
    }
    function test_audio_only_explicit_samples_and_frame_refusal_remain_distinct() {
        const item = field({session: testChannelSession, format: 2, sampleContext: true, valueUs: 1000000});
        compare(item.text, "48000");
        item.selectAll();
        typeText("48001");
        keyClick(Qt.Key_Return);
        compare(appliedValue, 1000021);
        const frames = testChannelSession.parseTimeValue("1f", 0, false, 0);
        verify(!frames.valid);
        verify(frames.error.length > 0);
    }
}
