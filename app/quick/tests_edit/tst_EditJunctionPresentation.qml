import QtQuick
import QtQuick.Controls
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: tests
    name: "EditJunctionPresentation"
    when: windowShown
    width: 1280
    height: 600
    visible: true

    Component {
        id: timelineComponent
        EditTimeline { session: testSession; player: testPlayer; width: 1200; height: 300 }
    }
    function init() {
        testSession.cancelClipDrag();
        while (testSession.canUndo) testSession.undo();
        testSession.appendAsset(testSession.media[0].id);
        testSession.appendAsset(testSession.media[0].id);
        const outgoing = testSession.visibleClipsUs(0, 200000000).find(row => row.video);
        testSession.applyCrossfade(outgoing.id, 500);
        compare(testSession.transitions.length, 2);
        for (const track of testSession.tracks)
            testSession.trackLayout.setTrackHeight(track.id, track.video ? 64 : 44);
    }
    function cleanup() {
        testSession.cancelClipDrag();
        while (testSession.canUndo) testSession.undo();
    }
    function test_projection_filters_time_and_track_before_curve_work() {
        const video = testSession.transitions.find(row => row.video);
        const rows = testSession.visibleTransitionsUs(99600000, 99800000, [video.trackId]);
        compare(rows.length, 1);
        compare(rows[0].id, video.id);
        compare(rows[0].overlapStartUs, 99500000);
        compare(rows[0].overlapEndUs, 100000000);
        compare(testSession.visibleTransitionsUs(0, 1000000, [video.trackId]).length, 0);
    }
    function test_captured_junction_survives_viewport_and_row_culling() {
        const video = testSession.transitions.find(row => row.video);
        const audio = testSession.transitions.find(row => !row.video);
        const rows = testSession.visibleTransitionsUs(0, 1000000, [audio.trackId], true, [video.id, 999999]);
        compare(rows.length, 1);
        compare(rows[0].id, video.id);
        compare(testSession.visibleTransitionsUs(0, 1000000, [audio.trackId], true, []).length, 0);
    }
    function test_curves_use_the_evaluated_complementary_audio_law() {
        const audio = testSession.transitions.find(row => !row.video);
        let points = testSession.junctionEnvelopePoints(audio.id, 0, 1, 2);
        compare(points.incoming.length, 3);
        compare(points.outgoing.length, 3);
        fuzzyCompare(points.incoming[1], 0.5, 0.000001);
        fuzzyCompare(points.outgoing[1], 0.5, 0.000001);
        testSession.selectTransition(audio.outgoing);
        testSession.setJunctionAudioLaw(1);
        points = testSession.junctionEnvelopePoints(audio.id, 0, 1, 2);
        fuzzyCompare(points.incoming[1], Math.sqrt(0.5), 0.000001);
        fuzzyCompare(points.outgoing[1], Math.sqrt(0.5), 0.000001);
        compare(testSession.junctionEnvelopePoints(audio.id, -1, 1, 2).incoming, undefined);
    }
    function test_real_overlap_has_curves_and_hard_cut_has_no_fake_duration() {
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.view.setVisibleInterval(98500000, 101000000);
        waitForRendering(strip);
        const junction = testSession.transitions.find(row => row.video);
        const curves = findChild(strip, "editJunctionCurves" + junction.id);
        verify(!!curves, "Crossfade is drawn with complementary curves rather than an opaque label");
        const window = findChild(strip, "editJunctionWindow" + junction.id);
        verify(!!window);
        fuzzyCompare(window.width, strip.positionAtUs(junction.endUs) - strip.positionAtUs(junction.startUs), 0.5);
        testSession.selectTransition(junction.outgoing);
        testSession.removeTransition();
        wait(0);
        const line = findChild(strip, "editHardCutLine" + junction.id);
        verify(!!line);
        compare(line.width, 1);
        verify(line.visible);
        const cut = testSession.transitions.find(row => row.id === junction.id);
        compare(cut.overlapStartUs, junction.overlapStartUs);
        compare(cut.overlapEndUs, junction.overlapEndUs);
        fuzzyCompare(line.x + line.parent.x + 0.5, strip.positionAtUs(cut.cutUs), 0.5);
        const label = findChild(strip, "editJunctionLabel" + junction.id);
        verify(!!label);
        verify(label.text.indexOf("Hard cut") >= 0);
    }
    function test_collapsed_tracks_do_not_draw_a_cut_into_the_next_row() {
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.view.setVisibleInterval(98500000, 101000000);
        const junction = testSession.transitions.find(row => row.video);
        testSession.selectTransition(junction.outgoing);
        testSession.removeTransition();
        testSession.trackLayout.setTrackHeight(junction.trackId, 22);
        waitForRendering(strip);
        const line = findChild(strip, "editHardCutLine" + junction.id);
        verify(!!line);
        compare(line.visible, false);
    }
    function test_tiny_overlap_has_a_marker_without_stretching_its_window() {
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.view.setVisibleInterval(0, 200000000);
        waitForRendering(strip);
        const junction = testSession.transitions.find(row => row.video);
        const marker = findChild(strip, "editTinyTransitionMarker" + junction.id);
        verify(!!marker && marker.visible, "An overlap narrower than eight pixels has a two-pixel marker");
        compare(marker.width, 2);
        const window = findChild(strip, "editJunctionWindow" + junction.id);
        fuzzyCompare(window.width, strip.positionAtUs(junction.endUs) - strip.positionAtUs(junction.startUs), 0.5);
        verify(window.width < 8);
        strip.view.setVisibleInterval(98500000, 101000000);
        waitForRendering(strip);
        compare(marker.visible, false);
    }
}
