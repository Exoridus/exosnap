import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: testCase
    name: "EditGainField"
    when: windowShown
    visible: true
    width: 640
    height: 240
    property int commits: 0
    property real applied: 0
    property var appliedTargets: []
    Item { id: canvas; width: 600; height: 200 }
    Component {
        id: fieldComponent
        EditGainField {
            session: testSession
            width: 240
            gainDb: -0.5
            canvasFocusItem: canvas
            applyValue: (value, target, context) => {
                testCase.applied = value;
                testCase.appliedTargets = context && context.targets ? context.targets : [];
                testCase.commits++;
                return true;
            }
        }
    }
    function init() {
        commits = 0;
        applied = 0;
        appliedTargets = [];
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
    function test_decimal_comma_applies_exact_tenths_then_restores_canvas() {
        const item = field();
        item.selectAll();
        typeText("-0,7 dB");
        keyClick(Qt.Key_Return);
        compare(commits, 1);
        compare(applied, -0.7);
        verify(canvas.activeFocus);
    }
    function test_track_limit_and_extra_precision_keep_draft_until_escape() {
        const item = field({track: true});
        item.selectAll();
        typeText("12.1");
        keyClick(Qt.Key_Return);
        compare(commits, 0);
        verify(item.draftActive && item.activeFocus);
        verify(item.errorText.length > 0);
        item.selectAll();
        typeText("0.01");
        keyClick(Qt.Key_Tab);
        compare(commits, 0);
        verify(item.draftActive && item.activeFocus);
        compare(item.text, "0.01");
        keyClick(Qt.Key_Escape);
        verify(canvas.activeFocus);
        verify(!item.draftActive);
        compare(commits, 0);
    }
    function test_escape_without_a_draft_restores_canvas_focus() {
        const item = field();
        verify(item.commitPendingDraft());
        verify(item.activeFocus && !item.draftActive);
        keyClick(Qt.Key_Escape);
        tryVerify(() => canvas.activeFocus);
        compare(commits, 0);
    }
    function test_background_gain_update_does_not_replace_active_text() {
        const item = field();
        item.selectAll();
        typeText("1.5");
        item.gainDb = 8;
        compare(item.text, "1.5");
        keyClick(Qt.Key_Return);
        compare(commits, 1);
        compare(applied, 1.5);
    }
    function test_multi_target_draft_keeps_its_captured_ids() {
        const item = field({targetIds: [31, 41]});
        item.selectAll();
        typeText("1.3");
        item.targetIds = [99];
        keyClick(Qt.Key_Return);
        compare(commits, 1);
        compare(appliedTargets, [31, 41]);
    }
    function test_context_hiding_cancels_an_invalid_draft() {
        const item = field();
        item.selectAll();
        typeText("0.01");
        keyClick(Qt.Key_Return);
        verify(item.draftActive);
        item.visible = false;
        verify(!item.draftActive);
        verify(testSession.preflightPendingDraft());
    }
}
