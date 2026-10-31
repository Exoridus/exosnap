import QtQuick
import QtQuick.Controls
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: tests
    name: "EditInspectorCrossfade"
    when: windowShown
    width: 1280
    height: 800
    visible: true
    Component {
        id: pageComponent
        EditPage {
            width: tests.width
            height: tests.height
            session: testSession
            timeline: testTimeline
            player: testPlayer
            exporter: testPageExporter
            recordings: testPageRecordings
            shell: testPageShell
            mediaPinPreferred: false
            detailsPinPreferred: false
        }
    }
    function init() {
        testSession.cancelClipDrag();
        while (testSession.canUndo) testSession.undo();
        testSession.appendAsset(testSession.media[0].id);
        testSession.requestSeekUs(50000000);
        testSession.splitSelected();
    }
    function cleanup() {
        testSession.cancelClipDrag();
        while (testSession.canUndo) testSession.undo();
        testSession.linkedSelection = true;
    }
    function test_inspector_applies_crossfade_around_cut_immediately_without_moving_surrounding_content() {
        const outgoing = testSession.visibleClipsUs(0, 100000000).find(row => row.video && row.startUs === 0);
        testSession.clearSelection();
        testSession.selectClip(outgoing.id);
        verify(testSession.canCrossfadeAtCut(outgoing.id));
        const geometry = () => testSession.visibleClipsUs(0, 100000000).map(row => [row.id, row.startUs, row.sourceInUs, row.sourceOutUs]);
        const before = geometry();
        const page = createTemporaryObject(pageComponent, tests);
        page.toggleDetails();
        const action = findChild(page, "editInspectorCrossfadeAtCut");
        verify(!!action, "The contextual Inspector exposes the valid around-cut transition action");
        verify(action.visible);
        mouseClick(action);
        compare(testSession.dragActive, false, "A valid command leaves no pending candidate to confirm");
        compare(testSession.transitions.length, 2);
        compare(testSession.durationUs, 100000000);
        verify(testSession.canUndo);
        const after = geometry();
        verify(JSON.stringify(after) !== JSON.stringify(before));
        testSession.undo();
        compare(testSession.transitions.length, 0);
        compare(geometry(), before);
        testSession.redo();
        compare(testSession.transitions.length, 2);
        compare(testSession.durationUs, 100000000);
        compare(geometry(), after);
    }
    function test_keyboard_focus_reveals_offscreen_inspector_field() {
        const page = createTemporaryObject(pageComponent, tests, {height: 600});
        page.toggleDetails();
        const inspector = findChild(page, "editInspector");
        const gain = findChild(page, "editDetailsMultiGain");
        verify(!!inspector && !!gain && gain.visible);
        inspector.scrollToHome();
        gain.forceActiveFocus(Qt.TabFocusReason);
        waitForPolish(page);
        const point = gain.mapToItem(inspector.flickable, 0, 0);
        verify(point.y >= 0 && point.y + gain.height <= inspector.flickable.height,
               "The focused numeric field remains visible in the short Inspector");
    }
    function test_standalone_fade_curve_controls_name_the_target_and_preserve_duration() {
        const clip = testSession.visibleClipsUs(0, 100000000).find(row => row.video);
        testSession.linkedSelection = false;
        testSession.clearSelection();
        testSession.selectClip(clip.id);
        verify(testSession.applyTimeValue(clip.id, "fadeIn", 1000000));
        const page = createTemporaryObject(pageComponent, tests);
        page.toggleDetails();
        const curve = findChild(page, "editDetailsFadeInCurve");
        verify(!!curve && curve.visible, "The selected video fade has an evaluator-backed curve selector");
        compare(curve.Accessible.name, "Video fade in curve");
        curve.forceActiveFocus(Qt.TabFocusReason);
        waitForRendering(page);
        curve.popup.open();
        tryVerify(() => curve.popup.contentItem.itemAtIndex(2) !== null);
        const option = curve.popup.contentItem.itemAtIndex(2);
        verify(option.enabled, "Supported curve options must be selectable through the real popup");
        waitForRendering(page);
        mouseClick(option, option.width / 2, option.height / 2);
        compare(testSession.selectedClipDetails.fadeInCurve, 2);
        compare(testSession.selectedClipDetails.fadeInUs, 1000000);
        const bias = findChild(page, "editDetailsFadeInBias");
        verify(!!bias && bias.visible);
        bias.forceActiveFocus(Qt.TabFocusReason);
        bias.selectAll();
        keyClick(Qt.Key_2);
        keyClick(Qt.Key_5);
        keyClick(Qt.Key_Return);
        compare(testSession.selectedClipDetails.fadeInBias, 0.25);
        verify(testSession.applyTimeValue(clip.id, "fadeIn", 2000000));
        compare(testSession.selectedClipDetails.fadeInCurve, 2);
        compare(testSession.selectedClipDetails.fadeInBias, 0.25);
        compare(testSession.selectedClipDetails.fadeInUs, 2000000);
    }
    function test_export_span_options_are_selectable_through_the_real_popup() {
        const page = createTemporaryObject(pageComponent, tests);
        page.openExport();
        const span = findChild(page, "editExportSpan");
        verify(!!span);
        waitForRendering(page);
        span.popup.open();
        tryVerify(() => span.popup.contentItem.itemAtIndex(2) !== null);
        const option = span.popup.contentItem.itemAtIndex(2);
        verify(option.enabled, "Supported export spans must not be disabled by missing option capability data");
        waitForRendering(page);
        mouseClick(option, option.width / 2, option.height / 2);
        compare(testPageExporter.spanKey, "result");
        page.openExport();
        compare(page.detailsOpen, false);
    }
    function controlNamed(item, name) {
        if (item.Accessible.name === name)
            return item;
        for (const child of item.children || []) {
            const result = controlNamed(child, name);
            if (result)
                return result;
        }
        return null;
    }
    function test_invalid_junction_bias_draft_blocks_selection_and_escape_restores_it() {
        const clip = testSession.visibleClipsUs(0, 100000000).find(row => row.video);
        verify(testSession.applyCrossfade(clip.id, 500));
        testSession.selectTransition(clip.id);
        testSession.setJunctionCurve(2, 0);
        const original = testSession.selectedJunctionDetails.id;
        const page = createTemporaryObject(pageComponent, tests);
        page.toggleDetails();
        const bias = controlNamed(findChild(page, "editInspector"), "Junction curve bias");
        verify(!!bias && bias.visible);
        verify(typeof bias.commitPendingDraft === "function", "Bias uses the same captured numeric draft protocol as other fields");
        bias.forceActiveFocus(Qt.TabFocusReason);
        bias.selectAll();
        keyClick(Qt.Key_Plus);
        verify(!testSession.setEditRangeUs(1000000, 2000000, testSession.tracks.map(row => row.id)));
        compare(testSession.selectedJunctionDetails.id, original);
        compare(bias.text, "+");
        verify(bias.errorText.length > 0);
        keyClick(Qt.Key_Escape);
        compare(bias.text, "0");
        compare(testSession.selectedJunctionDetails.bias, 0);
    }
    function test_fit_clips_is_reachable_from_the_toolbar_overflow() {
        const page = createTemporaryObject(pageComponent, tests);
        const action = findChild(page, "editFitClips");
        verify(!!action, "Occupied content fitting remains reachable from the toolbar menu");
        const strip = findChild(page, "editClipTimeline");
        strip.view.setVisibleInterval(1000000, 2000000);
        const before = strip.view.endUs - strip.view.startUs;
        action.triggered();
        verify(strip.view.endUs - strip.view.startUs > before);
        compare(testSession.positionUs, 50000000);
    }
    function test_contextual_inspection_stays_open_and_focuses_the_requested_trim_field() {
        const clip = testSession.visibleClipsUs(0, 100000000).find(row => row.video);
        verify(testSession.inspectClip(clip.id));
        const page = createTemporaryObject(pageComponent, tests, {height: 600});
        const strip = findChild(page, "editClipTimeline");
        strip.inspectorOpenRequested("");
        tryCompare(page, "detailsOpen", true);
        strip.inspectorOpenRequested("");
        compare(page.detailsOpen, true);
        for (const field of ["sourceIn", "sourceOut"]) {
            strip.inspectorOpenRequested(field);
            const editor = findChild(page, field === "sourceIn" ? "editDetailsSourceIn" : "editDetailsSourceOut");
            tryCompare(editor, "activeFocus", true);
            const inspector = findChild(page, "editInspector");
            const point = editor.mapToItem(inspector.flickable, 0, 0);
            verify(point.y >= 0 && point.y + editor.height <= inspector.flickable.height);
        }
        compare(testSession.selectedClipIds, [clip.id]);
    }
}
