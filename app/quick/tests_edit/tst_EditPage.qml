import QtQuick
import QtQuick.Controls
import QtTest
import ExoSnap.Quick.EditTestControls

Item {
    id: root
    width: 860
    height: 700

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
            shell: testPageShell
            Rectangle { anchors.fill: parent; z: -1; color: ExoTheme.background }
        }
    }

    TestCase {
        name: "EditPage"
        when: windowShown

        function make(properties) {
            const page = createTemporaryObject(pageComponent, root, Object.assign({mediaPinPreferred: false, detailsPinPreferred: false}, properties || {}));
            verify(!!page, "Component exists");
            return page;
        }

        function child(page, name) {
            const item = findChild(page, name);
            verify(!!item, "Object exists");
            return item;
        }

        function test_toolbar_pointer_preserves_timeline_space_and_keyboard_button_activation() {
            testTransportPlayer.setPlaying(false);
            const page = make({session: testSession, timeline: testTimeline, player: testTransportPlayer, width: 1280});
            const clips = child(page, "editClipTimeline");
            const snapping = child(page, "editSnapping");
            clips.forceActiveFocus();
            const before = testEditHarness.transportCommand(testTransportPlayer);
            mouseClick(snapping);
            verify(clips.activeFocus);
            keyPress(Qt.Key_Space);
            compare(testEditHarness.transportCommand(testTransportPlayer), before);
            keyRelease(Qt.Key_Space);
            compare(testEditHarness.transportCommand(testTransportPlayer), before + 1);
            snapping.forceActiveFocus(Qt.TabFocusReason);
            const checked = snapping.checked;
            const commands = testEditHarness.transportCommand(testTransportPlayer);
            keyClick(Qt.Key_Space);
            compare(snapping.checked, !checked);
            compare(testEditHarness.transportCommand(testTransportPlayer), commands);
            testSession.snapping = true;
            testTransportPlayer.setPlaying(false);
        }
        function test_synthetic_still_does_not_claim_native_presentation() {
            const still = testEditHarness.syntheticStillProbe();
            compare(still.source.toString(), "file:///synthetic-visual-frame.png");
            compare(still.requestedTimeUs, 42500000);
            compare(still.hasPresentedTime, false);
            compare(still.seekPending, false);
            compare(still.clipOpen, true);
            compare(still.playing, false);
        }

        function test_pointer_panel_close_restores_timeline_transport_data() {
            return [
                { tag: "media-footer", panel: "Media", header: false, keyboardFocus: false },
                { tag: "media-header", panel: "Media", header: true, keyboardFocus: false },
                { tag: "details-footer", panel: "Details", header: false, keyboardFocus: false },
                { tag: "details-header", panel: "Details", header: true, keyboardFocus: false },
                { tag: "media-footer-after-keyboard", panel: "Media", header: false, keyboardFocus: true },
                { tag: "media-header-after-keyboard", panel: "Media", header: true, keyboardFocus: true },
                { tag: "details-footer-after-keyboard", panel: "Details", header: false, keyboardFocus: true },
                { tag: "details-header-after-keyboard", panel: "Details", header: true, keyboardFocus: true }
            ];
        }

        function test_pointer_panel_close_restores_timeline_transport(data) {
            testTransportPlayer.setPlaying(false);
            const page = make({ session: testSession, timeline: testTimeline, player: testTransportPlayer });
            const clips = child(page, "editClipTimeline");
            const access = child(page, "edit" + data.panel + "Access");
            clips.forceActiveFocus();
            mouseClick(access);
            tryCompare(access, "expanded", true);
            const close = data.header
                ? child(page, "edit" + data.panel + "Pin").parent.children.find(control => control.text === (data.panel === "Media" ? qsTr("Close Media") : qsTr("Close Details")))
                : access;
            verify(!!close);
            if (data.keyboardFocus)
                close.forceActiveFocus(Qt.TabFocusReason);
            mouseClick(close);
            tryCompare(access, "expanded", false);
            verify(clips.activeFocus);
            const before = testEditHarness.transportCommand(testTransportPlayer);
            keyPress(Qt.Key_Space);
            compare(testEditHarness.transportCommand(testTransportPlayer), before);
            keyRelease(Qt.Key_Space);
            compare(testEditHarness.transportCommand(testTransportPlayer), before + 1);
            compare(access.expanded, false);
            testTransportPlayer.setPlaying(false);
        }

        function test_keyboard_panel_close_restores_invoker_data() {
            return [
                { tag: "media-footer", panel: "Media", close: "footer" },
                { tag: "media-header", panel: "Media", close: "header" },
                { tag: "media-escape", panel: "Media", close: "escape" },
                { tag: "details-footer", panel: "Details", close: "footer" },
                { tag: "details-header", panel: "Details", close: "header" },
                { tag: "details-escape", panel: "Details", close: "escape" }
            ];
        }

        function test_keyboard_panel_close_restores_invoker(data) {
            const page = make();
            const access = child(page, "edit" + data.panel + "Access");
            access.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            tryCompare(access, "expanded", true);
            if (data.close === "header") {
                const close = child(page, "edit" + data.panel + "Pin").parent.children.find(control => control.text === (data.panel === "Media" ? qsTr("Close Media") : qsTr("Close Details")));
                verify(!!close);
                close.forceActiveFocus(Qt.TabFocusReason);
            }
            keyClick(data.close === "escape" ? Qt.Key_Escape : Qt.Key_Space);
            tryCompare(access, "expanded", false);
            verify(access.activeFocus);
            keyClick(Qt.Key_Space);
            tryCompare(access, "expanded", true);
        }

        function test_harness_fit_completes_before_returning_on_settled_geometry() {
            while (testSession.canUndo) testSession.undo();
            testSession.appendAsset(testSession.media[0].id);
            const page = make({ session: testSession, timeline: testTimeline, player: testPlayer });
            const timeline = child(page, "editClipTimeline");
            const scroll = child(page, "editTimelineScroll");
            page.width = 1600;
            tryVerify(() => scroll.width > 1200);
            wait(0);
            timeline.zoomTo(timeline.minimumZoom * 2);
            verify(timeline.pixelsPerSecond > timeline.minimumZoom);
            page.applyHarnessView({ fit: true });
            verify(Math.abs(timeline.pixelsPerSecond - timeline.minimumZoom) < 0.001);
            compare(scroll.contentX, 0);
            verify(timeline.positionAt(testSession.durationMs) <= scroll.width);
            while (testSession.canUndo) testSession.undo();
        }

        function test_EmptyViewerStable() {
            const page = make();
            const viewer = child(page, "editViewer");
            const card = child(page, "editTimelineZone");
            compare(page.mediaOpen, false);
            compare(page.detailsOpen, false);
            compare(testPageSession.media.length, 0);
            compare(testPageSession.durationMs, 0);
            verify(child(page, "editEmptyViewerTitle").visible);
            tryVerify(() => viewer.width > 0 && card.height >= 220);
            const geometry = [viewer.x, viewer.y, viewer.width, viewer.height, card.x, card.y, card.width, card.height];
            while (testSession.canUndo) testSession.undo();
            testSession.appendAsset(testSession.media[0].id);
            page.session = testSession;
            wait(0);
            compare([viewer.x, viewer.y, viewer.width, viewer.height, card.x, card.y, card.width, card.height], geometry);
            while (testSession.canUndo) testSession.undo();
        }

        function test_ToolbarAtFourSizes_data() {
            return [
                { tag: "860", width: 860, height: 700 },
                { tag: "1280", width: 1280, height: 800 },
                { tag: "1600", width: 1600, height: 1000 },
                { tag: "1920", width: 1920, height: 1080 }
            ];
        }

        function test_ToolbarAtFourSizes(data) {
            const page = make({ width: data.width, height: data.height });
            const header = child(page, "editTimelineHeader");
            const actions = child(page, "editActionGroup");
            const transport = child(page, "editTransportGroup");
            const output = child(page, "editOutputGroup");
            verify(!findChild(page, "editTimelineFooter"));
            tryVerify(() => actions.x + actions.width <= transport.x);
            verify(transport.x + transport.width <= output.x);
            verify(output.x + output.width <= header.width);
            verify(header.height >= 46);
            if (data.width >= 1000) compare(Math.round(transport.x + transport.width / 2), Math.round(header.width / 2));
            const minimum = {editExport: 32, editMediaAccess: 32, editDetailsAccess: 32, editMixAccess: 32, editZoomOut: 26, editZoomIn: 26, editZoomFit: 26};
            for (const name of ["editUndo", "editRedo", "editSplit", "editDelete", "editTransportPlay", "editVolume", "editExport", "editMediaAccess", "editDetailsAccess", "editMixAccess", "editSnapping", "editRipple", "editAutoCrossfade", "editLinkedSelection", "editZoomOut", "editZoomIn", "editZoomFit"]) {
                const control = child(page, name);
                if (!control.visible)
                    continue;
                const p = control.mapToItem(page, 0, 0);
                const size = minimum[name] ?? 34;
                verify(control.width >= size && control.height >= Math.min(size, 32), name);
                verify(p.x >= 0 && p.x + control.width <= page.width, name);
                verify(p.y >= 0 && p.y + control.height <= page.height, name);
            }
        }

        function test_ActionTogglePanelDistinct() {
            const page = make();
            const action = child(page, "editUndo");
            const toggle = child(page, "editSnapping");
            const panel = child(page, "editMediaAccess");
            compare(action.checkable, false);
            compare(action.panelAccess, false);
            compare(toggle.checkable, true);
            compare(toggle.checked, true);
            compare(toggle.Accessible.checkable, true);
            compare(toggle.Accessible.checked, true);
            compare(panel.panelAccess, true);
            compare(panel.checkable, false);
            compare(panel.expanded, false);
            panel.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            tryCompare(panel, "expanded", true);
            compare(panel.Accessible.name, qsTr("Media"));
            verify(panel.Accessible.description.length > 0);
            verify(child(page, "editMediaPanel").visible);
            child(page, "editDetailsAccess").forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            tryCompare(page, "mediaOpen", false);
            compare(page.detailsOpen, true);
        }
        function test_FourIndependentBehaviorControls() {
            const page = make({width: 1280});
            const session = page.session;
            session.snapping = true;
            session.ripple = false;
            session.autoCrossfade = true;
            session.linkedSelection = true;
            for (const data of [
                {name: "editSnapping", property: "snapping", initial: true},
                {name: "editRipple", property: "ripple", initial: false},
                {name: "editAutoCrossfade", property: "autoCrossfade", initial: true},
                {name: "editLinkedSelection", property: "linkedSelection", initial: true}
            ]) {
                const control = child(page, data.name);
                compare(control.checked, data.initial);
                verify(control.ToolTip.text.length > 0);
                control.forceActiveFocus(Qt.TabFocusReason);
                keyClick(Qt.Key_Space);
                compare(session[data.property], !data.initial);
                keyClick(Qt.Key_Space);
                compare(session[data.property], data.initial);
            }
        }

        function test_PinIsExplicit() {
            const page = make({ width: 1280, height: 800 });
            const viewer = child(page, "editViewer");
            const fullWidth = viewer.width;
            page.toggleMedia();
            tryVerify(() => !!findChild(page, "editMediaPin"));
            compare(page.mediaPinned, false);
            compare(viewer.width, fullWidth);
            const pin = child(page, "editMediaPin");
            pin.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            tryCompare(page, "mediaPinned", true);
            verify(viewer.width < fullWidth);
            verify(viewer.width >= 480);
            page.width = 1000;
            tryVerify(() => viewer.width >= 480);
            compare(page.mediaPinned, true);
            page.width -= 1;
            tryCompare(page, "mediaPinned", false);
            compare(page.canPinMedia, false);
            page.width += 1;
            tryCompare(page, "canPinMedia", true);
            compare(page.mediaPinned, true);
            page.width = 760;
            tryCompare(page, "mediaPinned", false);
            page.width = 1280;
            compare(page.mediaPinned, true);
            page.toggleMedia();
            compare(page.mediaOpen, false);
            page.width = 1600;
            compare(page.mediaOpen, false);
        }

        function test_panel_layout_is_published_and_requests_use_the_page_rules() {
            const page = make({ width: 1100, height: 800 });
            tryVerify(() => testPageShell.editPanels.mediaOpen === false);
            compare(testPageShell.editPanels.detailsOpen, false);
            compare(testPageShell.editPanels.sourceTab, 0);

            testEditHarness.requestEditPanel(ShellAdapter.EditDetailsPanel, true, false);
            compare(page.detailsOpen, true);
            tryVerify(() => testPageShell.editPanels.detailsOpen === true);
            // Below 1200 px one overlay at a time, exactly as the footer button does.
            testEditHarness.requestEditPanel(ShellAdapter.EditMediaPanel, true, false);
            compare(page.mediaOpen, true);
            compare(page.detailsOpen, false);
            tryVerify(() => testPageShell.editPanels.detailsOpen === false);

            testEditHarness.requestEditPanel(ShellAdapter.EditMediaPanel, true, true);
            compare(page.mediaPinned, page.canPinMedia);
            tryVerify(() => testPageShell.editPanels.mediaPinned === page.canPinMedia);
            compare(testPageShell.editPanels.mediaCanPin, page.canPinMedia);

            testEditHarness.requestEditSourceTab(ShellAdapter.EditTransitionsTab);
            compare(page.sourceTab, 2);
            tryVerify(() => child(page, "editHistoryTab").checked);
            compare(testPageShell.editPanels.sourceTab, 2);

            testEditHarness.requestEditPanel(ShellAdapter.EditMediaPanel, false, false);
            compare(page.mediaOpen, false);
            compare(page.mediaPinned, false);
            tryVerify(() => testPageShell.editPanels.mediaOpen === false);
            testEditHarness.requestEditSourceTab(ShellAdapter.EditHistoryTab);
        }

        function test_dock_pin_preference_restores_after_temporary_overlay() {
            const page = make({width: 1600, height: 800});
            mouseClick(child(page, "editMediaAccess"));
            mouseClick(child(page, "editMediaPin"));
            tryCompare(page, "mediaPinned", true);
            page.width = 860;
            tryCompare(page, "mediaPinned", false);
            page.width = 1600;
            tryCompare(page, "mediaPinned", true);
        }

        function test_dock_width_keyboard_resize_and_reset() {
            const page = make({width: 1600, height: 800});
            mouseClick(child(page, "editMediaAccess"));
            const divider = child(page, "editMediaResize");
            const panel = child(page, "editMediaPanelLoader");
            divider.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Home);
            tryCompare(panel, "width", 300);
            keyClick(Qt.Key_Right);
            tryCompare(panel, "width", 308);
            keyClick(Qt.Key_Left, Qt.ShiftModifier);
            tryCompare(panel, "width", 276);
            keyClick(Qt.Key_Home);
            tryCompare(panel, "width", 300);
        }

        function test_inspector_follows_clip_and_range_context_without_export_or_storage() {
            while (testSession.canUndo) testSession.undo();
            testSession.appendAsset(testSession.media[0].id);
            const page = make({session: testSession, width: 1280, height: 800});
            const clips = testSession.visibleClipsUs(0, testSession.durationUs);
            verify(clips.length > 0);
            const linked = testSession.linkedSelection;
            testSession.linkedSelection = false;
            testSession.clearSelection();
            testSession.selectClipWithModifiers(clips[0].id);
            compare(testSession.selectedClipIds.length, 1);
            page.toggleDetails();
            const start = child(page, "editDetailsClipStart");
            verify(start.visible);
            const inspector = child(page, "editInspector");
            verify(!findChild(inspector, "editExportProfiles"));
            verify(!findChild(inspector, "editClearMediaCache"));
            verify(testSession.setEditRangeUs(1000000, 2000000, [testSession.tracks[0].id]));
            tryVerify(() => !start.visible);
            verify(child(page, "editDetailsRangeStart").visible);
            testSession.clearSelection();
            testSession.linkedSelection = linked;
            while (testSession.canUndo) testSession.undo();
        }

        function test_narrow_editing_controls_remain_reachable_through_overflow() {
            const page = make({width: 860});
            const linked = child(page, "editLinkedSelection");
            verify(!linked.visible);
            const before = testPageSession.linkedSelection;
            mouseClick(child(page, "editToolbarOverflow"));
            const action = child(page, "editOverflowLinkedSelection");
            tryVerify(() => action.visible);
            mouseClick(action);
            compare(testPageSession.linkedSelection, !before);
            testPageSession.linkedSelection = before;
        }

        function test_status_reports_the_active_range_without_transport_changes() {
            testSession.appendAsset(testSession.media[0].id);
            const page = make({session: testSession, width: 1280, height: 800});
            const position = testSession.positionUs;
            verify(testSession.setEditRangeUs(1000000, 2000000, [testSession.tracks[0].id]));
            const readout = child(page, "editRangeReadout");
            tryVerify(() => readout.text.length > 0);
            verify(readout.text.indexOf("undefined") < 0);
            verify(!child(page, "editSelectionStatus").visible);
            compare(testSession.positionUs, position);
            testSession.clearSelection();
            while (testSession.canUndo) testSession.undo();
        }

        function test_inspector_multi_audio_gain_applies_one_undoable_compatible_target_set() {
            const session = testClipboardSession;
            while (session.canUndo) session.undo();
            session.appendAsset(session.media[0].id);
            session.appendAsset(session.media[0].id);
            const audio = session.visibleClipsUs(0, session.durationUs).filter(clip => !clip.video);
            compare(audio.length, 2);
            verify(session.setClipGain(audio[0].id, -2));
            const linked = session.linkedSelection;
            session.linkedSelection = false;
            session.clearSelection();
            session.selectClipWithModifiers(audio[0].id);
            session.selectClipWithModifiers(audio[1].id, true);
            const page = make({session: session});
            page.toggleDetails();
            const field = child(page, "editDetailsMultiGain");
            verify(field.visible && field.enabled);
            verify(field.mixed);
            compare(field.targetIds.length, 2);
            const inspector = child(page, "editInspector");
            verify(inspector.revealItem(field));
            verify(waitForPolish(root.Window.window));
            mouseClick(field);
            tryCompare(field, "activeFocus", true);
            field.selectAll();
            const text = "1.3";
            for (let index = 0; index < text.length; ++index)
                keyClick(text[index]);
            keyClick(Qt.Key_Return);
            compare(session.selectedClipSummary.gainMixed, false);
            compare(session.selectedClipSummary.gainDb, 1.3);
            session.undo();
            compare(session.selectedClipSummary.gainMixed, true);
            session.clearSelection();
            session.linkedSelection = linked;
            while (session.canUndo) session.undo();
        }

        function test_vertical_split_restore_clamp_and_keyboard() {
            const page = make();
            const preferences = child(page, "editLayoutPreferences");
            const timeline = child(page, "editTimelineZone");
            preferences.timelineFraction = 0.55;
            page.restoreVertical();
            tryVerify(() => timeline.height > 250);
            page.height = 520;
            preferences.timelineFraction = 0.99;
            page.restoreVertical();
            verify(timeline.height >= 220);
            verify(child(page, "editViewerStage").height >= 160);
            const vertical = child(page, "editVerticalSplitHandle");
            preferences.timelineFraction = 0.4;
            vertical.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Up);
            compare(preferences.timelineFraction, 0.43);
            preferences.timelineFraction = -1;
            page.restoreVertical();
            verify(timeline.height >= 220);
            preferences.timelineFraction = 0.37;
        }

        function test_splitters_persist_across_processes() {
            if (testPersistenceStage.length === 0)
                skip("Set EXOSNAP_EDIT_PERSISTENCE_STAGE for explicit write/read processes.");
            const page = createTemporaryObject(pageComponent, root, {width: 1600, height: 1000});
            verify(!!page);
            const preferences = child(page, "editLayoutPreferences");
            compare(preferences.location, testPageSession.layoutSettingsLocation);
            if (testPersistenceStage === "write") {
                preferences.sourceFraction = 0.41;
                preferences.timelineFraction = 0.53;
                preferences.mediaWidth = 344;
                preferences.detailsWidth = 376;
                preferences.trackHeaderWidth = 208;
                preferences.mediaPinned = true;
                preferences.detailsPinned = true;
                preferences.sync();
                compare(preferences.sourceFraction, 0.41);
                compare(preferences.timelineFraction, 0.53);
            } else {
                compare(preferences.sourceFraction, 0.41);
                compare(preferences.timelineFraction, 0.53);
                compare(preferences.mediaWidth, 344);
                compare(preferences.detailsWidth, 376);
                compare(preferences.trackHeaderWidth, 208);
                verify(page.mediaPinPreferred && page.detailsPinPreferred);
                compare(page.mediaOpen, false);
                compare(page.detailsOpen, false);
                page.toggleMedia();
                page.toggleDetails();
                verify(page.mediaPinned && page.detailsPinned);
                compare(page.mediaWidth, 344);
                compare(page.detailsWidth, 376);
                compare(child(page, "editClipTimeline").userHeaderWidth, 208);
                page.height = 1000;
                tryVerify(() => child(page, "editTimelineZone").height > 450);
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
                // The incoming clip has zero weight at the first instant of the overlap.
                compare(frame.layers, position > 1500 && position < 2000 ? 2 : 1);
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
            strip.view.setVisibleInterval(98500000, 101000000);
            const overlay = child(page, "editCrossfadeOverlay");
            overlay.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Return);
            const popup = child(page, "editCrossfadeDurationPopup");
            tryCompare(popup, "opened", true);
            const duration = child(page, "editCrossfadeDuration");
            duration.forceActiveFocus(Qt.TabFocusReason);
            const positions = testSession.visibleClips(0, 200000).map(clip => clip.startMs);
            compare(duration.valueUs, 500000);
            duration.selectAll();
            const typedDuration = "0.4s";
            for (let index = 0; index < typedDuration.length; ++index)
                keyClick(typedDuration[index]);
            keyClick(Qt.Key_Return);
            compare(testSession.transitions[0].durationUs, 400000);
            compare(duration.valueUs, 400000);
            compare(testSession.visibleClips(0, 200000).map(clip => clip.startMs), positions);
            keyClick(Qt.Key_Escape);
            tryCompare(popup, "visible", false);
            page.toggleMedia();
            child(page, "editSourceBrowser").currentTab = 2;
            if (testVisualDirectory.length > 0) {
                let saved = false;
                verify(page.grabToImage(result => {
                    saved = result.saveToFile(testVisualDirectory + "/edit-crossfade-" + testScale + ".png");
                }, Qt.size(Math.round(page.width * Number(testScale)), Math.round(page.height * Number(testScale)))));
                tryVerify(() => saved);
                testSession.selectClip(videos[1].id);
                page.toggleDetails();
                tryVerify(() => findChild(page, "editDetailsFadeIn") !== null);
                saved = false;
                verify(page.grabToImage(result => {
                    saved = result.saveToFile(testVisualDirectory + "/edit-details-" + testScale + ".png");
                }, Qt.size(Math.round(page.width * Number(testScale)), Math.round(page.height * Number(testScale)))));
                tryVerify(() => saved);
                page.toggleDetails();
            }
            child(page, "editCrossfadeOverlay").forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Delete);
            compare(testSession.durationMs, 199500);
            compare(testSession.transitions[0].kind, "hardCut");
            while (testSession.canUndo) testSession.undo();
        }

        function test_export_cancel_is_accessible_and_navigation_independent() {
            const page = make();
            testEditHarness.exportVisualState(EditExportAdapter.Running);
            page.openExport();
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

        function test_export_is_a_right_panel_context_that_keeps_its_job() {
            const empty = make();
            verify(!child(empty, "editExport").enabled);
            empty.openExport();
            compare(empty.detailsOpen, false);
            testSession.appendAsset(testSession.media[0].id);
            const page = make({session: testSession, width: 1280, height: 800});
            const rail = child(page, "editExport");
            verify(rail.enabled);
            child(page, "editClipTimeline").forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_E, Qt.ControlModifier);
            tryCompare(page, "exportVisible", true);
            tryVerify(() => child(page, "editExportFormat").activeFocus);
            mouseClick(child(page, "editMixAccess"));
            compare(page.detailsOpen, true);
            compare(page.rightContext, "mix");
            verify(child(page, "editMixPanel").visible);
            testEditHarness.exportVisualState(EditExportAdapter.Done);
            verify(testPageExporter.resultUnseen);
            verify(child(page, "editExportRailDot").visible);
            compare(rail.Accessible.name, qsTr("Export complete"));
            mouseClick(rail);
            compare(page.rightContext, "export");
            tryCompare(testPageExporter, "resultUnseen", false);
            verify(!child(page, "editExportRailDot").visible);
            compare(child(page, "editExportPrimary").text, qsTr("Show in folder"));
            mouseClick(child(page, "editDetailsAccess"));
            compare(page.rightContext, "details");
            compare(testPageExporter.state, EditExportAdapter.Done);
            testEditHarness.exportVisualState(EditExportAdapter.Options);
            while (testSession.canUndo) testSession.undo();
        }

        function test_empty_timeline_keeps_export_panel_closable_data() {
            return [{tag: "escape"}, {tag: "close"}, {tag: "rail"}, {tag: "shortcut"}];
        }

        function test_empty_timeline_keeps_export_panel_closable(data) {
            while (testSession.canUndo) testSession.undo();
            testEditHarness.exportVisualState(EditExportAdapter.Options);
            testSession.appendAsset(testSession.media[0].id);
            const page = make({session: testSession, width: 1280, height: 800});
            const timeline = child(page, "editClipTimeline");
            const rail = child(page, "editExport");
            try {
                verify(page.canOpenExport);
                timeline.forceActiveFocus(Qt.TabFocusReason);
                keyClick(Qt.Key_E, Qt.ControlModifier);
                tryCompare(page, "exportVisible", true);
                testSession.selectAllClips();
                testSession.deleteSelected(false);
                compare(testSession.durationUs, 0);
                compare(page.canOpenExport, false);
                const close = child(page, "editDetailsPanel").closeButton;
                close.forceActiveFocus(Qt.TabFocusReason);
                verify(close.activeFocus);
                if (data.tag === "escape")
                    keyClick(Qt.Key_Escape);
                else if (data.tag === "close")
                    mouseClick(close);
                else if (data.tag === "rail") {
                    verify(rail.enabled);
                    mouseClick(rail);
                } else
                    keyClick(Qt.Key_E, Qt.ControlModifier);
                tryCompare(page, "detailsOpen", false);
                tryVerify(() => timeline.activeFocus);
                verify(!rail.enabled);
                page.openExport();
                compare(page.detailsOpen, false);
                keyClick(Qt.Key_E, Qt.ControlModifier);
                compare(page.detailsOpen, false);
                page.toggleRight("mix", true);
                compare(page.rightContext, "mix");
                verify(page.detailsOpen);
                page.openExport();
                compare(page.rightContext, "mix");
                verify(page.detailsOpen);
            } finally {
                while (testSession.canUndo) testSession.undo();
            }
        }

        function test_keyboard_close_returns_focus_without_activating_a_rail() {
            testSession.appendAsset(testSession.media[0].id);
            const page = make({session: testSession, width: 1280, height: 800});
            const timeline = child(page, "editClipTimeline");
            // Ctrl+D and Ctrl+E from the timeline hand focus back to it, so a
            // following Space reaches the transport instead of a rail button.
            for (const key of [Qt.Key_D, Qt.Key_E]) {
                timeline.forceActiveFocus(Qt.TabFocusReason);
                keyClick(key, Qt.ControlModifier);
                tryCompare(page, "detailsOpen", true);
                keyClick(key, Qt.ControlModifier);
                compare(page.detailsOpen, false);
                tryVerify(() => timeline.activeFocus);
                keyClick(Qt.Key_Space);
                compare(page.detailsOpen, false);
                verify(timeline.activeFocus);
            }
            // Escape closes the context that is shown instead of switching to
            // Details, and a rail opened from the keyboard gets focus back.
            const mixRail = child(page, "editMixAccess");
            mixRail.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            tryCompare(page, "detailsOpen", true);
            compare(page.rightContext, "mix");
            keyClick(Qt.Key_Escape);
            compare(page.detailsOpen, false);
            tryVerify(() => mixRail.activeFocus);
            // An invoker hidden since opening cannot take focus; the rail does.
            page.toggleMedia();
            const browser = child(page, "editSourceBrowser");
            browser.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_D, Qt.ControlModifier);
            tryCompare(page, "detailsOpen", true);
            page.toggleMedia();
            verify(!browser.visible);
            page.toggleDetails(true);
            compare(page.detailsOpen, false);
            tryVerify(() => child(page, "editDetailsAccess").activeFocus);
            while (testSession.canUndo) testSession.undo();
        }

        function test_hidden_export_review_is_announced_on_the_rail() {
            const page = make();
            testEditHarness.exportVisualState(EditExportAdapter.Preparing);
            page.openExport();
            verify(page.exportVisible);
            page.openExport();
            verify(!page.exportVisible);
            testEditHarness.exportVisualState(EditExportAdapter.Review);
            verify(child(page, "editExportRailDot").visible);
            const originalMode = testPageExporter.modeKey;
            testPageExporter.modeKey = originalMode === "finishedStereo" ? "originalStreams" : "finishedStereo";
            compare(testPageExporter.state, EditExportAdapter.Options);
            verify(!child(page, "editExportRailDot").visible);
            testPageExporter.modeKey = originalMode;
            testEditHarness.exportVisualState(EditExportAdapter.Review);
            page.openExport();
            verify(page.exportVisible);
            verify(!child(page, "editExportRailDot").visible);
            testEditHarness.exportVisualState(EditExportAdapter.Options);
        }

        function test_escape_from_a_focused_field_without_a_draft_closes_its_panel() {
            testSession.appendAsset(testSession.media[0].id);
            const page = make({session: testSession, width: 1280, height: 800});
            child(page, "editClipTimeline").forceActiveFocus();
            page.toggleRight("mix", true);
            const audio = testSession.tracks.find(track => !track.video);
            const gain = child(page, "editMixGain" + audio.id);
            gain.forceActiveFocus();
            verify(gain.draftActive);
            verify(gain.commitPendingDraft());
            verify(gain.activeFocus && !gain.draftActive);
            keyClick(Qt.Key_Escape);
            compare(page.detailsOpen, false);
            while (testSession.canUndo) testSession.undo();
        }

        function test_mix_rows_edit_the_same_track_values_as_details() {
            testSession.appendAsset(testSession.media[0].id);
            const page = make({session: testSession, width: 1280, height: 800});
            const audio = testSession.tracks.find(track => !track.video);
            verify(audio !== undefined);
            mouseClick(child(page, "editMixAccess"));
            const mute = child(page, "editMixMute" + audio.id);
            mute.forceActiveFocus();
            mute.clicked();
            compare(testSession.tracks.find(track => track.id === audio.id).enabled, false);
            compare(child(page, "editMixMute" + audio.id), mute);
            verify(mute.activeFocus);
            testSession.undo();
            compare(testSession.tracks.find(track => track.id === audio.id).enabled, true);
            const solo = child(page, "editMixSolo" + audio.id);
            const undoable = testSession.canUndo;
            solo.clicked();
            verify(testSession.tracks.find(track => track.id === audio.id).solo);
            compare(testSession.canUndo, undoable);
            solo.clicked();
            const fader = child(page, "editMixFader" + audio.id);
            const originalGain = testSession.tracks.find(track => track.id === audio.id).gainDb;
            mousePress(fader, fader.width * 0.4, fader.height / 2, Qt.LeftButton);
            mouseMove(fader, fader.width * 0.7, fader.height / 2);
            verify(fader.pressed);
            const heldGain = fader.value;
            testSession.setTrackSolo(audio.id, true);
            compare(child(page, "editMixFader" + audio.id), fader);
            verify(fader.pressed);
            compare(fader.value, heldGain);
            mouseRelease(fader, fader.width * 0.7, fader.height / 2, Qt.LeftButton);
            compare(testSession.tracks.find(track => track.id === audio.id).gainDb, Math.round(heldGain * 10) / 10);
            testSession.undo();
            compare(testSession.tracks.find(track => track.id === audio.id).gainDb, originalGain);
            testSession.setTrackSolo(audio.id, false);
            while (testSession.canUndo) testSession.undo();
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
                { tag: "export", objectName: "editExport", label: qsTr("Export"), shortcut: "Ctrl+E", enabled: false, rail: true }
            ];
        }

        function test_icon_actions(data) {
            const page = make();
            const action = child(page, data.objectName);
            compare(action.Accessible.name, data.label);
            compare(action.enabled, data.enabled);
            verify(action.glyph !== ExoGlyph.Invalid);
            verify(action.width >= (data.rail ? 32 : 34) && action.height >= 32);
            verify(action.ToolTip.text.indexOf(data.label) >= 0);
            if (data.shortcut.length > 0)
                verify(action.ToolTip.text.indexOf(data.shortcut) >= 0);
            const position = action.mapToItem(page, 0, 0);
            verify(position.x >= 0 && position.x + action.width <= page.width);
            verify(position.y >= 0 && position.y + action.height <= page.height);
        }

        function test_empty_sources_and_keyboard_tabs() {
            const page = make();
            page.toggleMedia();
            const project = child(page, "editMediaTab");
            const folders = child(page, "editFoldersTab");
            const recordings = child(page, "editHistoryTab");
            const title = child(page, "editSourceEmptyTitle");
            const help = child(page, "editSourceEmptyHelp");
            const add = child(page, "editAddMedia");
            compare(project.text, qsTr("Project"));
            compare(folders.text, qsTr("Folders"));
            compare(recordings.text, qsTr("Recordings"));
            compare(title.text, qsTr("Drop media here"));
            compare(help.text, qsTr("Add files or a folder with +."));
            verify(title.visible);
            verify(add.visible);
            project.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Right);
            tryCompare(folders, "activeFocus", true);
            compare(title.text, qsTr("No folders added"));
            keyClick(Qt.Key_Right);
            tryCompare(recordings, "activeFocus", true);
            compare(title.text, qsTr("No recordings yet"));
            keyClick(Qt.Key_Right);
            tryCompare(project, "activeFocus", true);
            compare(title.text, qsTr("Drop media here"));
            verify(project.visualFocus);
            compare(findChild(page, "editTransitionsTab"), null);
        }

        function test_browser_add_menu_exposes_files_and_folders_and_closes_with_escape() {
            const page = make();
            page.toggleMedia();
            const add = child(page, "editAddMedia");
            const menu = child(page, "editSourceAddMenu");
            add.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            tryCompare(menu, "opened", true);
            compare(child(page, "editSourceAddFiles").text, qsTr("Add files"));
            compare(child(page, "editSourceAddFolder").text, qsTr("Add folder"));
            keyClick(Qt.Key_Escape);
            tryCompare(menu, "visible", false);
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
            page.toggleMedia();
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

        function test_export_profile_selector_marks_unsupported_profiles() {
            testSession.appendAsset(testSession.media[0].id);
            const page = make({session: testSession});
            page.openExport();
            const profiles = child(page, "editExportProfiles");
            verify(profiles.Accessible.name.length > 0);
            compare(profiles.count, testPageExporter.profileOptions.length);
            compare(profiles.currentValue, testPageExporter.profileKey);
            while (testSession.canUndo) testSession.undo();
        }
        function test_export_intent_and_range_are_explicit_choices() {
            testSession.appendAsset(testSession.media[0].id);
            const page = make({session: testSession});
            page.openExport();
            const original = child(page, "editExportOriginalStreams");
            const finished = child(page, "editExportFinishedStereo");
            const span = child(page, "editExportSpan");
            compare(testPageExporter.modeKey, "finishedStereo");
            compare(original.checked, false);
            compare(finished.checked, true);
            compare(span.currentValue, "entire");
            original.clicked();
            compare(testPageExporter.modeKey, "originalStreams");
            compare(finished.checked, false);
            finished.clicked();
            compare(testPageExporter.modeKey, "finishedStereo");
            compare(original.checked, false);
            span.activated(1);
            compare(testPageExporter.spanKey, "occupied");
            span.activated(2);
            compare(testPageExporter.spanKey, "result");
            span.activated(3);
            compare(testPageExporter.spanKey, "time");
            testPageExporter.spanKey = "result";
            page.openExport();
            compare(page.detailsOpen, false);
            page.openExport();
            tryCompare(child(page, "editExportSpan"), "currentValue", "result");
            testPageExporter.spanKey = "entire";
            while (testSession.canUndo) testSession.undo();
        }
        function test_clear_media_cache_is_not_part_of_contextual_inspector() {
            const page = make();
            page.toggleDetails();
            verify(!findChild(child(page, "editInspector"), "editClearMediaCache"));
        }
        function test_inspector_omits_monitoring_admission_form() {
            const page = make({ width: 1280, height: 800 });
            page.toggleDetails();
            verify(!findChild(page, "editMonitorConfirm"));
            verify(!findChild(page, "editMonitorOverlapConfirmation"));
            page.toggleDetails();
        }
        function test_source_tabs_scroll_without_truncating_labels() {
            const page = make();
            page.toggleMedia();
            const browser = child(page, "editSourceBrowser");
            const scroll = child(page, "editSourceTabs");
            const project = child(page, "editMediaTab");
            const recordings = child(page, "editHistoryTab");
            recordings.text = "Aufnahmen mit langem Namen";
            tryVerify(() => scroll.contentWidth > scroll.width);
            browser.focusTab(2);
            tryVerify(() => recordings.x >= scroll.contentX
                && recordings.x + Math.min(recordings.width, scroll.width) <= scroll.contentX + scroll.width + 1);
            compare(recordings.contentItem.truncated, false);
            verify(recordings.visualFocus);
            browser.focusTab(0);
            tryCompare(scroll, "contentX", 0);
            verify(project.visualFocus);
            compare(project.contentItem.truncated, false);
        }
        function test_transport_and_undo_actions() {
            while (testSession.canUndo)
                testSession.undo();
            testSession.appendAsset(testSession.media[0].id);
            const page = make({ session: testSession, timeline: testTimeline, player: testPlayer });
            verify(testSession.durationMs > 0);
            compare(testPlayer.clipOpen, false);
            compare(child(page, "editTransportPlay").enabled, true);
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
