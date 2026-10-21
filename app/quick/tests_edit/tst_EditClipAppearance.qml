import QtQuick
import QtQuick.Controls
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: tests
    name: "EditClipAppearance"
    when: windowShown
    width: 860
    height: 500
    visible: true

    Component {
        id: timelineComponent
        EditTimeline { session: testSession; player: testPlayer; width: 820; height: 300 }
    }
    function init() {
        testSession.cancelClipDrag();
        while (testSession.canUndo) testSession.undo();
        testSession.appendAsset(testSession.media[0].id);
    }
    function cleanup() {
        testSession.cancelClipDrag();
        while (testSession.canUndo) testSession.undo();
    }
    function test_track_tint_has_separate_readable_header() {
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.view.setVisibleInterval(0, 10000000);
        waitForRendering(strip);
        const row = testSession.visibleClipsUs(0, 10000000)[0];
        testSession.trackLayout.setTrackColor(row.trackId, "#4e8890");
        const clip = findChild(strip, "editClip" + row.id);
        verify(!!clip);
        const header = findChild(clip, "editClipHeader" + row.id);
        verify(!!header, "The clip name has its own track-tinted header");
        verify(header.color.toString() !== clip.color.toString());
        const label = findChild(clip, "editClipName" + row.id);
        verify(!!label);
        compare(clip.opacity, 1);
        compare(label.opacity, 1);
        verify(header.height <= clip.height);
    }
    function test_disabled_track_keeps_name_readable_and_announces_state() {
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.view.setVisibleInterval(0, 10000000);
        waitForRendering(strip);
        const row = testSession.visibleClipsUs(0, 10000000)[0];
        verify(testSession.setTrackEnabled(row.trackId, false));
        const clip = findChild(strip, "editClip" + row.id);
        verify(!!clip);
        compare(clip.opacity, 1);
        verify(clip.Accessible.name.indexOf(qsTr("Disabled")) >= 0,
               "Disabled state is available without relying on track colour");
    }
}
