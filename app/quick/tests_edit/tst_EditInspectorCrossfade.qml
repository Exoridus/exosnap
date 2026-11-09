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
        testSession.autoKey = false;
        while (testSession.canUndo) testSession.undo();
        testSession.appendAsset(testSession.media[0].id);
        testSession.requestSeekUs(50000000);
        testSession.splitSelected();
    }
    function cleanup() {
        testSession.cancelClipDrag();
        testSession.autoKey = false;
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
        findChild(page, "editInspector").revealItem(action);
        waitForRendering(page);
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
        const timing = findChild(page, "editDetailsTimingToggle");
        timing.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
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
    function test_fit_selection_menu_matches_keyboard_and_restores_one_view() {
        const page = createTemporaryObject(pageComponent, tests);
        const strip = findChild(page, "editClipTimeline");
        const action = findChild(page, "editFitSelection");
        const selected = testSession.visibleClipsUs(0, 100000000).filter(row => row.startUs === 0).map(row => row.id);
        for (const mode of ["clip", "clips", "range"]) {
            testSession.clearSelection();
            if (mode === "range")
                verify(testSession.setEditRangeUs(10000000, 20000000, testSession.tracks.map(row => row.id)));
            else {
                testSession.linkedSelection = mode !== "clip";
                testSession.selectClip(selected[0]);
            }
            strip.view.setVisibleInterval(60000000, 70000000);
            const before = [strip.view.startUs, strip.view.endUs];
            const depth = strip.view.historyDepth;
            verify(action.enabled);
            action.triggered();
            const fitted = [strip.view.startUs, strip.view.endUs];
            compare(strip.view.historyDepth, depth + 1);
            strip.forceActiveFocus();
            keyClick(Qt.Key_Backspace);
            compare([strip.view.startUs, strip.view.endUs], before);
            keyClick(Qt.Key_Z);
            compare([strip.view.startUs, strip.view.endUs], fitted);
            keyClick(Qt.Key_Backspace);
            compare([strip.view.startUs, strip.view.endUs], before);
        }
        testSession.clearEditRange();
        testSession.clearSelection();
        verify(!action.enabled);
    }
    function test_crossfade_without_a_neighbor_is_disabled() {
        const incoming = testSession.visibleClipsUs(0, 100000000).find(row => row.video && row.startUs > 0);
        verify(testSession.inspectClip(incoming.id));
        const page = createTemporaryObject(pageComponent, tests);
        page.toggleDetails();
        verify(!findChild(page, "editInspectorCrossfadeAtCut").enabled);
    }
    function test_junction_overlay_fits_content_and_escape_restores_canvas_focus() {
        const outgoing = testSession.visibleClipsUs(0, 100000000).find(row => row.video && row.startUs === 0);
        testSession.selectTransition(outgoing.id);
        const page = createTemporaryObject(pageComponent, tests, {width: 860, height: 700});
        const strip = findChild(page, "editClipTimeline");
        strip.forceActiveFocus();
        keyClick(Qt.Key_D);
        const host = findChild(page, "editDetailsPanelLoader");
        tryVerify(() => host.height < host.parent.height);
        verify(host.height >= findChild(page, "editInspector").naturalContentHeight);
        const selected = testSession.selectedTransition;
        keyClick(Qt.Key_Escape);
        verify(!page.detailsOpen);
        compare(testSession.selectedTransition, selected);
        verify(strip.activeFocus);
    }
    function test_gain_point_keyboard_add_time_gain_undo_redo_and_delete() {
        const audio = testSession.visibleClipsUs(0, 100000000).find(row => !row.video && row.startUs === 0);
        verify(testSession.inspectClip(audio.id));
        testSession.requestSeekUs(5000000);
        const page = createTemporaryObject(pageComponent, tests);
        const strip = findChild(page, "editClipTimeline");
        strip.forceActiveFocus();
        keyClick(Qt.Key_D);
        const add = findChild(page, "editAddGainPoint");
        add.forceActiveFocus(Qt.TabFocusReason);
        verify(add.enabled);
        keyClick(Qt.Key_Space);
        const point = () => testSession.selectedClipDetails.gainKeys.find(key => key.sourceUs === 6000000);
        verify(testSession.selectedClipDetails.gainKeys.some(key => key.sourceUs === 5000000));
        const time = findChild(page, "editGainPointTime");
        time.changeFormat(1);
        time.forceActiveFocus(Qt.TabFocusReason);
        time.selectAll();
        keyClick("6");
        keyClick(Qt.Key_Right);
        compare(testSession.positionUs, 5000000);
        keyClick(Qt.Key_Return);
        verify(!!point());
        verify(!testSession.dragActive);
        const gain = findChild(page, "editGainPointGain");
        gain.forceActiveFocus(Qt.TabFocusReason);
        gain.selectAll();
        keyClick("-");
        keyClick("9");
        keyClick(Qt.Key_Return);
        compare(point().gainDb, -9);
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(point().gainDb, 0);
        keyClick(Qt.Key_Z, Qt.ControlModifier | Qt.ShiftModifier);
        compare(point().gainDb, -9);
        gain.forceActiveFocus(Qt.TabFocusReason);
        gain.selectAll();
        keyClick("-");
        keyClick(Qt.Key_Return);
        verify(gain.errorText.length > 0);
        compare(point().gainDb, -9);
        keyClick(Qt.Key_Escape);
        verify(page.detailsOpen);
        compare(point().gainDb, -9);
        const remove = findChild(page, "editRemoveGainPoint");
        remove.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        verify(!point());
        strip.forceActiveFocus();
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(point().gainDb, -9);
    }
    function test_gain_point_numeric_edit_refuses_invalid_time_without_leaving_a_gesture() {
        const audio = testSession.visibleClipsUs(0, 100000000).find(row => !row.video && row.startUs === 0);
        verify(testSession.inspectClip(audio.id));
        verify(testSession.addGainKey(audio.id, 5000000));
        const page = createTemporaryObject(pageComponent, tests);
        page.toggleDetails();
        const inspector = findChild(page, "editInspector");
        const before = JSON.stringify(testSession.selectedClipDetails.gainKeys);
        verify(!inspector.editGainPoint(audio.id, 5000000, -1000000, -9));
        verify(!testSession.dragActive);
        compare(JSON.stringify(testSession.selectedClipDetails.gainKeys), before);
        verify(inspector.editGainPoint(audio.id, 5000000, 6000001, -9));
        compare(inspector.selectedGainKey.sourceUs, 6000000);
        compare(inspector.selectedGainKey.gainDb, -9);
    }
    function test_visual_inspection_routes_expand_the_requested_section() {
        const clip = testSession.visibleClipsUs(0, 100000000).find(row => row.video);
        verify(testSession.inspectClip(clip.id));
        const page = createTemporaryObject(pageComponent, tests);
        verify(!page.applyHarnessView({inspectorField: "animation"}));
        page.toggleDetails();
        const inspector = findChild(page, "editInspector");
        compare(inspector.animationExpanded, false);
        compare(inspector.timingExpanded, false);
        verify(page.applyHarnessView({inspectorField: "animation"}));
        compare(inspector.animationExpanded, true);
        verify(page.applyHarnessView({inspectorField: "fadeIn"}));
        compare(inspector.timingExpanded, true);
        const fade = findChild(page, "editDetailsFadeIn");
        tryCompare(fade, "activeFocus", true);
        waitForPolish(page);
        const point = fade.mapToItem(inspector.flickable, 0, 0);
        verify(point.y >= 0 && point.y + fade.height <= inspector.flickable.height);
        verify(!page.applyHarnessView({inspectorField: "unknown"}));
        keyClick(Qt.Key_Escape);
    }
    function test_focused_gain_point_opens_its_numeric_editor_without_moving_playhead() {
        const audio = testSession.visibleClipsUs(0, 100000000).find(row => !row.video && row.startUs === 0);
        verify(testSession.addGainKey(audio.id, 5000000));
        const page = createTemporaryObject(pageComponent, tests);
        const strip = findChild(page, "editClipTimeline");
        strip.view.setVisibleInterval(0, 10000000);
        waitForRendering(strip);
        const point = findChild(strip, "editGainPoint" + audio.id + "_5000000");
        verify(!!point && point.visible);
        verify(point.Accessible.name.includes("dB"));
        const position = testSession.positionUs;
        point.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Return);
        tryCompare(page, "detailsOpen", true);
        const time = findChild(page, "editGainPointTime");
        tryCompare(time, "activeFocus", true);
        compare(time.valueUs, 5000000);
        compare(testSession.positionUs, position);
        keyClick(Qt.Key_Escape);
    }
    function test_crossfade_availability_uses_committed_duration_with_short_source_handles() {
        while (testSession.canUndo) testSession.undo();
        testSession.appendAsset(testSession.media[0].id);
        testSession.linkedSelection = false;
        const outgoing = testSession.visibleClipsUs(0, 100000000).find(row => row.video);
        verify(testSession.applyTimeValue(outgoing.id, "sourceOut", 99900000));
        testSession.appendAsset(testSession.media[0].id);
        const incoming = testSession.visibleClipsUs(0, 200000000).find(row => row.video && row.id !== outgoing.id);
        verify(testSession.applyTimeValue(incoming.id, "sourceIn", 100000));
        verify(testSession.applyTimeValue(incoming.id, "clipStart", 99900000));
        verify(testSession.inspectClip(outgoing.id));
        const geometry = () => testSession.visibleClipsUs(0, 200000000).map(row => [row.id, row.startUs, row.sourceInUs, row.sourceOutUs]);
        const before = geometry();
        const revision = testEditHarness.timelineCheckpoint().revision;
        verify(!testSession.canCrossfadeAtCut(outgoing.id));
        verify(!testSession.canCrossfadeAtCut(outgoing.id, 1000000));
        verify(testSession.canCrossfadeAtCut(outgoing.id, 100000));
        compare(geometry(), before);
        compare(testEditHarness.timelineCheckpoint().revision, revision);
        const page = createTemporaryObject(pageComponent, tests);
        page.toggleDetails();
        const action = findChild(page, "editInspectorCrossfadeAtCut");
        const duration = findChild(page, "editInspectorCrossfadeDuration");
        verify(!action.enabled);
        duration.changeFormat(1);
        duration.forceActiveFocus(Qt.TabFocusReason);
        duration.selectAll();
        for (const character of "0.1") keyClick(character);
        verify(!action.enabled, "Uncommitted text does not change command availability");
        keyClick(Qt.Key_Return);
        verify(action.enabled);
        duration.forceActiveFocus(Qt.TabFocusReason);
        duration.selectAll();
        keyClick("1");
        keyClick(Qt.Key_Return);
        verify(!action.enabled);
        duration.forceActiveFocus(Qt.TabFocusReason);
        duration.selectAll();
        for (const character of "0.1") keyClick(character);
        keyClick(Qt.Key_Return);
        verify(action.enabled);
        action.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        compare(testSession.transitions.length, 1);
        compare(testEditHarness.timelineCheckpoint().revision, revision + 1);
        const after = geometry();
        testSession.undo();
        compare(testSession.transitions.length, 0);
        compare(geometry(), before);
        testSession.redo();
        compare(testSession.transitions.length, 1);
        compare(geometry(), after);
    }
    function test_crossfade_availability_refreshes_when_link_scope_changes() {
        const outgoing = testSession.visibleClipsUs(0, 100000000).find(row => row.video && row.startUs === 0);
        const audio = testSession.tracks.find(row => !row.video);
        verify(testSession.inspectClip(outgoing.id));
        const page = createTemporaryObject(pageComponent, tests);
        page.toggleDetails();
        const action = findChild(page, "editInspectorCrossfadeAtCut");
        verify(action.enabled);
        verify(testSession.setTrackLocked(audio.id, true));
        verify(!action.enabled);
        testSession.linkedSelection = false;
        compare(testSession.selectedClip, outgoing.id);
        verify(action.enabled);
        testSession.linkedSelection = true;
        compare(testSession.selectedClip, outgoing.id);
        verify(!action.enabled);
    }
    function test_collapsed_animation_keeps_auto_key_mode_and_key_state_visible() {
        const clip = testSession.visibleClipsUs(0, 100000000).find(row => row.video && row.startUs === 0);
        verify(testSession.inspectClip(clip.id));
        testSession.requestSeekUs(5000000);
        const page = createTemporaryObject(pageComponent, tests);
        const strip = findChild(page, "editClipTimeline");
        strip.forceActiveFocus();
        keyClick(Qt.Key_D);
        const inspector = findChild(page, "editInspector");
        const autoKey = findChild(page, "editDetailsAutoKey");
        const value = findChild(page, "editVideoProperty0");
        const marker = findChild(page, "editVideoPropertyState0");
        compare(inspector.animationExpanded, false);
        verify(autoKey.visible);
        verify(!autoKey.checked);
        keyClick(Qt.Key_K);
        verify(autoKey.checked);
        compare(inspector.animationExpanded, false);
        const disclosure = findChild(page, "editDetailsAnimationToggle");
        disclosure.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        compare(inspector.animationExpanded, true);
        verify(autoKey.checked);
        keyClick(Qt.Key_Space);
        compare(inspector.animationExpanded, false);
        verify(autoKey.checked);
        value.forceActiveFocus(Qt.TabFocusReason);
        value.selectAll();
        for (const character of "0.25") keyClick(character);
        keyClick(Qt.Key_Return);
        compare(testSession.videoProperties(clip.id, 5000000)[0].value, 0.25);
        verify(testSession.videoProperties(clip.id, 5000000)[0].key);
        verify(marker.visible);
        compare(marker.kind, ExoGlyph.KeyframeFilled);
        testSession.requestSeekUs(2500000);
        verify(marker.visible);
        compare(marker.kind, ExoGlyph.Keyframe);
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        verify(!testSession.videoProperties(clip.id, 5000000)[0].animated);
        verify(!marker.visible);
        verify(autoKey.checked);
        autoKey.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        verify(!autoKey.checked);
        verify(!testSession.autoKey);
        compare(inspector.animationExpanded, false);
        value.forceActiveFocus(Qt.TabFocusReason);
        value.selectAll();
        keyClick(Qt.Key_K);
        verify(!testSession.autoKey, "The numeric draft owns its text keys");
        compare(testSession.positionUs, 2500000);
        keyClick(Qt.Key_Escape);
    }
}
