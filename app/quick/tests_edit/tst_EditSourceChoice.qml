import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: testCase
    name: "EditSourceChoice"
    width: 640
    height: 480
    visible: true
    when: windowShown
    Component {
        id: choiceComponent
        EditSourceChoice { session: testPageSession }
    }
    function settle() {
        verify(waitForPolish(testCase.Window.window));
        testCase.Window.window.update();
        verify(waitForRendering(testCase.Window.window.contentItem));
    }
    function prepare() {
        const fixture = testEditHarness.prepareSourceAudioChoice();
        verify(fixture.ready, fixture.reason || "Real source audio fixture is unavailable");
        const dialog = createTemporaryObject(choiceComponent, testCase);
        verify(!!dialog);
        dialog.open();
        tryCompare(dialog, "opened", true);
        settle();
        return dialog;
    }
    function cleanup() {
        testEditHarness.clearSourceAudioChoiceFixture();
    }
    function track(dialog, index) {
        settle();
        tryVerify(() => {
            let previousBottom = 0;
            for (const row of testPageSession.sourceAudioChoice.tracks) {
                const item = findChild(dialog.contentItem, "editSourceAudioTrack-" + row.id);
                if (!item || !item.visible || item.width <= 0 || item.height <= 0)
                    return false;
                const position = item.mapToItem(dialog.contentItem, 0, 0);
                if (position.y < previousBottom || position.y + item.height > dialog.contentItem.height)
                    return false;
                previousBottom = position.y + item.height;
            }
            return true;
        });
        const id = testPageSession.sourceAudioChoice.tracks[index].id;
        const control = findChild(dialog.contentItem, "editSourceAudioTrack-" + id);
        verify(control.Accessible.name.length > 0);
        return control;
    }
    function test_single_stream_apply_needs_no_overlap_checkbox() {
        const dialog = prepare();
        const draft = testPageSession.sourceAudioChoice;
        compare(draft.tracks.length, 2);
        verify(dialog.title.indexOf(draft.name) >= 0);
        mouseClick(track(dialog, 0));
        tryVerify(() => testPageSession.sourceAudioChoice.tracks[0].selected);
        const overlap = findChild(dialog, "editSourceAudioOverlap");
        verify(!!overlap);
        verify(!overlap.visible);
        const apply = findChild(dialog, "editSourceAudioApply");
        verify(apply.enabled);
        settle();
        mouseClick(apply);
        tryVerify(() => !testPageSession.sourceAudioChoice.active, 5000);
        tryCompare(dialog, "visible", false);
        compare(testPageSession.sourceAudioChoices.length, 0);
    }
    function test_multiple_unknown_streams_require_explicit_overlap_and_cancel_discards_draft() {
        const dialog = prepare();
        const asset = testPageSession.sourceAudioChoice.assetId;
        mouseClick(track(dialog, 0));
        tryVerify(() => testPageSession.sourceAudioChoice.tracks[0].selected);
        const second = track(dialog, 1);
        mouseClick(second);
        tryVerify(() => testPageSession.sourceAudioChoice.tracks[1].selected);
        const overlap = findChild(dialog, "editSourceAudioOverlap");
        verify(overlap.visible);
        const apply = findChild(dialog, "editSourceAudioApply");
        verify(!apply.enabled);
        mouseClick(overlap);
        verify(apply.enabled);
        dialog.close();
        tryCompare(dialog, "visible", false);
        compare(testPageSession.sourceAudioChoices.length, 1);
        verify(testPageSession.beginSourceAudioChoice(asset));
        verify(testPageSession.sourceAudioChoice.tracks.every(row => !row.selected));
        testPageSession.cancelSourceAudioChoice();
    }
    function test_stream_selection_preserves_the_focused_control_for_keyboard_toggle() {
        const dialog = prepare();
        const first = track(dialog, 0);
        mouseClick(first);
        tryVerify(() => testPageSession.sourceAudioChoice.tracks[0].selected);
        verify(first && first.activeFocus, "Updating a stream choice must keep its focused control");
        keyClick(Qt.Key_Space);
        tryVerify(() => !testPageSession.sourceAudioChoice.tracks[0].selected);
    }
    function test_destroying_the_dialog_cancels_its_unapplied_draft() {
        const dialog = prepare();
        verify(testPageSession.sourceAudioChoice.active);
        dialog.destroy();
        tryVerify(() => !testPageSession.sourceAudioChoice.active);
    }
}
