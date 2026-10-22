import QtQuick
import QtQuick.Controls
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: tests
    name: "EditActionAvailability"
    when: windowShown
    width: 1280
    height: 800
    visible: true
    SignalSpy { id: actionClicks; signalName: "clicked" }
    Component {
        id: pageComponent
        EditPage {
            width: tests.width
            height: tests.height
            session: testPageSession
            timeline: testPageTimeline
            player: testPagePlayer
            exporter: testPageExporter
            recordings: testPageRecordings
            shell: testPageShell
            mediaPinPreferred: false
            detailsPinPreferred: false
        }
    }
    function init() {
        testPageSession.close();
        testSession.cancelClipDrag();
        testSession.cancelRangeGesture();
        while (testSession.canUndo) testSession.undo();
    }
    function cleanup() {
        testSession.cancelClipDrag();
        testSession.cancelRangeGesture();
        while (testSession.canUndo) testSession.undo();
        testPageSession.close();
        testEditHarness.exportVisualState(EditExportAdapter.Options);
    }
    function test_narrow_export_status_does_not_cover_transport_data() {
        return [{tag: "preparing", state: EditExportAdapter.Preparing, audioChoice: false},
                {tag: "cancelling", state: EditExportAdapter.Cancelling, audioChoice: false},
                {tag: "preparing-audio-choice", state: EditExportAdapter.Preparing, audioChoice: true},
                {tag: "cancelling-audio-choice", state: EditExportAdapter.Cancelling, audioChoice: true}];
    }
    function test_narrow_export_status_does_not_cover_transport(data) {
        if (data.audioChoice) {
            const fixture = testEditHarness.prepareSourceAudioChoice();
            if (!fixture.ready)
                skip(fixture.reason);
            testPageSession.cancelSourceAudioChoice();
        }
        testEditHarness.exportVisualState(data.state);
        const page = createTemporaryObject(pageComponent, tests, {width: 860});
        waitForPolish(page);
        waitForRendering(page);
        const transport = findChild(page, "editTransportGroup");
        const output = findChild(page, "editVolume").parent;
        const left = output.mapToItem(transport.parent, 0, 0).x;
        verify(transport.x + transport.width + 4 <= left,
               "Output controls remain outside the transport group at 860 px: "
               + JSON.stringify({transportX: transport.x, transportWidth: transport.width,
                                 outputX: left, outputWidth: output.width}));
        // Export state lives on its rail button, not in the timeline toolbar.
        verify(!findChild(findChild(page, "editTimelineHeader"), "editExportProgress"));
        const rail = findChild(page, "editExport");
        if (testPageExporter.running)
            verify(findChild(page, "editExportRailProgress").visible);
        verify(rail.Accessible.name.length > 0);
    }
    function test_empty_timeline_disabled_actions_explain_their_condition() {
        compare(testPageSession.durationUs, 0);
        const page = createTemporaryObject(pageComponent, tests);
        for (const name of ["editUndo", "editRedo", "editSplit", "editDelete", "editRippleDeleteAction",
                            "editTransportStart", "editTransportPreviousFrame", "editTransportPlay",
                            "editTransportNextFrame", "editTransportEnd", "editLoop", "editRangeOnce", "editExport"]) {
            const action = findChild(page, name);
            verify(!!action, name);
            compare(action.enabled, false, name);
            verify(action.Accessible.description.length > 0, name + " exposes its actual disabled reason");
            verify(action.ToolTip.text.includes(action.Accessible.description), name + " tooltip includes the same reason");
        }
        const undo = findChild(page, "editUndo");
        mouseMove(undo);
        tryCompare(undo.ToolTip, "visible", true);
    }
    function test_transport_actions_have_icons_data() {
        return [{tag: "loop", name: "editLoop"}, {tag: "range-once", name: "editRangeOnce"}];
    }
    function test_transport_actions_have_icons(data) {
        const page = createTemporaryObject(pageComponent, tests);
        const action = findChild(page, data.name);
        verify(!!action);
        verify(action.glyph !== ExoGlyph.Invalid, "The compact action uses a drawn icon instead of an overflowing localized label");
    }
    function test_range_delete_action_lifts_the_visible_range() {
        testSession.appendAsset(testSession.media[0].id);
        verify(testSession.setEditRangeUs(1000000, 2000000, testSession.tracks.map(row => row.id)));
        const page = createTemporaryObject(pageComponent, tests, {
            session: testSession, timeline: testTimeline, player: testPlayer
        });
        waitForPolish(page);
        const action = findChild(page, "editDelete");
        verify(!!action);
        verify(action.enabled, "Delete is available for the active range selection");
        waitForRendering(page);
        actionClicks.target = action;
        actionClicks.clear();
        const point = action.mapToItem(tests, action.width / 2, action.height / 2);
        verify(point.x >= 0 && point.y >= 0 && point.x < tests.Window.window.width && point.y < tests.Window.window.height,
               JSON.stringify({x: point.x, y: point.y, width: action.width, height: action.height,
                               windowWidth: tests.Window.window.width, windowHeight: tests.Window.window.height}));
        mouseClick(action);
        compare(actionClicks.count, 1, "The visible toolbar button receives the pointer click");
        const clips = testSession.visibleClipsUs(0, 100000000).filter(row => row.video);
        compare(clips.length, 2, testSession.workspaceError + " " + JSON.stringify(clips));
        compare(clips[0].endUs, 1000000);
        compare(clips[1].startUs, 2000000);
        compare(testSession.durationUs, 100000000);
    }
}
