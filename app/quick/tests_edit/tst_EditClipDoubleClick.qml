import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: tests
    name: "EditClipDoubleClick"
    when: windowShown
    width: 640
    height: 520
    visible: true

    Component {
        id: timelineComponent
        EditTimeline { session: testSession; player: testPlayer; width: 600; height: 240 }
    }
    Component { id: spyComponent; SignalSpy {} }

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
    function makeStrip() {
        const strip = createTemporaryObject(timelineComponent, tests);
        verify(!!strip);
        strip.view.setVisibleInterval(0, 100000000);
        waitForRendering(strip);
        return strip;
    }
    function test_fade_doubleclick_resets_only_that_end_and_undo_restores_it() {
        const strip = makeStrip();
        const id = testSession.selectedClip;
        testSession.setClipFade(id, true, 1000);
        testSession.setClipFade(id, false, 2000);
        waitForRendering(strip);
        const clip = findChild(strip, "editClip" + id);
        verify(!!clip);
        mouseDoubleClickSequence(clip, 3, 3);
        compare(testSession.selectedClipDetails.fadeInUs, 0);
        compare(testSession.selectedClipDetails.fadeOutUs, 2000000);
        testSession.undo();
        compare(testSession.selectedClipDetails.fadeInUs, 1000000);
        waitForRendering(strip);
        strip.view.setVisibleInterval(90000000, 101000000);
        waitForRendering(strip);
        mouseDoubleClickSequence(clip, clip.width - 3, 3);
        compare(testSession.selectedClipDetails.fadeInUs, 1000000);
        compare(testSession.selectedClipDetails.fadeOutUs, 0);
    }
    function test_header_and_trim_request_explicit_inspector_fields() {
        const strip = makeStrip();
        const spy = createTemporaryObject(spyComponent, tests, {target: strip, signalName: "inspectorOpenRequested"});
        verify(spy.valid);
        const clip = findChild(strip, "editClip" + testSession.selectedClip);
        verify(!!clip);
        mouseDoubleClickSequence(clip, 60, 8);
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "");
        mouseDoubleClickSequence(clip, 2, 30);
        compare(spy.count, 2);
        compare(spy.signalArguments[1][0], "sourceIn");
        strip.view.setVisibleInterval(90000000, 101000000);
        waitForRendering(strip);
        mouseDoubleClickSequence(clip, clip.width - 2, 30);
        compare(spy.count, 3);
        compare(spy.signalArguments[2][0], "sourceOut");
        compare(testSession.selectedClipIds.length, 1);
    }
}