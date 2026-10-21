import QtQuick
import QtQuick.Controls
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: tests
    name: "EditRangeScope"
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
        testSession.cancelRangeGesture();
        while (testSession.canUndo) testSession.undo();
        testSession.appendAsset(testSession.media[0].id);
    }
    function cleanup() {
        testSession.cancelRangeGesture();
        while (testSession.canUndo) testSession.undo();
    }
    function tracks() {
        return {audio: testSession.tracks.find(row => !row.video).id,
                video: testSession.tracks.find(row => row.video).id};
    }
    function test_range_gain_has_an_explicit_delta_control() {
        const ids = tracks();
        verify(testSession.setEditRangeUs(1000000, 2000000, [ids.audio]));
        const page = createTemporaryObject(pageComponent, tests);
        page.toggleDetails();
        const gain = findChild(page, "editDetailsRangeGain");
        verify(!!gain && gain.visible, "The active range exposes its additive audio gain command");
        verify(!gain.applyValue(3, 0, {}), "An incomplete linked-track scope is refused atomically");
        compare(testSession.visibleClipsUs(0, 100000000).filter(row => !row.video).length, 1);
        verify(testSession.setEditRangeUs(1000000, 2000000, [ids.audio, ids.video]));
        verify(gain.applyValue(3, 0, {}));
        const audio = testSession.visibleClipsUs(0, 100000000).filter(row => !row.video);
        compare(audio.length, 3);
        compare(audio.find(row => row.startUs === 1000000).gainDb, 3);
        compare(audio.find(row => row.startUs === 0).gainDb, 0);
        testSession.undo();
        compare(testSession.visibleClipsUs(0, 100000000).filter(row => !row.video).length, 1);
    }
    function test_collapsed_grid_conversion_retains_both_nearest_endpoints() {
        const ids = tracks();
        verify(testSession.setEditRangeUs(21, 42, [ids.audio]));
        const original = testSession.editRange;
        verify(testSession.beginRangeGestureUs(21, [ids.audio], true, true));
        testSession.updateRangeGestureToTrackUs(42, ids.video);
        compare(testSession.rangeGestureValid, false);
        const candidate = testSession.rangeGestureCandidate;
        verify(candidate.startUs !== undefined && candidate.endUs !== undefined,
               "Invalid scope conversion still exposes both nearest endpoints");
        compare(candidate.startUs, 0);
        compare(candidate.endUs, 0);
        compare(candidate.trackIds.length, 2);
        verify(testSession.rangeGestureReason.length > 0);
        compare(testSession.editRange, original);
        verify(!testSession.commitRangeGesture());
        compare(testSession.editRange, original);
    }
    function test_include_linked_tracks_is_reviewed_before_one_scope_commit() {
        const ids = tracks();
        verify(testSession.setEditRangeUs(1000000, 2000000, [ids.audio]));
        const original = testSession.editRange;
        const position = testSession.positionUs;
        verify(testSession.includeLinkedRangeTracks());
        compare(testSession.rangeScopeReviewActive, true);
        compare(testSession.rangeGestureValid, true);
        compare(testSession.rangeGestureCandidate.trackIds.length, 2);
        compare(testSession.editRange, original);
        testSession.deleteSelected(false);
        compare(testSession.rangeScopeReviewActive, true);
        compare(testSession.visibleClipsUs(0, 10000000).length, 2);
        verify(testSession.commitRangeGesture());
        compare(testSession.editRange.trackIds.length, 2);
        compare(testSession.positionUs, position);
    }
    function test_inspector_can_remove_scope_cancel_and_keep_focus() {
        const ids = tracks();
        verify(testSession.setEditRangeUs(1000000, 2000000, [ids.audio, ids.video]));
        const original = testSession.editRange;
        const page = createTemporaryObject(pageComponent, tests);
        page.toggleDetails();
        const choiceName = "editRangeScope" + ids.video;
        tryVerify(() => findChild(page, choiceName) !== null);
        const choice = findChild(page, choiceName);
        verify(choice.checked);
        choice.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        compare(testSession.rangeScopeReviewActive, true);
        compare(testSession.rangeGestureCandidate.trackIds, [ids.audio]);
        compare(testSession.editRange, original);
        verify(choice.activeFocus);
        const cancel = findChild(page, "editRangeScopeCancel");
        verify(!!cancel);
        mouseClick(cancel);
        compare(testSession.rangeScopeReviewActive, false);
        compare(testSession.editRange, original);
    }
    function test_scope_grid_conversion_and_empty_scope_are_atomic() {
        const ids = tracks();
        verify(testSession.setEditRangeUs(21000, 42000, [ids.audio], 21000));
        const original = testSession.editRange;
        verify(testSession.previewRangeScope([ids.video, ids.audio]));
        compare(testSession.rangeGestureCandidate.startUs, 16666);
        compare(testSession.rangeGestureCandidate.endUs, 50000);
        compare(testSession.rangeGestureCandidate.anchorUs, 16666);
        compare(testSession.editRange, original);
        verify(testSession.commitRangeGesture());
        verify(testSession.previewRangeScope([ids.audio]));
        compare(testSession.rangeGestureCandidate.startUs, 16667);
        compare(testSession.rangeGestureCandidate.endUs, 50000);
        verify(testSession.commitRangeGesture());
        const converted = testSession.editRange;
        verify(testSession.previewRangeScope([]));
        compare(testSession.rangeGestureValid, false);
        compare(testSession.rangeGestureCandidate.trackIds.length, 0);
        verify(!testSession.commitRangeGesture());
        compare(testSession.editRange, converted);
    }
}
