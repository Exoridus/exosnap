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
        const slider = findChild(popup, "audioRouteGain");
        compare(slider.from, item.routing.minimumGainDb);
        compare(slider.to, item.routing.maximumGainDb);
        slider.forceActiveFocus();
        keyClick(Qt.Key_Left);
        tryVerify(() => item.routing.tracks[0].routes[0].gainDb < 0);
        verify(popup.opened);
    }
    function test_inline_rename_escape_and_removal_confirmation() {
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
        tryCompare(item, "confirmingRemoval", true);
        tryCompare(menu, "opened", true);
        compare(routing.tracks.length, 1);
        waitForRendering(item);
        mouseClick(findChild(menu, "audioConfirmRemoveTrack"));
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

    function test_source_creation_keeps_assignments_and_returns_to_picker() {
        const item = panel();
        const trackId = item.routing.tracks[0].id;
        mouseClick(findChild(row(item, trackId), "audioTrackSources"));
        const picker = findChild(item, "audioSourcePopup");
        tryCompare(picker, "opened", true);
        mouseClick(findChild(picker.contentItem, "audioCreateSource"));
        const menu = findChild(item, "audioSourceCreationMenu");
        tryCompare(menu, "opened", true);
        verify(!picker.opened);
        mouseClick(findChild(menu, "audioCreateSystemSource"));
        tryCompare(picker, "opened", true);
        compare(item.routing.sources.length, 3);
        compare(item.routing.tracks[0].routes.length, 2);
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
        for (const state of ["base", "sources", "gain", "menu", "confirm-remove", "rename", "empty"]) {
            card.harnessCollapse();
            const trackRow = row(card, track.id);
            if (state === "sources")
                mouseClick(findChild(trackRow, "audioTrackSources"));
            else if (state === "gain")
                mouseClick(findChild(trackRow, "audioRoute-" + track.routes[0].id));
            else if (state === "menu" || state === "confirm-remove") {
                mouseClick(findChild(trackRow, "audioTrackMenu"));
                const menu = findChild(card, "audioTrackActions");
                tryCompare(menu, "opened", true);
                verify(menu.height > 100);
                if (state === "confirm-remove") {
                    mouseClick(findChild(menu, "audioRequestRemoveTrack"));
                    tryCompare(menu, "opened", true);
                }
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
