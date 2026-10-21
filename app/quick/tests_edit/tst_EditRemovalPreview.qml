import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: tests
    name: "EditRemovalPreview"
    when: windowShown
    width: 640
    height: 520
    visible: true

    Component {
        id: timelineComponent
        EditTimeline { session: testSession; player: testPlayer; width: 600; height: 240 }
    }
    function init() {
        testSession.cancelClipDrag();
        testSession.cancelRangeGesture();
        testSession.snapping = false;
        testSession.ripple = false;
        testSession.autoCrossfade = true;
        testSession.linkedSelection = true;
        testSession.clearSelection();
        while (testSession.canUndo) testSession.undo();
        testSession.appendAsset(testSession.media[0].id);
        for (const track of testSession.tracks)
            testSession.trackLayout.setTrackHeight(track.id, 64);
    }
    function test_removed_whole_clips_remain_visible_as_original_donors_until_commit() {
        const strip = createTemporaryObject(timelineComponent, tests);
        verify(!!strip);
        strip.view.setVisibleInterval(0, 10000000);
        waitForRendering(strip);
        const id = testSession.selectedClip;
        const clip = findChild(strip, "editClip" + id);
        verify(!!clip);
        const duration = testSession.durationUs;
        const tracks = testSession.tracks.map(row => row.id);
        verify(testSession.setEditRangeUs(0, duration, tracks));
        verify(testSession.previewRemoveTimeOnAllTracks());
        verify(testSession.dragValid);
        compare(testSession.dragClips.length, 0);
        waitForRendering(strip);
        verify(clip.donor);
        compare(testSession.durationUs, duration);
        testSession.cancelClipDrag();
        waitForRendering(strip);
        verify(!clip.donor);
    }
    function test_junction_only_candidate_marks_the_original_without_changed_clips() {
        const outgoing = testSession.visibleClipsUs(0, 100000000).find(row => row.video).id;
        testSession.selectClip(outgoing);
        testSession.requestSeekUs(50000000);
        testSession.splitSelected();
        verify(testSession.previewCrossfadeAtCut(outgoing));
        verify(testSession.commitClipDrag());
        const junction = testSession.transitions.find(row => row.outgoing === outgoing);
        verify(!!junction);
        const strip = createTemporaryObject(timelineComponent, tests);
        verify(!!strip);
        strip.view.setVisibleInterval(48000000, 52000000);
        waitForRendering(strip);
        const canvas = findChild(strip, "editJunctionCurves" + junction.id);
        verify(!!canvas);
        const overlay = canvas.parent;
        testSession.beginJunctionDrag(outgoing, 0);
        testSession.updateClipDragUs(50000, 0, -1, false, 0);
        verify(testSession.dragActive && testSession.dragValid);
        compare(testSession.dragClips.length, 0);
        waitForRendering(strip);
        verify(overlay.donor);
        testSession.cancelClipDrag();
        waitForRendering(strip);
        verify(!overlay.donor);
    }
    function test_trimmed_spacers_mark_the_original_without_changed_clips() {
        const row = testSession.visibleClipsUs(0, 100000000).find(value => value.video);
        testSession.clearSelection();
        testSession.selectClip(row.id);
        testSession.moveClip(row.id, 2000, false);
        testSession.selectGapUs(row.trackId, 1000000);
        testSession.copyBlankDuration();
        testSession.requestSeekUs(0);
        testSession.pasteSpacer();
        compare(testSession.spacers.length, 1);
        const spacer = testSession.spacers[0];
        const strip = createTemporaryObject(timelineComponent, tests);
        verify(!!strip);
        strip.view.setVisibleInterval(0, 10000000);
        waitForRendering(strip);
        const original = findChild(strip, "editSpacer" + spacer.id);
        verify(!!original);
        testSession.beginSpacerDrag(spacer.id, 1);
        testSession.updateClipDragUs(-500000, 0, -1, false, 0);
        verify(testSession.dragActive && testSession.dragValid);
        compare(testSession.dragClips.length, 0);
        compare(testSession.dragSpacers.length, 1);
        waitForRendering(strip);
        verify(original.donor);
        compare(original.opacity, 0.3);
        testSession.cancelClipDrag();
        waitForRendering(strip);
        verify(!original.donor);
        compare(original.opacity, 1);
    }
}