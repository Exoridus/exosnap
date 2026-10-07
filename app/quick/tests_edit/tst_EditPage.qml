import QtQuick
import QtQuick.Controls
import QtTest
import ExoSnap.Quick.EditTestControls

Item {
    id: root
    width: 860
    height: 600

    Component {
        id: pageComponent
        EditPage {
            width: root.width
            height: root.height
            session: testPageSession
            timeline: testPageTimeline
            player: testPagePlayer
            exporter: testPageExporter
            recordings: testPageRecordings
            Rectangle { anchors.fill: parent; z: -1; color: ExoTheme.background }
        }
    }

    TestCase {
        name: "EditPage"
        when: windowShown

        function make(properties) {
            const page = createTemporaryObject(pageComponent, root, properties || {});
            verify(!!page, "Component exists");
            return page;
        }

        function child(page, name) {
            const item = findChild(page, name);
            verify(!!item, "Object exists");
            return item;
        }

        function test_splitters_restore_clamp_resize_and_keyboard() {
            const page = make();
            const preferences = child(page, "editLayoutPreferences");
            const source = child(page, "editSourceBrowser");
            const timeline = child(page, "editTimelineZone");
            const upper = child(page, "editHorizontalSplit");
            preferences.sourceFraction = 0.4;
            preferences.timelineFraction = 0.55;
            page.restoreHorizontal();
            page.restoreVertical();
            tryVerify(() => Math.abs(source.width / (upper.width - 6) - 0.4) < 0.01);
            verify(timeline.height > 250);
            page.width = 1500;
            page.height = 1000;
            tryVerify(() => Math.abs(source.width / (upper.width - 6) - 0.4) < 0.01);
            page.width = 760;
            page.height = 520;
            preferences.sourceFraction = 0.99;
            preferences.timelineFraction = 0.99;
            page.restoreHorizontal();
            page.restoreVertical();
            tryVerify(() => source.width >= 220 && upper.width - source.width - 6 >= 239);
            verify(timeline.height >= 200);
            verify(upper.height >= 160);
            const horizontal = child(page, "editHorizontalSplitHandle");
            preferences.sourceFraction = 0.4;
            page.restoreHorizontal();
            horizontal.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Right);
            compare(preferences.sourceFraction, 0.43);
            const vertical = child(page, "editVerticalSplitHandle");
            preferences.timelineFraction = 0.4;
            vertical.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Up);
            compare(preferences.timelineFraction, 0.43);
            preferences.sourceFraction = NaN;
            preferences.timelineFraction = -1;
            page.restoreHorizontal();
            page.restoreVertical();
            verify(source.width >= 220);
            preferences.sourceFraction = 0.29;
            preferences.timelineFraction = 0.37;
        }

        function test_splitters_persist_across_processes() {
            if (testPersistenceStage.length === 0) return;
            const page = make();
            const preferences = child(page, "editLayoutPreferences");
            if (testPersistenceStage === "write") {
                preferences.sourceFraction = 0.41;
                preferences.timelineFraction = 0.53;
                preferences.sync();
            } else {
                compare(preferences.sourceFraction, 0.41);
                compare(preferences.timelineFraction, 0.53);
                page.width = 1500;
                page.height = 1000;
                const source = child(page, "editSourceBrowser");
                const upper = child(page, "editHorizontalSplit");
                tryVerify(() => Math.abs(source.width / (upper.width - 6) - 0.41) < 0.01);
                page.width = 760;
                page.height = 520;
                tryVerify(() => source.width >= 220 && upper.width - source.width - 6 >= 239);
            }
        }

        function test_real_media_worker_seeks_generation_and_playback() {
            if (!testMediaPreview.available)
                skip("Set EXOSNAP_EDIT_RENDER_FIXTURES for real media.");
            testMediaPreview.open();
            let frame = {};
            tryVerify(() => { frame = testMediaPreview.takeFrame(); return frame.time === 0 || testMediaPreview.error.length > 0; }, 5000);
            compare(testMediaPreview.error, "");
            const seeks = [1499, 1500, 1750, 1999, 2000, 3000, 1750, 0];
            for (const position of seeks) {
                testMediaPreview.seek(position);
                tryVerify(() => { frame = testMediaPreview.takeFrame(); return frame.time === position; }, 5000);
                compare(frame.layers, position >= 1500 && position < 2000 ? 2 : 1);
                if (position === 1750) compare(frame.weight, 0.5);
                compare(testMediaPreview.error, "");
            }
            verify(testMediaPreview.rejectsStaleFrame());
            testMediaPreview.seek(0);
            tryVerify(() => { frame = testMediaPreview.takeFrame(); return frame.time === 0; }, 5000);
            testMediaPreview.play();
            let delivered = 0;
            let overlap = false;
            let incoming = false;
            let lastTime = -1;
            let firstGeneration = 0;
            let lastGeneration = 0;
            const started = Date.now();
            tryVerify(() => {
                frame = testMediaPreview.takeFrame();
                if (frame.time !== undefined && frame.time !== lastTime) {
                    lastTime = frame.time;
                    if (!firstGeneration) firstGeneration = frame.generation;
                    lastGeneration = frame.generation;
                    ++delivered;
                    overlap = overlap || frame.layers === 2;
                    incoming = incoming || frame.time >= 2000;
                }
                return testMediaPreview.ended || testMediaPreview.error.length > 0;
            }, 12000);
            compare(testMediaPreview.error, "");
            verify(overlap && incoming);
            verify(delivered > 20);
            console.log("Crossfade worker delivery:", lastGeneration - firstGeneration + 1,
                "mailbox frames,", delivered, "polled frames in", Date.now() - started, "ms");
        }

        function test_crossfade_overlay_duration_keyboard_and_visual() {
            while (testSession.canUndo) testSession.undo();
            const asset = testSession.media[0].id;
            testSession.appendAsset(asset);
            testSession.appendAsset(asset);
            const videos = testSession.visibleClips(0, 200000).filter(row => row.video);
            verify(testSession.canCrossfade(videos[0].id));
            verify(!testSession.canCrossfade(videos[1].id));
            testSession.applyCrossfade(videos[0].id, 500);
            compare(testSession.durationMs, 199500);
            const page = make({ session: testSession, timeline: testTimeline, player: testPlayer });
            const strip = child(page, "editClipTimeline");
            strip.zoomTo(200);
            child(page, "editTimelineScroll").contentX = strip.positionAt(98500);
            const overlay = child(page, "editCrossfadeOverlay");
            overlay.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Return);
            const popup = child(page, "editCrossfadeDurationPopup");
            tryCompare(popup, "opened", true);
            const duration = child(page, "editCrossfadeDuration");
            duration.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Up);
            compare(testSession.transitions[0].durationMs, 501);
            keyClick(Qt.Key_Escape);
            tryCompare(popup, "visible", false);
            child(page, "editSourceBrowser").currentTab = 2;
            if (testVisualDirectory.length > 0) {
                let saved = false;
                verify(page.grabToImage(result => {
                    saved = result.saveToFile(testVisualDirectory + "/edit-crossfade-" + testScale + ".png");
                }, Qt.size(Math.round(page.width * Number(testScale)), Math.round(page.height * Number(testScale)))));
                tryVerify(() => saved);
            }
            child(page, "editCrossfadeOverlay").forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Delete);
            compare(testSession.transitions.length, 0);
            compare(testSession.durationMs, 200000);
            while (testSession.canUndo) testSession.undo();
        }

        function test_export_cancel_is_accessible_and_navigation_independent() {
            const page = make();
            testEditHarness.exportVisualState(EditExportAdapter.Running);
            const cancel = child(page, "editCancelExport");
            verify(cancel.visible && cancel.enabled);
            compare(cancel.Accessible.name, qsTr("Cancel export"));
            page.visible = false;
            compare(testPageExporter.state, EditExportAdapter.Running);
            page.visible = true;
            cancel.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            compare(testPageExporter.state, EditExportAdapter.Cancelling);
            verify(testPageExporter.running);
            verify(!cancel.enabled);
            compare(child(page, "editExportProgress").text, qsTr("Cancelling…"));
            testEditHarness.exportVisualState(EditExportAdapter.Options);
        }

        function test_icon_actions_data() {
            return [
                { tag: "play", objectName: "editTransportPlay", label: qsTr("Play"), shortcut: "Space", enabled: false },
                { tag: "undo", objectName: "editUndo", label: qsTr("Undo"), shortcut: "Ctrl+Z", enabled: false },
                { tag: "redo", objectName: "editRedo", label: qsTr("Redo"), shortcut: "Ctrl+Y", enabled: false },
                { tag: "split", objectName: "editSplit", label: qsTr("Split"), shortcut: "Ctrl+B", enabled: false },
                { tag: "delete", objectName: "editDelete", label: qsTr("Delete"), shortcut: "Delete", enabled: false },
                { tag: "volume", objectName: "editVolume", label: qsTr("Preview volume"), shortcut: "", enabled: true },
                { tag: "snap", objectName: "editSnapping", label: qsTr("Snapping"), shortcut: "", enabled: true },
                { tag: "export", objectName: "editExport", label: qsTr("Export — %1").arg(testPageExporter.profileOptions[0].label), shortcut: "", enabled: false }
            ];
        }

        function test_icon_actions(data) {
            const page = make();
            const action = child(page, data.objectName);
            compare(action.Accessible.name, data.label);
            compare(action.enabled, data.enabled);
            verify(action.glyph !== ExoGlyph.Invalid);
            compare(action.width, action.height);
            verify(action.ToolTip.text.indexOf(data.label) >= 0);
            if (data.shortcut.length > 0)
                verify(action.ToolTip.text.indexOf(data.shortcut) >= 0);
            const position = action.mapToItem(page, 0, 0);
            verify(position.x >= 0 && position.x + action.width <= page.width);
            verify(position.y >= 0 && position.y + action.height <= page.height);
        }

        function test_empty_sources_and_keyboard_tabs() {
            const page = make();
            const history = child(page, "editHistoryTab");
            const media = child(page, "editMediaTab");
            const transitions = child(page, "editTransitionsTab");
            const title = child(page, "editSourceEmptyTitle");
            const help = child(page, "editSourceEmptyHelp");
            const add = child(page, "editAddMedia");
            compare(title.text, qsTr("No recordings yet"));
            compare(help.text, qsTr("Recent recordings appear here automatically."));
            verify(title.visible);
            compare(add.visible, false);

            history.forceActiveFocus(Qt.TabFocusReason);
            verify(history.visualFocus);
            keyClick(Qt.Key_Tab);
            tryCompare(media, "activeFocus", true);
            keyClick(Qt.Key_Space);
            tryCompare(title, "text", qsTr("Drop media here"));
            compare(add.visible, true);
            verify(add.Accessible.name.length > 0);
            verify(add.glyph !== ExoGlyph.Invalid);
            keyClick(Qt.Key_Tab);
            tryCompare(transitions, "activeFocus", true);
            keyClick(Qt.Key_Space);
            tryCompare(title, "text", qsTr("Transitions will appear here when available."));
            verify(!title.visible);
            verify(child(page, "editCrossfadeItem").visible);
            compare(add.visible, false);
            compare(child(page, "editSourceItems").count, 0);
            keyClick(Qt.Key_Right);
            tryCompare(history, "activeFocus", true);
            compare(history.visualFocus, true);
            compare(title.text, qsTr("No recordings yet"));
            keyClick(Qt.Key_Left);
            tryCompare(transitions, "activeFocus", true);
            compare(transitions.visualFocus, true);
            compare(transitions.Accessible.role, Accessible.PageTab);
            compare(title.text, qsTr("Transitions will appear here when available."));
        }

        function test_snapping_keyboard_toggle() {
            const page = make();
            const snap = child(page, "editSnapping");
            const timeline = child(page, "editClipTimeline");
            snap.forceActiveFocus(Qt.TabFocusReason);
            compare(snap.checked, true);
            keyClick(Qt.Key_Space);
            compare(timeline.snapping, false);
            compare(snap.checked, false);
            keyClick(Qt.Key_Space);
            compare(timeline.snapping, true);
            compare(snap.checked, true);
        }

        function test_hidden_resident_page_retains_source_and_zoom() {
            const page = make();
            const media = child(page, "editMediaTab");
            const timeline = child(page, "editClipTimeline");
            media.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            timeline.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Plus, Qt.ControlModifier);
            verify(timeline.pixelsPerSecond > 40);
            const zoomBefore = timeline.pixelsPerSecond;
            page.visible = false;
            page.visible = true;
            compare(timeline.pixelsPerSecond, zoomBefore);
            compare(child(page, "editSourceEmptyTitle").text, qsTr("Drop media here"));
        }

        function test_volume_popup_keyboard_and_escape() {
            const page = make();
            const button = child(page, "editVolume");
            const popup = child(page, "editVolumePopup");
            const slider = child(page, "editVolumeSlider");
            compare(popup.visible, false);
            button.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            tryCompare(popup, "opened", true);
            tryCompare(slider, "activeFocus", true);
            for (let i = 0; i < 11; ++i) keyClick(Qt.Key_Left);
            compare(testPagePlayer.volume, 0);
            verify(button.Accessible.name.indexOf(qsTr("muted")) >= 0);
            for (let i = 0; i < 11; ++i) keyClick(Qt.Key_Right);
            compare(testPagePlayer.volume, 1);
            keyClick(Qt.Key_Escape);
            tryCompare(popup, "visible", false);
            tryCompare(button, "activeFocus", true);
        }

        function test_export_profile_menu_contains_only_supported_profiles() {
            const page = make();
            const button = child(page, "editExportProfiles");
            const menu = child(page, "editExportProfileMenu");
            verify(button.Accessible.name.length > 0);
            button.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            tryCompare(menu, "opened", true);
            compare(menu.count, testPageExporter.profileOptions.filter(option => option.selectable).length);
            compare(menu.itemAt(0).text, testPageExporter.profileOptions[0].label);
            verify(menu.itemAt(0).checked);
            keyClick(Qt.Key_Escape);
            tryCompare(menu, "visible", false);
        }

        function test_source_tabs_scroll_without_truncating_labels() {
            const page = make();
            const browser = child(page, "editSourceBrowser");
            const scroll = child(page, "editSourceTabs");
            const history = child(page, "editHistoryTab");
            const transitions = child(page, "editTransitionsTab");
            transitions.text = "Transitions mit langem Namen";
            tryVerify(() => scroll.contentWidth > scroll.width);
            browser.focusTab(2);
            tryVerify(() => transitions.x >= scroll.contentX
                && transitions.x + transitions.width <= scroll.contentX + scroll.width + 1);
            compare(transitions.contentItem.truncated, false);
            verify(transitions.visualFocus);
            browser.focusTab(0);
            tryCompare(scroll, "contentX", 0);
            verify(history.visualFocus);
            compare(history.contentItem.truncated, false);
        }
        function test_transport_and_undo_actions() {
            while (testSession.canUndo)
                testSession.undo();
            testSession.appendAsset(testSession.media[0].id);
            const page = make({ session: testSession, timeline: testTimeline, player: testPlayer });
            verify(testSession.durationMs > 0);
            compare(testPlayer.clipOpen, false);
            compare(child(page, "editTransportPlay").enabled, false);
            page.player = testTransportPlayer;
            compare(testTransportPlayer.clipOpen, true);
            compare(child(page, "editTransportPlay").enabled, true);
            page.player = testPlayer;
            const timeline = child(page, "editClipTimeline");
            testSession.requestSeek(5000);
            timeline.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Left, Qt.ShiftModifier);
            compare(testSession.positionMs, 4000);
            keyClick(Qt.Key_Home);
            compare(testSession.positionMs, 0);

            child(page, "editUndo").forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            compare(testSession.durationMs, 0);
            child(page, "editRedo").forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            compare(testSession.durationMs, 100000);
            while (testSession.canUndo)
                testSession.undo();
        }
    }
}
