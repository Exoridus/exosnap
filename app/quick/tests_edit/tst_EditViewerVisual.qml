import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: tests
    name: "EditViewerVisual"
    when: windowShown
    width: 640
    height: 360
    visible: true

    function test_declared_states_keep_synthetic_still_unmeasured_and_transport_unchanged() {
        const facts = testEditHarness.syntheticViewerStatesProbe();
        verify(facts.states, "Each requested declared preview state must reach the adapter");
        verify(facts.reasons, "Each state must retain its declared visible reason");
        verify(facts.retainedUnmeasuredStill, "Opening/buffering omit the still; degraded/failed retain it without a presentation stamp");
        verify(facts.loop, "The visual loop bounds must be latched without playback");
        verify(facts.staleRepliesFenced, "Each synthetic still must fence replies queued for an earlier open");
        verify(facts.noTransport, "Composition seeding must not submit native transport commands");
    }

    Component {
        id: viewerComponent
        EditPlayer {
            width: tests.width
            height: tests.height
            session: testSession
            player: testPlayer
        }
    }

    function test_state_reason_is_visible_data() {
        return [
            {tag: "opening", state: EditPlayerAdapter.Opening, retains: false},
            {tag: "buffering", state: EditPlayerAdapter.Buffering, retains: false},
            {tag: "degraded", state: EditPlayerAdapter.Degraded, retains: true},
            {tag: "failed", state: EditPlayerAdapter.Failed, retains: true}
        ];
    }

    function test_state_reason_is_visible(data) {
        verify(testEditHarness.prepareViewerState(testPlayer, data.state));
        const viewer = createTemporaryObject(viewerComponent, tests);
        verify(viewer !== null);
        const center = findChild(viewer, "editViewerReason");
        const badge = findChild(viewer, "editViewerStateBadge");
        const still = findChild(viewer, "editHarnessStill");
        verify(center !== null && badge !== null && still !== null);
        compare(center.visible, !data.retains);
        compare(badge.visible, data.retains);
        compare(still.source.toString().length > 0, data.retains);
        verify(testPlayer.placeholderText.length > 0);
    }
}