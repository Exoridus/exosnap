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
    function test_auto_key_start_and_midpoint_undo() {
        const inspector = createTemporaryObject(inspectorComponent, tests);
        testSession.autoKey = true;
        testSession.requestSeekUs(5000000);
        const id = testSession.selectedClipDetails.id;
        verify(testSession.setVideoProperty(id, 0, 0.25));
        compare(inspector.videoValues[0].value, 0.25);
        compare(testSession.videoProperties(id, 0)[0].key, true);
        compare(testSession.videoProperties(id, 0)[0].value, 0);
        compare(testSession.videoProperties(id, 2500000)[0].value, 0.125);
        compare(testSession.videoProperties(id, 5000000)[0].key, true);
        testSession.autoKey = false;
        verify(testSession.setVideoKeyValue(id, 0, 5000000, 0.4));
        compare(testSession.videoProperties(id, 0)[0].value, 0);
        compare(testSession.videoProperties(id, 5000000)[0].value, 0.4);
        testSession.undo();
        testSession.undo();
        compare(testSession.videoProperties(id, 5000000)[0].animated, false);
        testSession.autoKey = false;
    }
    function test_diamond_removes_only_current_key_and_retains_last_value() {
        testSession.autoKey = true;
        testSession.requestSeekUs(5000000);
        const id = testSession.selectedClipDetails.id;
        verify(testSession.setVideoProperty(id, 4, 0.4));
        verify(testSession.toggleVideoKey(id, 4));
        compare(testSession.videoProperties(id, 0)[4].key, true);
        compare(testSession.videoProperties(id, 5000000)[4].key, false);
        testSession.requestSeekUs(0);
        verify(testSession.toggleVideoKey(id, 4));
        compare(testSession.videoProperties(id, 0)[4].animated, false);
        compare(testSession.videoProperties(id, 0)[4].value, 1);
        testSession.autoKey = false;
    }
    function test_viewer_candidate_commits_once_or_cancels() {
        testSession.autoKey = true;
        testSession.requestSeekUs(5000000);
        const id = testSession.selectedClipDetails.id;
        const start = testSession.selectedClipDetails.startUs;
        verify(testSession.beginVideoTransformDrag(id));
        testSession.updateVideoTransformDrag(0.1, 0.2, 0.5, 30);
        testSession.updateVideoTransformDrag(0.25, 0.2, 0.5, 30);
        compare(testSession.videoPreviewActive, true);
        compare(testSession.videoProperties(id, 5000000)[0].value, 0);
        testSession.cancelClipDrag();
        compare(testSession.videoPreviewActive, false);
        compare(testSession.videoProperties(id, 5000000)[0].animated, false);
        verify(testSession.beginVideoTransformDrag(id));
        testSession.updateVideoTransformDrag(0.25, 0.2, 0.5, 30);
        verify(testSession.commitClipDrag());
        compare(testSession.selectedClipDetails.startUs, start);
        compare(testSession.videoProperties(id, 5000000)[0].value, 0.25);
        testSession.undo();
        compare(testSession.videoProperties(id, 5000000)[0].animated, false);
        testSession.autoKey = false;
    }
    function test_supported_fade_options_are_enabled_and_activated_by_pointer() {
        const inspector = createTemporaryObject(inspectorComponent, tests);
        findChild(inspector, "editDetailsTimingToggle").forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
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
