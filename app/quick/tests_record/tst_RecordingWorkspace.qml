import QtQuick
import QtTest

import ExoSnap.Quick.RecordPickerTestControls

TestCase {
    id: testCase
    name: "RecordingWorkspace"
    when: windowShown
    width: 1600
    height: 700
    visible: true

    Component {
        id: dockComponent
        RecordTransportDock {
            width: 1100
            height: 96
            recordViewModel: recordDriver.adapter
            clipAdapter: recordDriver.clipAdapter
        }
    }

    Component {
        id: surfaceComponent
        RecordingWorkspaceSurface {
            width: 1100
            height: 600
            workspaceState: RecordingWorkspaceState {}
            Rectangle {
                objectName: "previewProbe"
                anchors.fill: parent
            }
        }
    }

    Component {
        id: inspectorComponent
        SettingsPage {
            width: 860
            height: 600
            settings: recordDriver.settings
        }
    }

    Component {
        id: sourcePanelComponent
        AudioRoutingPanel {
            width: 320
            height: 600
            routing: recordDriver.settings.audioRouting
        }
    }

    function compareBounds(actual, expectedX, expectedWidth) {
        compare(actual.x, expectedX);
        compare(actual.y, 0);
        compare(actual.width, expectedWidth);
        compare(actual.height, 600);
    }

    function compareSurface(surface, expectedX, expectedWidth) {
        compareBounds(surface.previewBounds, expectedX, expectedWidth);
        const probe = findItem(surface, "previewProbe");
        verify(!!probe);
        const origin = probe.mapToItem(surface, 0, 0);
        compare(origin.x, expectedX);
        compare(origin.y, 0);
        compare(probe.width, expectedWidth);
        compare(probe.height, 600);
    }

    function findItem(item, name) {
        if (!item)
            return null;
        if (item.objectName === name)
            return item;
        for (let i = 0; i < item.children.length; ++i) {
            const found = findItem(item.children[i], name);
            if (found)
                return found;
        }
        return null;
    }

    function findAccessible(item, name) {
        if (!item || !item.visible)
            return null;
        if (item.Accessible.name === name)
            return item;
        for (let i = 0; i < item.children.length; ++i) {
            const found = findAccessible(item.children[i], name);
            if (found)
                return found;
        }
        return null;
    }

    function findAccessibleControl(item, name) {
        if (!item || !item.visible)
            return null;
        if (item.Accessible.name === name && (item.options !== undefined || item.from !== undefined))
            return item;
        for (let i = 0; i < item.children.length; ++i) {
            const found = findAccessibleControl(item.children[i], name);
            if (found)
                return found;
        }
        return null;
    }

    function findTextItems(item, prefix, result) {
        if (!item || !item.visible)
            return;
        if (typeof item.text === "string" && item.text.startsWith(prefix))
            result.push(item);
        for (let i = 0; i < item.children.length; ++i)
            findTextItems(item.children[i], prefix, result);
    }

    function cleanup() {
        recordDriver.setRecordingState("loading");
        recordDriver.setRecordingContext(false);
        recordDriver.seedAudioSources(0, "");
        recordDriver.workspaceState.closeSources();
        recordDriver.workspaceState.closeProfile();
    }

    function test_clock_centered_and_stop_reachable(data) {
        recordDriver.setRecordingState("recording");
        const dock = createTemporaryObject(dockComponent, testCase);
        verify(!!dock);
        dock.width = data.width;
        waitForRendering(dock);
        const clock = findItem(dock, "recordTransportClock");
        const actions = findItem(dock, "recordTransportActions");
        const stop = findAccessible(dock, qsTr("Stop recording"));
        verify(!!clock && !!actions && !!stop);
        verify(Math.abs(clock.x + clock.width / 2 - dock.width / 2) <= 1);
        verify(clock.width >= 112);
        verify(clock.x + clock.width + 8 <= actions.x);
        const stopPosition = stop.mapToItem(dock, 0, 0);
        verify(stopPosition.x >= 0 && stopPosition.x + stop.width <= dock.width);
    }

    function test_clock_centered_and_stop_reachable_data() {
        return [{ tag: "narrow", width: 860 },
                { tag: "medium", width: 1100 },
                { tag: "wide", width: 1600 }];
    }

    function test_many_long_named_sources_keep_clock_and_stop_clear(data) {
        recordDriver.seedAudioSources(8, "Very long configured audio source with device identity ");
        recordDriver.setRecordingState("recording");
        const dock = createTemporaryObject(dockComponent, testCase, {
            width: data.width, routing: recordDriver.settings.audioRouting
        });
        verify(!!dock);
        waitForRendering(dock);
        const clock = findItem(dock, "recordTransportClock");
        const stop = findAccessible(dock, qsTr("Stop recording"));
        const overflow = findAccessible(dock, qsTr("More audio sources"));
        verify(!!clock && !!stop && !!overflow);
        verify(Math.abs(clock.x + clock.width / 2 - dock.width / 2) <= 1);
        const overflowRight = overflow.mapToItem(dock, overflow.width, 0).x;
        verify(overflowRight + 8 <= clock.x);
        const stopPosition = stop.mapToItem(dock, 0, 0);
        verify(stopPosition.x >= clock.x + clock.width);
        verify(stopPosition.x + stop.width <= dock.width);
    }

    function test_many_long_named_sources_keep_clock_and_stop_clear_data() {
        return [{ tag: "860", width: 860 }, { tag: "1100", width: 1100 }, { tag: "1600", width: 1600 }];
    }
    function test_named_overflow_at_narrow_width() {
        recordDriver.setRecordingState("recording");
        const dock = createTemporaryObject(dockComponent, testCase);
        verify(!!dock);
        dock.width = 860;
        waitForRendering(dock);
        verify(!!findAccessible(dock, qsTr("More recording actions")));
        verify(!!findAccessible(dock, qsTr("Stop recording")));
    }

    function test_overlay_keeps_workspace_width_and_excludes_second_narrow_overlay() {
        const state = recordDriver.workspaceState;
        state.workspaceWidth = 1100;
        state.workspaceHeight = 600;
        state.openSources();
        compare(state.sourceState, "Overlay");
        compare(state.workspaceWidth, 1100);
        state.openProfile();
        compare(state.sourceState, "Closed");
        compare(state.profileState, "Overlay");
        compare(state.workspaceWidth, 1100);
    }

    function test_resize_closes_pinned_panel_without_reopening() {
        const state = recordDriver.workspaceState;
        state.workspaceWidth = 1300;
        state.workspaceHeight = 600;
        state.openSources();
        verify(state.canPinSources);
        state.pinSources();
        compare(state.sourceState, "Pinned");
        state.workspaceWidth = 700;
        compare(state.sourceState, "Closed");
        state.workspaceWidth = 1300;
        compare(state.sourceState, "Closed");
    }

    function test_production_surface_overlay_preserves_preview_bounds(data) {
        const surface = createTemporaryObject(surfaceComponent, testCase);
        verify(!!surface);
        surface.width = data.width;
        surface.scale = data.scale;
        const state = surface.workspaceState;
        compareSurface(surface, 0, data.width);
        compare(state.workspaceWidth, data.width);
        compare(state.workspaceHeight, 600);
        state.openSources();
        compare(state.sourceState, "Overlay");
        compareSurface(surface, 0, data.width);
        state.openProfile();
        compare(state.profileState, "Overlay");
        compare(state.sourceState, data.width < 1200 ? "Closed" : "Overlay");
        compareSurface(surface, 0, data.width);
    }

    function test_production_surface_overlay_preserves_preview_bounds_data() {
        return [{ tag: "860 logical, scale 1", width: 860, scale: 1 },
                { tag: "1100 logical, scale 1.5", width: 1100, scale: 1.5 },
                { tag: "1600 logical, scale 2", width: 1600, scale: 2 }];
    }

    function test_production_surface_pin_and_resize() {
        const surface = createTemporaryObject(surfaceComponent, testCase);
        verify(!!surface);
        surface.width = 1600;
        const state = surface.workspaceState;
        state.openSources();
        state.pinSources();
        compare(state.sourceState, "Pinned");
        compareSurface(surface, 320, 1280);
        state.openProfile();
        compareSurface(surface, 320, 1280);
        state.pinProfile();
        compare(state.profileState, "Pinned");
        compareSurface(surface, 320, 920);
        surface.width = 1000;
        compare(state.profileState, "Closed");
        compare(state.sourceState, "Pinned");
        compareSurface(surface, 320, 680);
        surface.width = 1600;
        compare(state.profileState, "Closed");
        compareSurface(surface, 320, 1280);
    }

    function test_production_surface_rejects_unsafe_second_pin(data) {
        const surface = createTemporaryObject(surfaceComponent, testCase);
        verify(!!surface);
        surface.width = data.width;
        const state = surface.workspaceState;
        state.openSources();
        state.pinSources();
        compare(state.sourceState, "Pinned");
        compareSurface(surface, 320, data.width - 320);
        state.openProfile();
        compare(state.profileState, "Overlay");
        compareSurface(surface, 320, data.width - 320);
        verify(!state.canPinProfile);
        state.pinProfile();
        compare(state.profileState, "Overlay");
        compareSurface(surface, 320, data.width - 320);
        verify(state.notice.length > 0);
    }

    function test_production_surface_rejects_unsafe_second_pin_data() {
        return [{ tag: "860", width: 860 }, { tag: "1100", width: 1100 }];
    }

    function test_production_surface_escape_closes_panel() {
        const surface = createTemporaryObject(surfaceComponent, testCase);
        verify(!!surface);
        surface.workspaceState.openSources();
        surface.forceActiveFocus();
        verify(surface.activeFocus);
        keyClick(Qt.Key_Escape);
        compare(surface.workspaceState.sourceState, "Closed");
    }

    function test_inspector_active_and_next_use_one_settings_plan() {
        const settings = recordDriver.settings;
        const activeFrameRate = settings.frameRate;
        const nextFrameRate = activeFrameRate === 30 ? 24 : 30;
        recordDriver.setRecordingContext(true);
        const inspector = createTemporaryObject(inspectorComponent, testCase);
        verify(!!inspector);
        waitForRendering(inspector);
        verify(!!findAccessible(inspector, qsTr("Active recording (read-only): %1").arg(settings.activePresetName)));
        const frameRate = findAccessibleControl(inspector, qsTr("Frame rate"));
        verify(!!frameRate);
        verify(!frameRate.enabled);

        const editNext = findAccessible(inspector, qsTr("Edit next recording"));
        verify(!!editNext);
        mouseClick(editNext);
        tryCompare(settings, "editingNextRecording", true);
        verify(!!findAccessible(inspector, qsTr("For the next recording")));
        verify(frameRate.enabled);
        settings.frameRate = nextFrameRate;
        compare(settings.frameRate, nextFrameRate);

        const showActive = findAccessible(inspector, qsTr("Show active recording"));
        verify(!!showActive);
        mouseClick(showActive);
        tryCompare(settings, "editingNextRecording", false);
        compare(settings.frameRate, activeFrameRate);
        verify(!frameRate.enabled);

        settings.editNextRecording();
        compare(settings.frameRate, nextFrameRate);
        recordDriver.setRecordingContext(false);
        compare(settings.frameRate, nextFrameRate);
        settings.frameRate = activeFrameRate;
    }

    function test_inspector_frame_rate_deep_link_reveals_and_focuses() {
        recordDriver.setRecordingContext(false);
        const inspector = createTemporaryObject(inspectorComponent, testCase);
        verify(!!inspector);
        waitForRendering(inspector);
        inspector.focusTarget(SettingsAdapter.FrameRate);
        const frameRate = findAccessibleControl(inspector, qsTr("Frame rate"));
        verify(!!frameRate);
        tryCompare(frameRate, "activeFocus", true);
        const position = frameRate.mapToItem(inspector, 0, 0);
        verify(position.y >= 0 && position.y + frameRate.height <= inspector.height,
               "the focused frame rate control is within the settings viewport");
    }

    function test_source_panel_shows_eight_long_named_sources_at_320_width() {
        const prefix = "Very long configured audio source with device identity ";
        recordDriver.seedAudioSources(8, prefix);
        const panel = createTemporaryObject(sourcePanelComponent, testCase);
        verify(!!panel);
        waitForRendering(panel);
        for (let index = 0; index < 8; ++index) {
            const names = [];
            findTextItems(panel, prefix + index, names);
            verify(names.length > 0, "each configured source has a visible identity");
            for (const name of names)
                verify(name.width <= panel.width, "source identity remains within the card width");
        }
    }
}
