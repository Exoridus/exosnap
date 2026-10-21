import QtQuick
import QtQuick.Controls
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: tests
    name: "EditCacheIntegration"
    when: windowShown
    visible: true
    width: 1000
    height: 400
    Component { id: timelineComponent; EditTimeline { session: testSession; player: testPlayer; width: 960; height: 260 } }
    function init() {
        testSession.cancelClipDrag();
        testSession.cancelRangeGesture();
        while (testSession.canUndo) testSession.undo();
        testSession.appendAsset(testSession.media[0].id);
        testSession.snapping = false;
        testSession.linkedSelection = true;
        testSession.requestSeekUs(0);
    }
    function cleanup() { testSession.cancelClipDrag(); testSession.cancelRangeGesture(); }
    function strip() {
        const item = createTemporaryObject(timelineComponent, tests);
        verify(!!item);
        item.view.setVisibleInterval(0, 20000000);
        return item;
    }
    function video() { return testSession.visibleClipsUs(0, 100000000).filter(row => row.video)[0]; }
    function waitVisual(item) { waitForRendering(item); wait(0); }
    function test_clip_candidate_keeps_delegate_identity_across_preview_and_pan() {
        const item = strip();
        const row = video();
        testSession.beginClipDrag(row.id, -1, true);
        testSession.updateClipDragUs(1000000, row.trackId, -1, false, 0);
        verify(testSession.dragActive && testSession.dragValid);
        waitVisual(item);
        const ghost = findChild(item, "editDragGhost" + row.id);
        verify(!!ghost);
        testSession.updateClipDragUs(2000000, row.trackId, -1, false, 0);
        waitVisual(item);
        compare(findChild(item, "editDragGhost" + row.id), ghost, "Preview data updates must retain the candidate object");
        item.view.setVisibleInterval(1000000, 21000000);
        waitVisual(item);
        compare(findChild(item, "editDragGhost" + row.id), ghost, "A visible candidate must survive pan");
    }
    function makeSpacer() {
        const row = video();
        testSession.moveClip(row.id, 20000, false);
        testSession.selectGapUs(row.trackId, 10000000);
        testSession.copyBlankDuration();
        testSession.requestSeekUs(0);
        testSession.pasteSpacer();
        compare(testSession.spacers.length, 1);
        return testSession.spacers[0];
    }
    function test_spacer_candidate_and_committed_row_keep_delegate_identity() {
        const spacer = makeSpacer();
        const item = strip();
        waitVisual(item);
        const committed = findChild(item, "editSpacer" + spacer.id);
        verify(!!committed);
        testSession.beginSpacerDrag(spacer.id, 1);
        testSession.updateClipDragUs(-1000000, 0, -1, false, 0);
        verify(testSession.dragActive && testSession.dragValid);
        waitVisual(item);
        const ghost = findChild(item, "editSpacerGhost" + spacer.id);
        verify(!!ghost);
        testSession.updateClipDragUs(-2000000, 0, -1, false, 0);
        waitVisual(item);
        compare(findChild(item, "editSpacerGhost" + spacer.id), ghost);
        verify(testSession.commitClipDrag());
        waitVisual(item);
        compare(findChild(item, "editSpacer" + spacer.id), committed);
    }
    function test_pressed_spacer_keeps_its_grabber_when_navigation_culls_other_rows() {
        const spacer = makeSpacer();
        const item = strip();
        waitVisual(item);
        const committed = findChild(item, "editSpacer" + spacer.id);
        verify(!!committed);
        mousePress(committed, 20, committed.height / 2);
        try {
            item.view.setVisibleInterval(50000000, 70000000);
            waitVisual(item);
            compare(findChild(item, "editSpacer" + spacer.id), committed,
                "An existing pressed spacer owns the grab until release or cancellation");
        } finally {
            mouseRelease(item, item.width / 2, item.height / 2);
        }
    }
    function test_junction_candidate_keeps_its_delegate_across_boundary_updates() {
        testSession.requestSeekUs(50000000);
        testSession.splitSelected();
        const row = testSession.visibleClipsUs(0, 100000000).filter(value => value.video && value.startUs === 0)[0];
        verify(testSession.previewCrossfadeAtCut(row.id, 500000));
        verify(testSession.commitClipDrag());
        const item = strip();
        item.view.setVisibleInterval(48000000, 52000000);
        testSession.beginJunctionDrag(row.id, 1);
        testSession.updateClipDragUs(-50000, 0, -1, false, 0);
        verify(testSession.dragActive && testSession.dragValid);
        waitVisual(item);
        const ghost = findChild(item, "editDragJunctionGhost");
        verify(!!ghost);
        testSession.updateClipDragUs(-100000, 0, -1, false, 0);
        waitVisual(item);
        compare(findChild(item, "editDragJunctionGhost"), ghost);
    }
    function test_playhead_relevance_updates_without_rebuilding_tile_images() {
        verify(testPreviewWorkload.prepare(1));
        const item = createTemporaryObject(timelineComponent, tests, {session: testPreviewWorkload.session, thumbnails: testPreviewWorkload.timeline});
        item.view.setVisibleInterval(0, 20000000);
        const row = testPreviewWorkload.session.visibleClipsUs(0, 20000000)[0];
        waitVisual(item);
        const clip = findChild(item, "editClip" + row.id);
        verify(!!clip);
        const before = testPreviewWorkload.thumbnailQueries();
        testPreviewWorkload.session.requestSeekUs(5300000);
        waitVisual(item);
        compare(clip.playheadSourceUs, row.sourceInUs + 5300000 - row.startUs);
        verify(String(clip.mediaRelevance).includes("5300000"), "The published relevance must include the live playhead");
        compare(testPreviewWorkload.thumbnailQueries(), before, "Relevance alone must not rebuild tile rows/images");
    }
    function test_active_trim_relevance_follows_the_captured_candidate_without_reloading_images() {
        verify(testPreviewWorkload.prepare(1));
        const item = createTemporaryObject(timelineComponent, tests, {session: testPreviewWorkload.session, thumbnails: testPreviewWorkload.timeline});
        item.view.setVisibleInterval(0, 20000000);
        const row = testPreviewWorkload.session.visibleClipsUs(0, 20000000)[0];
        waitVisual(item);
        const clip = findChild(item, "editClip" + row.id);
        verify(!!clip);
        const y = clip.trackBounds.headerHeight + clip.bodyHeight / 2;
        mousePress(clip, 1, y);
        mouseMove(clip, 55, y);
        tryCompare(testPreviewWorkload.session, "dragActive", true);
        waitVisual(item);
        const candidate = testPreviewWorkload.session.dragClips.find(value => value.id === row.id);
        verify(!!candidate);
        verify(clip.trimSourceUs > 0);
        compare(clip.trimSourceUs, candidate.sourceInUs);
        verify(String(clip.mediaRelevance).includes(String(candidate.sourceInUs)));
        const queries = testPreviewWorkload.thumbnailQueries();
        mouseMove(clip, 75, y);
        waitVisual(item);
        const changed = testPreviewWorkload.session.dragClips.find(value => value.id === row.id);
        compare(clip.trimSourceUs, changed.sourceInUs);
        compare(testPreviewWorkload.thumbnailQueries(), queries);
        testPreviewWorkload.session.cancelClipDrag();
        mouseRelease(clip, 75, y);
    }
}
