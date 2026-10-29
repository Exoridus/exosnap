pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import ExoSnap.Quick.RecordPickerTestControls

TestCase {
    id: testCase
    name: "AudioRoutingPanel"
    when: windowShown
    width: 860
    height: 700
    visible: true

    Component {
        id: panelComponent
        AudioRoutingPanel {
            width: 520
            routing: recordDriver.settings.audioRouting
        }
    }
    Component {
        id: settingsComponent
        SettingsPage {
            settings: recordDriver.settings
        }
    }
    Rectangle {
        anchors.fill: parent
        color: ExoTheme.background
        z: -1
    }
    function init() {
        recordDriver.seedAudioSources(2, "Source ");
        recordDriver.clearActiveAudioPlan();
    }
    function cleanup() {
        recordDriver.setTestAppearance(QuickThemeTokens, "dark");
        testCase.width = 860;
        testCase.height = 700;
        testCase.Window.window.width = 860;
        testCase.Window.window.height = 700;
    }
    function panel() {
        const result = createTemporaryObject(panelComponent, testCase);
        verify(!!result);
        waitForRendering(result);
        return result;
    }
    function row(item, id) {
        return findChild(item, "audioTrack-" + id);
    }
    function visibleText(item, text) {
        if (!item || !item.visible)
            return null;
        if (item.text === text)
            return item;
        for (const child of item.children) {
            const result = visibleText(child, text);
            if (result)
                return result;
        }
        return null;
    }

    function test_empty_warning_is_next_to_track_name() {
        const item = panel();
        verify(item.routing.addEmptyTrack(item.routing.revision));
        waitForRendering(item);
        const track = item.routing.tracks[item.routing.tracks.length - 1];
        const trackRow = row(item, track.id);
        const name = visibleText(trackRow, track.name);
        const warning = visibleText(trackRow, qsTr("No source"));
        verify(!!name && !!warning);
        const gap = warning.mapToItem(trackRow, 0, 0).x - name.mapToItem(trackRow, name.contentWidth, 0).x;
        verify(gap >= 0 && gap <= ExoTheme.spacingSm + 1, "The empty-track warning sits directly beside its name.");
    }

    function test_compact_layout_and_assignment_survive_model_refresh() {
        const item = panel();
        const routing = item.routing;
        const sourceId = routing.sources[0].id;
        const trackId = routing.tracks[0].id;
        verify(item.height < 220);
        mouseClick(findChild(row(item, trackId), "audioTrackSources"));
        const popup = findChild(item, "audioSourcePopup");
        tryCompare(popup, "opened", true);
        let toggle = findChild(popup.contentItem, "audioAssign-" + sourceId);
        verify(toggle.checked);
        mouseClick(toggle);
        tryCompare(popup, "opened", true);
        compare(routing.tracks[0].routes.length, 1);
        waitForRendering(item);
        toggle = findChild(popup.contentItem, "audioAssign-" + sourceId);
        verify(!toggle.checked);
        mouseClick(toggle);
        compare(routing.tracks[0].routes.length, 2);
        popup.close();
        item.width = 300;
        waitForRendering(item);
        verify(findChild(row(item, trackId), "audioTrackMenu").x >= 0);
    }
    function test_route_gain_popup_uses_engine_bounds_and_stays_open() {
        const item = panel();
        const track = item.routing.tracks[0];
        mouseClick(findChild(item, "audioRoute-" + track.routes[0].id));
        const popup = findChild(item, "audioGainPopup");
        tryCompare(popup, "opened", true);
        const field = findChild(popup, "audioRouteGain");
        compare(field.from, item.routing.minimumGainDb);
        compare(field.to, item.routing.maximumGainDb);
        compare(field.suffix, qsTr("dB"));
        field.forceActiveFocus();
        keyClick(Qt.Key_Down);
        tryVerify(() => item.routing.tracks[0].routes[0].gainDb < 0);
        verify(popup.opened);
    }
    function test_inline_rename_escape_and_direct_removal() {
        const item = panel();
        const routing = item.routing;
        const trackId = routing.tracks[0].id;
        row(item, trackId).beginRename();
        let field = findChild(row(item, trackId), "audioTrackName");
        field.text = "Cancelled";
        keyClick(Qt.Key_Escape);
        compare(routing.tracks[0].name, "Audio 1");
        row(item, trackId).beginRename();
        field.text = "My mix";
        keyClick(Qt.Key_Return);
        tryCompare(routing.tracks[0], "name", "My mix");
        mouseClick(findChild(row(item, trackId), "audioTrackMenu"));
        const menu = findChild(item, "audioTrackActions");
        tryCompare(menu, "opened", true);
        const remove = findChild(menu, "audioRequestRemoveTrack");
        verify(remove.visible && remove.enabled);
        mouseClick(remove, remove.width / 2, remove.height / 2);
        tryVerify(() => routing.tracks.length === 0);
        compare(routing.sources.length, 2);
    }
    function test_keyboard_reorder_preserves_stable_track_identity() {
        const item = panel();
        const id = item.routing.tracks[0].id;
        verify(item.routing.addEmptyTrack(item.routing.revision));
        const firstRow = row(item, id);
        firstRow.forceActiveFocus();
        keyClick(Qt.Key_Down, Qt.AltModifier);
        tryVerify(() => item.routing.tracks[1].id === id);
        const secondRow = row(item, id);
        secondRow.forceActiveFocus();
        keyClick(Qt.Key_Up, Qt.AltModifier);
        tryVerify(() => item.routing.tracks[0].id === id);
    }

    function test_drag_reorders_by_stable_identity() {
        const item = panel();
        const id = item.routing.tracks[0].id;
        verify(item.routing.addEmptyTrack(item.routing.revision));
        verify(item.routing.addEmptyTrack(item.routing.revision));
        waitForRendering(item);
        const grip = findChild(row(item, id), "audioTrackGrip");
        const target = row(item, item.routing.tracks[2].id);
        const start = grip.mapToItem(testCase, grip.width / 2, grip.height / 2);
        const end = target.mapToItem(testCase, target.width / 2, target.height / 2);
        mouseDrag(grip, grip.width / 2, grip.height / 2, end.x - start.x, end.y - start.y, Qt.LeftButton);
        tryVerify(() => item.routing.tracks[2].id === id);
    }

    function test_flat_source_creation_assigns_in_one_action() {
        const item = panel();
        const trackId = item.routing.tracks[0].id;
        mouseClick(findChild(row(item, trackId), "audioTrackSources"));
        const picker = findChild(item, "audioSourcePopup");
        tryCompare(picker, "opened", true);
        const create = findChild(picker.contentItem, "audioCreateSystemSource");
        verify(!!create && create.visible);
        verify(!findChild(picker.contentItem, "audioCreateIndependentSource"));
        mouseClick(create);
        tryCompare(picker, "opened", true);
        compare(item.routing.sources.length, 3);
        compare(item.routing.tracks[0].routes.length, 3);
    }

    function test_single_track_has_no_reorder_handle() {
        const item = panel();
        const track = item.routing.tracks[0];
        verify(!findChild(row(item, track.id), "audioTrackGrip").visible);
        verify(item.routing.addEmptyTrack(item.routing.revision));
        waitForRendering(item);
        verify(findChild(row(item, track.id), "audioTrackGrip").visible);
    }

    function test_number_and_quiet_grip_share_a_vertical_gutter() {
        const item = panel();
        const trackId = item.routing.tracks[0].id;
        verify(item.routing.addEmptyTrack(item.routing.revision));
        waitForRendering(item);
        const trackRow = row(item, trackId);
        const number = visibleText(trackRow, "01");
        const grip = findChild(trackRow, "audioTrackGrip");
        const numberCenter = number.mapToItem(trackRow, number.width / 2, number.height / 2);
        const gripCenter = grip.mapToItem(trackRow, grip.width / 2, grip.height / 2);
        fuzzyCompare(numberCenter.x, gripCenter.x, 1);
        verify(numberCenter.y < gripCenter.y);
        mouseMove(item, item.width - 1, item.height - 1);
        tryCompare(grip, "hovered", false);
        const ink = grip.contentItem.color;
        mouseMove(grip, grip.width / 2, grip.height / 2);
        tryCompare(grip, "hovered", true);
        compare(grip.contentItem.color, ink);
    }

    function test_track_rms_is_measured_active_output_only_data() {
        return [
            {
                tag: "measured",
                rms: 0.25,
                available: true
            },
            {
                tag: "measured-silence",
                rms: 0,
                available: true
            },
            {
                tag: "unavailable",
                rms: -1,
                available: false
            }
        ];
    }

    function test_track_rms_is_measured_active_output_only(data) {
        recordDriver.seedAudioSources(1, "Measured source ", 1, data.rms);
        const item = panel();
        const trackId = item.routing.tracks[0].id;
        item.editingNext = false;
        waitForRendering(item);
        let trackRow = row(item, trackId);
        let meter = findChild(trackRow, "audioTrackMeter");
        compare(trackRow.meterAvailable, data.available);
        compare(meter.visible, data.available);
        if (data.available) {
            verify(item.routing.trackMeters[trackId].available);
            const expectedDbfs = data.rms === 0 ? Number.NEGATIVE_INFINITY : 20 * Math.log10(data.rms);
            if (data.rms === 0) {
                compare(item.routing.trackMeters[trackId].rmsDbfs, expectedDbfs);
                compare(meter.Accessible.description, qsTr("−∞ dBFS"));
                compare(meter.children[0].width, 0);
            } else {
                fuzzyCompare(item.routing.trackMeters[trackId].rmsDbfs, expectedDbfs, 0.000001);
            }
        }
        item.editingNext = true;
        const routeId = item.routing.tracks[0].routes[0].id;
        verify(item.routing.setRouteGain(routeId, -12.375, item.routing.revision));
        waitForRendering(item);
        trackRow = row(item, trackId);
        compare(trackRow.track.id, trackId);
        if (data.available)
            verify(item.routing.trackMeters[trackId].available);
        verify(!trackRow.meterAvailable);
        verify(!findChild(trackRow, "audioTrackMeter").visible);
    }

    function test_failed_rename_is_reported_on_its_track() {
        const item = panel();
        const trackId = item.routing.tracks[0].id;
        row(item, trackId).beginRename();
        const field = findChild(row(item, trackId), "audioTrackName");
        field.text = "";
        keyClick(Qt.Key_Return);
        tryVerify(() => item.routing.lastError !== "");
        const error = findChild(row(item, trackId), "audioTrackError");
        verify(!!error && error.visible);
        compare(error.text, item.routing.lastError);
        compare(item.title, qsTr("Audio tracks"));
        compare(item.subtitle, "");
    }

    function test_visual_states_data() {
        return [
            {
                tag: "dark-1600x1000",
                appearance: "dark",
                width: 1600,
                height: 1000
            },
            {
                tag: "light-1600x1000",
                appearance: "light",
                width: 1600,
                height: 1000
            },
            {
                tag: "dark-860x700",
                appearance: "dark",
                width: 860,
                height: 700
            },
            {
                tag: "light-860x700",
                appearance: "light",
                width: 860,
                height: 700
            }
        ];
    }

    function test_visual_states(data) {
        recordDriver.seedCompactAudioRouting();
        recordDriver.setTestAppearance(QuickThemeTokens, data.appearance);
        testCase.width = data.width;
        testCase.height = data.height;
        const window = testCase.Window.window;
        window.width = data.width;
        window.height = data.height;
        const page = createTemporaryObject(settingsComponent, testCase, {
            width: data.width,
            height: data.height
        });
        verify(!!page);
        tryVerify(() => page.revealAutomationTarget("audio") === 1);
        const card = page.automationTargets.audio;
        const track = card.routing.tracks[0];
        for (const state of ["base", "sources", "gain", "menu", "rename", "empty"]) {
            card.harnessCollapse();
            const trackRow = row(card, track.id);
            if (state === "sources")
                mouseClick(findChild(trackRow, "audioTrackSources"));
            else if (state === "gain")
                mouseClick(findChild(trackRow, "audioRoute-" + track.routes[0].id));
            else if (state === "menu") {
                mouseClick(findChild(trackRow, "audioTrackMenu"));
                const menu = findChild(card, "audioTrackActions");
                tryCompare(menu, "opened", true);
                verify(menu.height > 100);
            } else if (state === "rename")
                trackRow.beginRename();
            else if (state === "empty")
                verify(card.routing.addEmptyTrack(card.routing.revision));
            waitForRendering(page);
            if (recordDriver.visualArtifactDirectory !== "")
                grabImage(window.contentItem).save(recordDriver.visualArtifactDirectory + "/" + data.tag + "-" + state + ".png");
            if (state === "rename")
                keyClick(Qt.Key_Escape);
        }
    }
}
