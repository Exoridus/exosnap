import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: testCase
    name: "EditPlayerPresentation"
    when: windowShown
    width: 640
    height: 400
    visible: true

    Component {
        id: playerComponent
        EditPlayer {
            session: testSession
            player: testTransportPlayer
            width: 600
            height: 340
        }
    }

    function init() {
        testTransportPlayer.setPlaying(false);
        testTransportPlayer.endScrub();
    }

    function test_viewer_has_no_editing_controls_or_metadata() {
        const viewer = createTemporaryObject(playerComponent, testCase);
        verify(viewer);
        compare(findChild(viewer, "editPlayerToggle"), null);
        compare(viewer.color, ExoTheme.overlaySurface);
        verify(!viewer.gradient);
    }

    function test_SeekDoesNotDim() {
        const viewer = createTemporaryObject(playerComponent, testCase);
        verify(viewer);
        const surface = findChild(viewer, "editViewerSurface");
        verify(surface);
        const color = viewer.color;
        compare(surface.opacity, 1);
        testTransportPlayer.beginScrub();
        compare(testTransportPlayer.scrubbing, true);
        compare(surface.opacity, 1);
        compare(viewer.opacity, 1);
        compare(viewer.color, color);
        testTransportPlayer.endScrub();
        compare(surface.opacity, 1);
    }
    function test_rotation_crosses_the_angle_seam_in_both_directions_and_multiple_turns() {
        const viewer = createTemporaryObject(playerComponent, testCase);
        const radians = degrees => degrees * Math.PI / 180;
        for (const initial of [-35, 0, 420]) {
            viewer.beginRotation(initial, radians(170));
            verify(Math.abs(viewer.rotationAt(radians(-179)) - (initial + 11)) < 0.0001);
            viewer.beginRotation(initial, radians(-170));
            verify(Math.abs(viewer.rotationAt(radians(179)) - (initial - 11)) < 0.0001);
            viewer.beginRotation(initial, 0);
            for (let degrees = 30; degrees <= 720; degrees += 30) {
                const wrapped = Math.atan2(Math.sin(radians(degrees)), Math.cos(radians(degrees)));
                verify(Math.abs(viewer.rotationAt(wrapped) - (initial + degrees)) < 0.0001);
            }
        }
    }
}
