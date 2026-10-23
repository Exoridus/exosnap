import QtQuick
import QtQuick.Controls
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: tests
    name: "EditInspectorInput"
    when: windowShown
    width: 800
    height: 700
    visible: true
    Component {
        id: inspectorComponent
        EditInspector { width: 360; height: 600; session: testSession; canvasFocusItem: tests }
    }
    function init() {
        testSession.cancelClipDrag();
        while (testSession.canUndo) testSession.undo();
        testSession.appendAsset(testSession.media[0].id);
        testSession.linkedSelection = false;
        const clip = testSession.visibleClipsUs(0, 100000000).find(row => row.video);
        testSession.clearSelection();
        testSession.selectClip(clip.id);
    }
    function cleanup() {
        testSession.cancelClipDrag();
        while (testSession.canUndo) testSession.undo();
        testSession.linkedSelection = true;
    }
    function test_supported_fade_options_are_enabled_and_activated_by_pointer() {
        const inspector = createTemporaryObject(inspectorComponent, tests);
        const curve = findChild(inspector, "editDetailsFadeInCurve");
        verify(!!curve && curve.visible);
        inspector.revealItem(curve);
        waitForRendering(inspector);
        curve.popup.open();
        tryVerify(() => curve.popup.contentItem.itemAtIndex(2) !== null);
        const option = curve.popup.contentItem.itemAtIndex(2);
        verify(option.enabled, "The actual curve popup option is enabled");
        waitForRendering(tests);
        mouseClick(option, option.width / 2, option.height / 2);
        compare(testSession.selectedClipDetails.fadeInCurve, 2);
    }
    function findAccessibleItem(item, name) {
        if (item.Accessible.name === name)
            return item;
        for (const child of item.children || []) {
            const match = findAccessibleItem(child, name);
            if (match)
                return match;
        }
        return null;
    }
    function junctionBiasEditor() {
        testSession.appendAsset(testSession.media[0].id);
        const first = testSession.visibleClipsUs(0, 200000000).find(row => row.video && row.startUs === 0);
        testSession.applyCrossfade(first.id, 500);
        const audio = testSession.transitions.find(row => !row.video);
        const video = testSession.transitions.find(row => row.video);
        verify(!!audio && !!video);
        testSession.selectTransition(audio.outgoing);
        testSession.setJunctionCurve(2, 0.1);
        compare(testSession.selectedJunctionDetails.curve, 2);
        const inspector = createTemporaryObject(inspectorComponent, tests);
        verify(!!inspector);
        const field = findAccessibleItem(inspector, "Junction curve bias");
        verify(!!field && field.visible);
        inspector.revealItem(field);
        waitForRendering(inspector);
        const input = field.contentItem || field;
        input.forceActiveFocus(Qt.TabFocusReason);
        tryCompare(input, "activeFocus", true);
        input.selectAll();
        return {audio, video, field, input};
    }
    function test_valid_junction_bias_draft_commits_to_original_junction_before_selection() {
        const edit = junctionBiasEditor();
        try {
            keyClick("2");
            keyClick("5");
            compare(edit.input.text, "25");
            testSession.selectTransition(edit.video.outgoing);
            compare(testSession.selectedJunctionDetails.id, edit.video.id);
            compare(testSession.transitions.find(row => row.id === edit.audio.id).bias, 0.25);
            compare(testSession.transitions.find(row => row.id === edit.video.id).bias, 0);
        } finally {
            keyClick(Qt.Key_Escape);
            tests.forceActiveFocus();
        }
    }
    function test_invalid_junction_bias_draft_blocks_selection_and_preserves_text() {
        const edit = junctionBiasEditor();
        try {
            keyClick("-");
            compare(edit.input.text, "-");
            testSession.selectTransition(edit.video.outgoing);
            compare(testSession.selectedJunctionDetails.id, edit.audio.id);
            compare(edit.input.text, "-");
            verify(edit.input.activeFocus);
        } finally {
            keyClick(Qt.Key_Escape);
            tests.forceActiveFocus();
        }
    }
}
