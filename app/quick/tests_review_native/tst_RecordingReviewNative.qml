import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: testCase
    name: "RecordingReviewNative"
    when: windowShown
    width: 860
    height: 500
    visible: true
    SignalSpy {
        id: editSpy
        target: testRecordingReview
        signalName: "editRequested"
    }
    function cleanup() {
        testRecordingReview.close();
        editSpy.clear();
    }
    function requireFrame(viewer) {
        const surface = findChild(viewer, "reviewSurface");
        for (let elapsed = 0; !surface.hasFrame && elapsed < 5000; elapsed += 20)
            wait(20);
        verify(surface.hasFrame, JSON.stringify({
            error: surface.errorText,
            reason: viewer.player.placeholderText,
            state: viewer.player.previewState,
            pending: viewer.player.transportPending,
            seeking: viewer.player.seekPending,
            tracks: viewer.player.monitoringTracks
        }));
    }

    function test_native_open_seek_select_pause_and_close() {
        const viewer = createTemporaryObject(reviewComponent, testCase, {
            session: testRecordingReview.session,
            player: testRecordingReview.player,
            audioController: testRecordingReview
        });
        verify(viewer);
        verify(testEditHarness.prepareRecordingReview());
        const review = testRecordingReview;
        tryCompare(review.session, "importPending", false, 10000);
        tryCompare(review.player, "clipOpen", true, 10000);
        requireFrame(viewer);
        tryVerify(() => review.player.monitoringTracks.length === 2, 10000);
        tryVerify(() => review.player.monitoringTracks.filter(track => track.selected).length === 1, 10000);
        compare(review.player.desiredPlaying, false);
        review.session.requestSeek(700);
        tryCompare(review.player, "seekPending", false, 10000);
        compare(review.session.positionMs, 700);
        tryVerify(() => Math.abs(Number(review.player.presentedTimeUs) - 700000) <= 50000, 10000);
        const other = review.player.monitoringTracks.find(track => !track.selected);
        const original = review.player.monitoringTracks.find(track => track.selected);
        findChild(viewer, "reviewAudio").forceActiveFocus();
        const panel = findChild(viewer, "reviewAudioPanel");
        tryCompare(panel, "opened", true);
        // Popup content is reparented into the window overlay, outside the viewer's visual tree.
        let otherButton = null;
        tryVerify(() => (otherButton = findChild(panel.contentItem, "reviewTrack-" + other.id)) !== null);
        tryCompare(otherButton, "enabled", true);
        mouseClick(otherButton);
        tryVerify(() => review.player.monitoringTracks.some(track => track.id === other.id && track.selected), 10000);
        compare(review.player.monitoringTracks.filter(track => track.selected).length, 1);
        tryCompare(review.player, "seekPending", false, 10000);
        review.selectTrack(original.id);
        tryVerify(() => review.player.monitoringTracks.some(track => track.id === original.id && track.selected), 10000);
        compare(review.player.monitoringTracks.filter(track => track.selected).length, 1);
        compare(review.player.volume, 0);
        const speaker = findChild(viewer, "reviewAudio");
        mouseClick(speaker);
        compare(review.muted, false);
        compare(review.player.volume, review.volume);
        mouseClick(speaker);
        compare(review.muted, true);
        compare(review.player.volume, 0);
        const volume = findChild(viewer, "reviewVolume");
        volume.forceActiveFocus();
        keyClick(Qt.Key_Left);
        verify(review.volume < 1);
        compare(review.player.volume, 0);
        keyClick(Qt.Key_Escape);
        mouseClick(viewer, viewer.width / 2, viewer.height / 3);
        tryCompare(review.player, "playing", true, 10000);
        findChild(viewer, "reviewPlayPause").forceActiveFocus();
        keyClick(Qt.Key_Space);
        tryCompare(review.player, "playing", false, 10000);
        review.requestEdit();
        compare(editSpy.count, 1);
        verify(testEditHarness.recordingReviewFixtureIdentity().length > 0);
        compare(editSpy.signalArguments[0][0], testEditHarness.recordingReviewFixtureIdentity());
        mouseClick(findChild(viewer, "reviewClose"));
        compare(review.active, false);
        compare(review.session.importPending, false);
    }

    Component {
        id: reviewComponent
        RecordingReview {
            width: testCase.width
            height: testCase.height
            session: testSession
            player: testTransportPlayer
            onCloseRequested: if (audioController)
                audioController.close()
        }
    }
}
