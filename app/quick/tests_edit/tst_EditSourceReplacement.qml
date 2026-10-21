import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: testCase
    name: "EditSourceReplacement"
    width: 800
    height: 640
    visible: true
    when: windowShown
    SignalSpy { id: workspaceChanges; target: testPageSession; signalName: "workspaceChanged" }
    Component {
        id: browserComponent
        EditSourceBrowser {
            session: testPageSession
            recordings: testPageRecordings
            timeline: testPageTimeline
            width: 320
            height: 560
        }
    }
    function prepare(adopted) {
        const fixture = testEditHarness.prepareSourceAudioChoice();
        verify(fixture.ready, fixture.reason || "Real media fixture is unavailable");
        testPageSession.cancelSourceAudioChoice();
        const browser = createTemporaryObject(browserComponent, testCase);
        verify(!!browser);
        const row = testPageSession.media[0];
        testPageSession.beginSourceReplacement(fixture.assetId, "file:///" + row.path.replace(/\\/g, "/"), adopted === undefined ? true : adopted);
        tryVerify(() => testPageSession.sourceReplacement.active && !testPageSession.sourceReplacement.busy, 10000);
        workspaceChanges.clear();
        return {browser: browser, fixture: fixture};
    }
    function cleanup() {
        testEditHarness.clearSourceAudioChoiceFixture();
    }
    function test_real_replacement_review_requires_explicit_apply_and_named_audio() {
        const setup = prepare();
        const dialog = findChild(setup.browser, "editSourceReplacement");
        verify(!!dialog, "Project media needs a source replacement review dialog");
        tryCompare(dialog, "opened", true);
        const draft = testPageSession.sourceReplacement;
        verify(dialog.title.indexOf(draft.name) >= 0);
        const apply = findChild(dialog, "editSourceReplacementApply");
        verify(!!apply);
        const first = findChild(dialog.contentItem, "editSourceReplacementTrack-" + draft.tracks[0].id);
        verify(!!first);
        verify(first.Accessible.name.length > 0);
        verify(dialog.contentItem.revealItem(first));
        mouseClick(first);
        tryVerify(() => testPageSession.sourceReplacement.tracks[0].selected);
        verify(apply.enabled);
        compare(workspaceChanges.count, 0);
        mouseClick(apply);
        tryVerify(() => !testPageSession.sourceReplacement.active, 10000);
        tryCompare(dialog, "visible", false);
    }
    function test_adoption_switch_keeps_review_open_and_requires_unknown_overlap_confirmation() {
        const setup = prepare(false);
        const dialog = findChild(setup.browser, "editSourceReplacement");
        verify(!!dialog);
        tryCompare(dialog, "opened", true);
        const originalUrl = testPageSession.sourceReplacement.url;
        const adopt = findChild(dialog, "editSourceReplacementAdopt");
        verify(!adopt.checked);
        verify(waitForPolish(testCase.Window.window));
        mouseClick(adopt);
        tryVerify(() => testPageSession.sourceReplacement.adoptChangedBytes && !testPageSession.sourceReplacement.busy, 10000);
        verify(dialog.opened);
        compare(testPageSession.sourceReplacement.url, originalUrl);
        for (const row of testPageSession.sourceReplacement.tracks)
            testPageSession.setSourceReplacementTrack(row.id, true);
        const overlap = findChild(dialog, "editSourceReplacementOverlap");
        tryCompare(overlap, "visible", true);
        const apply = findChild(dialog, "editSourceReplacementApply");
        verify(!apply.enabled);
        verify(waitForPolish(testCase.Window.window));
        verify(dialog.contentItem.revealItem(overlap));
        mouseClick(overlap);
        tryVerify(() => apply.enabled);
        dialog.close();
        tryVerify(() => !testPageSession.sourceReplacement.active);
        compare(workspaceChanges.count, 0);
    }
    function test_keyboard_focus_reveals_scrolled_audio_confirmation() {
        const setup = prepare();
        const dialog = findChild(setup.browser, "editSourceReplacement");
        verify(!!dialog);
        tryCompare(dialog, "opened", true);
        for (const row of testPageSession.sourceReplacement.tracks)
            testPageSession.setSourceReplacementTrack(row.id, true);
        const overlap = findChild(dialog, "editSourceReplacementOverlap");
        tryCompare(overlap, "visible", true);
        verify(waitForPolish(testCase.Window.window));
        dialog.contentItem.scrollToHome();
        overlap.forceActiveFocus(Qt.TabFocusReason);
        tryVerify(() => {
            const view = dialog.contentItem.flickable;
            const position = overlap.mapToItem(view, 0, 0);
            return position.y >= 0 && position.y + overlap.height <= view.height;
        });
        keyClick(Qt.Key_Space);
        tryVerify(() => testPageSession.sourceReplacement.overlapConfirmed);
    }
    function test_scope_review_disables_replacement_without_discarding_its_candidate() {
        const setup = prepare();
        const dialog = findChild(setup.browser, "editSourceReplacement");
        verify(!!dialog);
        tryCompare(dialog, "opened", true);
        const originalUrl = testPageSession.sourceReplacement.url;
        const apply = findChild(dialog, "editSourceReplacementApply");
        verify(apply.enabled);
        const tracks = testPageSession.tracks;
        verify(testPageSession.setEditRangeUs(0, 500000, [tracks[0].id]));
        verify(testPageSession.previewRangeScope([tracks[tracks.length - 1].id]));
        verify(testPageSession.rangeScopeReviewActive);
        tryCompare(apply, "enabled", false);
        verify(testPageSession.sourceReplacement.active);
        compare(testPageSession.sourceReplacement.url, originalUrl);
        verify(testPageSession.sourceReplacement.reason.length > 0);
        testPageSession.cancelRangeGesture();
        tryCompare(apply, "enabled", true);
    }
    function test_source_audio_decision_waits_for_track_scope_review() {
        const fixture = testEditHarness.prepareSourceAudioChoice();
        verify(fixture.ready, fixture.reason || "Real media fixture is unavailable");
        const track = testPageSession.sourceAudioChoice.tracks[0];
        testPageSession.setSourceAudioTrack(track.id, true);
        verify(testPageSession.sourceAudioChoice.canApply);
        const tracks = testPageSession.tracks;
        verify(testPageSession.setEditRangeUs(0, 500000, [tracks[0].id]));
        verify(testPageSession.previewRangeScope([tracks[tracks.length - 1].id]));
        verify(!testPageSession.sourceAudioChoice.canApply);
        verify(!testPageSession.applySourceAudioChoice());
        verify(testPageSession.sourceAudioChoice.active);
        verify(testPageSession.sourceAudioChoice.tracks[0].selected);
        testPageSession.cancelRangeGesture();
        verify(testPageSession.sourceAudioChoice.canApply);
    }
    function test_review_uses_window_geometry_above_a_narrow_media_dock_data() {
        return [{tag: "compact", windowWidth: 860}, {tag: "wide", windowWidth: 1280}];
    }
    function test_review_uses_window_geometry_above_a_narrow_media_dock(data) {
        testCase.width = data.windowWidth;
        const setup = prepare();
        setup.browser.width = 240;
        setup.browser.height = 160;
        const dialog = findChild(setup.browser, "editSourceReplacement");
        verify(!!dialog);
        tryCompare(dialog, "opened", true);
        verify(waitForPolish(testCase.Window.window));
        verify(dialog.parent !== setup.browser);
        verify(dialog.width > setup.browser.width, "Review must use the window, not the Media dock width");
        verify(dialog.height > setup.browser.height, "Review must use the window, not the Media dock height");
        verify(dialog.x >= 0 && dialog.y >= 0);
        verify(dialog.x + dialog.width <= dialog.parent.width);
        verify(dialog.y + dialog.height <= dialog.parent.height);
        const footer = dialog.footer;
        for (const child of footer.contentItem.children)
            if (child.visible && child.text !== undefined)
                verify(child.x >= 0 && child.x + child.width <= footer.contentItem.width, "Footer actions must fit");
        dialog.close();
    }
    function test_destroying_browser_cancels_review_without_changing_the_source() {
        const setup = prepare();
        const dialog = findChild(setup.browser, "editSourceReplacement");
        verify(!!dialog, "Project media needs a source replacement review dialog");
        tryCompare(dialog, "opened", true);
        setup.browser.destroy();
        tryVerify(() => !testPageSession.sourceReplacement.active);
        compare(workspaceChanges.count, 0);
    }
}
