import QtQuick
import QtQuick.Controls
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: tests
    name: "EditTimelinePresentation"
    when: windowShown
    width: 640
    height: 520
    visible: true

    Component {
        id: timelineComponent
        EditTimeline { session: testSession; player: testPlayer; width: 600; height: 200 }
    }
    Component {
        id: rulerComponent
        EditTimelineRuler { pixelsPerMs: 0.04; viewportX: 0; viewportWidth: 600 }
    }
    function init() {
        testSession.cancelClipDrag();
        testSession.snapping = true;
        testSession.ripple = false;
        testSession.autoCrossfade = true;
        testSession.linkedSelection = true;
        while (testSession.canUndo) testSession.undo();
        testSession.appendAsset(testSession.media[0].id);
        for (const track of testSession.tracks)
            testSession.trackLayout.setTrackHeight(track.id, 64);
        testSession.requestSeek(0);
    }
    function test_timeline_lane_contract_has_compact_ruler_and_responsive_header() {
        const strip = createTemporaryObject(timelineComponent, tests, {width: 1200});
        verify(!!strip);
        compare(strip.view, findChild(strip, "editViewport"));
        compare(strip.interaction, findChild(strip, "editInteraction"));
        compare(strip.rulerHeight, 26);
        compare(strip.rangeBandTop, 18);
        compare(strip.rangeBandHeight, 8);
        compare(strip.labelWidth, 168);
        strip.width = 900;
        compare(strip.labelWidth, 96);
        strip.userHeaderWidth = 80;
        compare(strip.labelWidth, 80);
        strip.userHeaderWidth = 300;
        strip.width = 1200;
        compare(strip.labelWidth, 280);
        compare(strip.laneViewportRect.x, strip.laneStart);
        verify(strip.laneViewportRect.width > 0);
    }
    function test_crossfade_at_cut_menu_keeps_complete_candidate_pending_until_enter_or_escape() {
        const strip = createTemporaryObject(timelineComponent, tests);
        testSession.requestSeekUs(50000000);
        testSession.splitSelected();
        const outgoing = testSession.visibleClipsUs(0, 100000000).filter(row => row.video && row.startUs === 0)[0].id;
        testSession.selectClip(outgoing);
        strip.view.setVisibleInterval(48000000, 52000000);
        waitForRendering(strip);
        const clip = findChild(strip, "editClip" + outgoing);
        verify(!!clip);
        mouseClick(clip, 40, 8, Qt.RightButton);
        const action = findChild(strip, "editCrossfadeAtCut");
        verify(!!action);
        verify(action.enabled);
        const duration = testSession.durationUs;
        mouseClick(action, action.width / 2, action.height / 2);
        verify(testSession.dragActive);
        verify(testSession.dragValid);
        compare(testSession.durationUs, duration);
        compare(testSession.transitions.length, 0);
        strip.forceActiveFocus();
        keyClick(Qt.Key_Escape);
        verify(!testSession.dragActive);
        compare(testSession.transitions.length, 0);
        verify(testSession.previewCrossfadeAtCut(outgoing));
        strip.forceActiveFocus();
        keyClick(Qt.Key_Return);
        verify(!testSession.dragActive);
        compare(testSession.transitions.length, 2);
        compare(testSession.durationUs, duration);
    }
    function test_escape_before_threshold_prevents_later_mouse_motion_from_starting_an_edit() {
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.view.setVisibleInterval(0, 10000000);
        waitForRendering(strip);
        const clip = findChild(strip, "editClip" + testSession.selectedClip);
        verify(!!clip);
        mousePress(clip, 60, 8);
        keyClick(Qt.Key_Escape);
        verify(!testSession.dragActive);
        try {
            mouseMove(clip, 90, 8, 20);
            verify(!testSession.dragActive);
            verify(!strip.pointerScrolling);
        } finally {
            mouseRelease(clip, 90, 8);
        }
    }
    function test_header_click_preserves_playhead_and_content_click_seeks() {
        const strip = createTemporaryObject(timelineComponent, tests);
        verify(!!strip);
        strip.view.setVisibleInterval(0, 10000000);
        waitForRendering(strip);
        const clip = findChild(strip, "editClip" + testSession.selectedClip);
        verify(!!clip);
        testSession.requestSeekUs(0);
        mouseClick(clip, 60, 8, Qt.LeftButton);
        compare(testSession.positionUs, 0);
        mouseClick(clip, 120, 40, Qt.LeftButton);
        verify(testSession.positionUs > 0);
        compare(testSession.selectionMode, EditSessionAdapter.ClipSelection);
    }
    function test_range_band_moves_then_clears_without_seeking() {
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.view.setVisibleInterval(0, 10000000);
        waitForRendering(strip);
        const band = findChild(strip, "editRangeBand");
        verify(!!band);
        const tracks = testSession.tracks.map(row => row.id);
        verify(testSession.setEditRangeUs(1000000, 3000000, tracks, 1000000));
        const position = testSession.positionUs;
        const at = strip.view.mapping.xAtTime(2000000);
        const to = strip.view.mapping.xAtTime(3000000);
        mousePress(band, at, 4);
        mouseMove(band, to, 4, 20);
        verify(testSession.rangeGestureActive);
        compare(testSession.editRange.startUs, 1000000);
        mouseRelease(band, to, 4);
        compare(testSession.editRange.startUs, 2000000);
        compare(testSession.editRange.endUs, 4000000);
        compare(testSession.editRange.anchorUs, 2000000);
        compare(testSession.positionUs, position);
        mouseClick(band, strip.view.mapping.xAtTime(7000000), 4);
        compare(testSession.selectionMode, EditSessionAdapter.NoSelection);
        compare(testSession.positionUs, position);
    }
    function test_header_edge_drag_autoscroll_preserves_captured_clip() {
        const strip = createTemporaryObject(timelineComponent, tests, {session: testClipboardSession});
        strip.view.setVisibleInterval(0, 5000000);
        waitForRendering(strip);
        const row = strip.session.visibleClipsUs(0, 10000000).find(clip => clip.video);
        const clip = findChild(strip, "editClip" + row.id);
        verify(!!clip);
        const press = clip.mapToItem(strip, 40, 8);
        const start = strip.view.startUs;
        mousePress(clip, 40, 8);
        try {
            mouseMove(strip, strip.width - 4, press.y, 20);
            verify(strip.session.dragActive);
            tryVerify(() => strip.view.startUs > start, 1000);
            compare(findChild(strip, "editClip" + row.id), clip);
            keyClick(Qt.Key_Escape);
            compare(strip.session.dragActive, false);
        } finally {
            mouseRelease(strip, strip.width - 4, press.y);
            strip.session.cancelClipDrag();
        }
    }
    function test_standalone_fade_envelope_is_drawn_inside_the_content() {
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.view.setVisibleInterval(0, 10000000);
        const row = testSession.visibleClipsUs(0, 10000000).find(clip => clip.video);
        verify(testSession.applyTimeValue(row.id, "fadeIn", 1000000));
        waitForRendering(strip);
        const envelope = findChild(strip, "editFadeEnvelope" + row.id + "_0");
        verify(!!envelope);
        verify(envelope.visible);
        verify(envelope.width > 0);
        compare(envelope.y, strip.trackGeometry(row.trackId, false).headerHeight);
        verify(envelope.samples.length > 2);
        compare(envelope.samples[0], 0);
        compare(envelope.samples[envelope.samples.length - 1], 1);
    }
    function test_range_band_creates_all_tracks_and_escape_restores_it() {
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.view.setVisibleInterval(0, 10000000);
        waitForRendering(strip);
        const band = findChild(strip, "editRangeBand");
        verify(!!band);
        testSession.clearSelection();
        const at = strip.view.mapping.xAtTime(1000000);
        const to = strip.view.mapping.xAtTime(3000000);
        mousePress(band, at, 4);
        mouseMove(band, to, 4, 20);
        mouseRelease(band, to, 4);
        compare(testSession.editRange.trackIds.length, testSession.tracks.length);
        compare(testSession.editRange.startUs, 1000000);
        compare(testSession.editRange.endUs, 3000000);
        mousePress(band, to, 4);
        mouseMove(band, strip.view.mapping.xAtTime(4000000), 4, 20);
        keyClick(Qt.Key_Escape);
        mouseRelease(band, strip.view.mapping.xAtTime(4000000), 4);
        compare(testSession.editRange.endUs, 3000000);
    }
    function test_tiny_clip_tick_selects_actual_clip_and_locked_drag_refuses() {
        const target = testSession.visibleClipsUs(0, 100000000).find(row => row.video).id;
        const track = testSession.visibleClipsUs(0, 100000000).find(row => row.id === target).trackId;
        testSession.appendAsset(testSession.media[0].id);
        verify(testSession.applyTimeValue(target, "duration", 16666));
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.fitTimeline();
        waitForRendering(strip);
        const tick = findChild(strip, "editClip" + target);
        verify(!!tick);
        verify(tick.width >= 2);
        compare(testSession.visibleClipsUs(0, 100000000).find(row => row.id === target).durationUs, 16666);
        testSession.clearSelection();
        mouseClick(tick, 1, 10);
        compare(testSession.selectedClip, target);
        const name = findChild(strip, "editNarrowClipName");
        verify(!!name && name.visible && name.text.length > 0);
        verify(testSession.setTrackLocked(track, true));
        try {
            mousePress(tick, 1, 10);
            mouseMove(tick, 40, 10, 20);
            verify(testSession.dragActive);
            verify(!testSession.dragValid);
            mouseRelease(tick, 40, 10);
            verify(!testSession.dragActive);
            compare(testSession.visibleClipsUs(0, 100000000).find(row => row.id === target).startUs, 0);
        } finally {
            testSession.setTrackLocked(track, false);
        }
    }
    function test_header_divider_keyboard_uses_eight_and_thirty_two_pixel_steps() {
        const strip = createTemporaryObject(timelineComponent, tests, {width: 1200});
        const divider = findChild(strip, "editHeaderDivider");
        verify(!!divider);
        divider.forceActiveFocus();
        const position = testSession.positionUs;
        keyClick(Qt.Key_Right);
        compare(strip.userHeaderWidth, 176);
        keyClick(Qt.Key_Left, Qt.ShiftModifier);
        compare(strip.userHeaderWidth, 144);
        compare(testSession.positionUs, position);
    }
    function test_header_divider_has_a_dedicated_strip_outside_track_controls() {
        const strip = createTemporaryObject(timelineComponent, tests);
        waitForRendering(strip);
        const divider = findChild(strip, "editHeaderDivider");
        for (const track of testSession.tracks) {
            const menu = findChild(strip, "editTrackMenu" + track.id);
            const resize = findChild(strip, "editTrackResize" + track.id);
            verify(menu.mapToItem(strip, menu.width, 0).x <= divider.x);
            verify(resize.mapToItem(strip, resize.width, 0).x <= divider.x);
        }
    }
    function test_narrow_track_overflow_is_reachable_by_keyboard_without_transport() {
        const strip = createTemporaryObject(timelineComponent, tests);
        waitForRendering(strip);
        const track = testSession.tracks.find(row => !row.video).id;
        const menu = findChild(strip, "editTrackMenu" + track);
        const editor = findChild(strip, "editTrackSettings");
        verify(!!menu && !!editor);
        const position = testSession.positionUs;
        menu.forceActiveFocus();
        keyClick(Qt.Key_Space);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Return);
        tryCompare(editor, "opened", true);
        compare(testSession.positionUs, position);
        editor.close();
    }
    function makeOverlapJunction() {
        testSession.requestSeekUs(50000000);
        testSession.splitSelected();
        const outgoing = testSession.visibleClipsUs(0, 100000000).find(row => row.video && row.startUs === 0).id;
        verify(testSession.applyCrossfade(outgoing, 1000));
        return testSession.transitions.find(row => row.outgoing === outgoing);
    }
    function test_junction_popup_fits_short_window_and_scrolls_to_the_last_control() {
        const window = tests.Window.window;
        const originalHeight = window.height;
        window.height = 480;
        try {
            makeOverlapJunction();
            let junction = testSession.transitions.find(row => !row.video);
            verify(!!junction);
            testSession.selectTransition(junction.outgoing);
            testSession.setJunctionWindow(junction.overlapStartUs / 1000, junction.overlapEndUs / 1000);
            junction = testSession.transitions.find(row => row.id === junction.id);
            compare(junction.kind, "crossfade");
            const strip = createTemporaryObject(timelineComponent, tests);
            strip.openTransitionPopup(junction);
            const popup = findChild(strip, "editCrossfadeDurationPopup");
            tryCompare(popup, "opened", true);
            compare(popup.crossfade, true);
            waitForRendering(popup.contentItem);
            verify(popup.height <= window.height - 16);
            const scroll = findChild(strip, "editJunctionPopupScroll");
            verify(!!scroll);
            const field = findChild(strip, "editCrossfadeDuration");
            verify(scroll.contentHeight > scroll.height, "content=" + scroll.contentHeight + ", viewport=" + scroll.height + ", popup=" + popup.height + ", window=" + window.height + ", field=" + field.visible + ":" + field.height + ":" + field.width + ", kind=" + popup.details.kind);
            scroll.contentItem.contentY = scroll.contentHeight - scroll.height;
            wait(0);
            const remove = findChild(strip, "editRemoveOverlap");
            verify(remove.mapToItem(window.contentItem, 0, remove.height).y <= window.height);
            remove.forceActiveFocus();
            const first = findChild(strip, "editCrossfadeDuration");
            first.forceActiveFocus();
            waitForRendering(popup.contentItem);
            tryVerify(() => first.mapToItem(window.contentItem, 0, 0).y >= popup.y, 1000);
            popup.close();
        } finally {
            window.height = originalHeight;
        }
    }
    function test_hard_cut_popup_names_and_only_exposes_its_cut_position() {
        const junction = makeOverlapJunction();
        verify(testSession.applyTimeValue(junction.id, "junctionCut", 49500000));
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.openTransitionPopup(testSession.transitions.find(row => row.id === junction.id));
        const popup = findChild(strip, "editCrossfadeDurationPopup");
        tryCompare(popup, "opened", true);
        const title = findChild(strip, "editJunctionPopupTitle");
        verify(!!title);
        compare(title.text, "Hard cut");
        verify(!findChild(strip, "editCrossfadeDuration").visible);
        verify(!findChild(strip, "editJunctionStart").visible);
        verify(!findChild(strip, "editJunctionEnd").visible);
        verify(findChild(strip, "editJunctionCut").visible);
        verify(!findChild(strip, "editRemoveCrossfade").visible);
        popup.close();
    }
    function test_clip_context_menu_opens_existing_junction_on_either_side() {
        const junction = makeOverlapJunction();
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.view.setVisibleInterval(48000000, 52000000);
        waitForRendering(strip);
        for (const entry of [{clip: junction.outgoing, action: "editJunctionAfterClip"},
                             {clip: junction.incoming, action: "editJunctionBeforeClip"}]) {
            testSession.selectClip(entry.clip);
            strip.forceActiveFocus();
            keyClick(Qt.Key_Menu);
            const action = findChild(strip, entry.action);
            verify(!!action);
            verify(action.visible && action.enabled);
            mouseClick(action, action.width / 2, action.height / 2);
            const popup = findChild(strip, "editCrossfadeDurationPopup");
            tryCompare(popup, "opened", true);
            compare(testSession.selectedTransition, junction.outgoing);
            popup.close();
        }
    }
    function test_hard_cut_marker_drag_previews_cut_without_moving_clips() {
        const junction = makeOverlapJunction();
        verify(testSession.applyTimeValue(junction.id, "junctionCut", 49500000));
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.view.setVisibleInterval(48000000, 51000000);
        waitForRendering(strip);
        const line = findChild(strip, "editHardCutLine" + junction.id);
        verify(!!line);
        const start = testSession.visibleClipsUs(0, 100000000).find(row => row.id === junction.incoming).startUs;
        mousePress(line, 0.5, 20);
        mouseMove(line, 24, 20, 20);
        verify(testSession.dragActive);
        verify(testSession.dragValid);
        compare(testSession.transitions.find(row => row.id === junction.id).cutUs, 49500000);
        mouseRelease(line, 24, 20);
        verify(!testSession.dragActive);
        verify(testSession.transitions.find(row => row.id === junction.id).cutUs > 49500000);
        compare(testSession.transitions.find(row => row.id === junction.id).kind, "hardCut");
        compare(testSession.visibleClipsUs(0, 100000000).find(row => row.id === junction.incoming).startUs, start);
        testSession.undo();
        compare(testSession.transitions.find(row => row.id === junction.id).cutUs, 49500000);
    }
    function test_remove_overlap_popup_stages_one_complete_structural_candidate() {
        const junction = makeOverlapJunction();
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.view.setVisibleInterval(48000000, 51000000);
        strip.openTransitionPopup(junction);
        const popup = findChild(strip, "editCrossfadeDurationPopup");
        tryCompare(popup, "opened", true);
        const remove = findChild(strip, "editRemoveOverlap");
        verify(!!remove);
        const checkpoint = testEditHarness.timelineCheckpoint();
        const geometry = JSON.stringify(testSession.visibleClipsUs(0, 100000000));
        remove.forceActiveFocus();
        keyClick(Qt.Key_Space);
        verify(testSession.dragActive && testSession.dragValid, testSession.workspaceError + " / " + testSession.dragReason);
        compare(JSON.stringify(testSession.visibleClipsUs(0, 100000000)), geometry);
        compare(testSession.transitions.length, 2);
        compare(testEditHarness.timelineCheckpoint(), checkpoint);
        const overlay = findChild(strip, "editJunctionCurves" + junction.id).parent;
        compare(overlay.opacity, 0.3);
        strip.forceActiveFocus();
        keyClick(Qt.Key_Escape);
        compare(testSession.transitions.length, 2);
        verify(testSession.previewRemoveOverlap(junction.id));
        strip.forceActiveFocus();
        keyClick(Qt.Key_Return);
        compare(testSession.transitions.length, 0);
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(testSession.transitions.length, 2);
    }
    function test_actual_overlap_body_selects_on_press_and_escape_restores_original_range() {
        const junction = makeOverlapJunction();
        verify(testSession.applyTimeValue(junction.id, "junctionStart", 49400000));
        verify(testSession.applyTimeValue(junction.id, "junctionEnd", 49600000));
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.view.setVisibleInterval(48000000, 51000000);
        waitForRendering(strip);
        const overlay = findChild(strip, "editJunctionCurves" + junction.id).parent;
        verify(testSession.setEditRangeUs(1000000, 2000000, testSession.tracks.map(row => row.id)));
        const original = JSON.stringify(testSession.editRange);
        const position = testSession.positionUs;
        const at = strip.positionAtUs(49100000) - overlay.x;
        mousePress(overlay, at, 20);
        compare(testSession.selectedTransition, junction.outgoing);
        mouseMove(overlay, at + 30, 20, 20);
        verify(!testSession.dragActive);
        compare(testSession.positionUs, position);
        strip.forceActiveFocus();
        keyClick(Qt.Key_Escape);
        mouseRelease(overlay, at + 30, 20);
        compare(JSON.stringify(testSession.editRange), original);
        compare(testSession.positionUs, position);
    }
    function test_track_controls_and_header_resize_are_available() {
        const strip = createTemporaryObject(timelineComponent, tests, {width: 1200});
        waitForRendering(strip);
        const audio = testSession.tracks.find(row => !row.video);
        verify(!!findChild(strip, "editTrackMenu" + audio.id));
        const mute = findChild(strip, "editTrackEnabled" + audio.id);
        const solo = findChild(strip, "editTrackSolo" + audio.id);
        const lock = findChild(strip, "editTrackLock" + audio.id);
        verify(!!mute && !!solo && !!lock);
        mouseClick(mute);
        compare(testSession.tracks.find(row => row.id === audio.id).enabled, false);
        mouseClick(solo);
        verify(testSession.trackSolo(audio.id));
        mouseClick(lock);
        verify(testSession.trackLocked(audio.id));
        testSession.setTrackLocked(audio.id, false);
        testSession.setTrackSolo(audio.id, false);
        const divider = findChild(strip, "editHeaderDivider");
        verify(!!divider);
        mousePress(divider, 3, 80);
        mouseMove(divider, 43, 80, 20);
        mouseRelease(divider, 43, 80);
        compare(strip.userHeaderWidth, 208);
        mouseDoubleClickSequence(divider, 3, 80);
        compare(strip.userHeaderWidth, 168);
    }
    function test_track_header_body_double_click_collapses_and_restores_height() {
        const strip = createTemporaryObject(timelineComponent, tests);
        waitForRendering(strip);
        const row = testSession.tracks[0];
        const label = findChild(strip, "editTrackLabel0");
        verify(!!label);
        compare(label.height, 64);
        mouseDoubleClickSequence(label, 50, 8);
        compare(testSession.trackLayout.heightForId(row.id), 22);
        mouseDoubleClickSequence(label, 50, 8);
        compare(testSession.trackLayout.heightForId(row.id), 64);
    }
    function test_track_grip_reorders_with_marker_escape_and_keyboard() {
        testSession.beginClipDrag(testSession.visibleClipsUs(0, 10000000).find(row => row.video).id, -1, true);
        testSession.updateClipDragUs(0, 0, 0);
        verify(testSession.commitClipDrag());
        const strip = createTemporaryObject(timelineComponent, tests, {height: 300});
        waitForRendering(strip);
        const trackId = testSession.tracks[0].id;
        const grip = findChild(strip, "editTrackGrip" + trackId);
        const marker = findChild(strip, "editTrackReorderMarker");
        const next = findChild(strip, "editTrackLabel1");
        const destination = next.mapToItem(strip, 50, next.height / 2 + 5);
        // The name no longer reorders: only the grip does.
        mousePress(findChild(strip, "editTrackLabel0"), 50, 8);
        mouseMove(strip, destination.x, destination.y, 20);
        mouseRelease(strip, destination.x, destination.y);
        compare(testSession.tracks[0].id, trackId);
        // Escape during the drag writes nothing.
        mousePress(grip, 6, 11);
        mouseMove(strip, destination.x, destination.y, 20);
        verify(marker.visible);
        keyClick(Qt.Key_Escape);
        verify(!marker.visible);
        mouseRelease(strip, destination.x, destination.y);
        compare(testSession.tracks[0].id, trackId);
        // Release writes one undoable reorder.
        mousePress(grip, 6, 11);
        mouseMove(strip, destination.x, destination.y, 20);
        mouseRelease(strip, destination.x, destination.y);
        compare(testSession.tracks[1].id, trackId);
        testSession.undo();
        compare(testSession.tracks[0].id, trackId);
        // The focused grip moves its track with Up and Down.
        findChild(strip, "editTrackGrip" + trackId).forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Down);
        compare(testSession.tracks[1].id, trackId);
        tryVerify(() => findChild(strip, "editTrackGrip" + trackId).activeFocus);
        keyClick(Qt.Key_Up);
        compare(testSession.tracks[0].id, trackId);
        testSession.undo();
        testSession.undo();
    }
    function test_content_drag_creates_range_without_authored_move() {
        const strip = createTemporaryObject(timelineComponent, tests, {tool: "range"});
        verify(!!strip);
        strip.view.setVisibleInterval(0, 10000000);
        waitForRendering(strip);
        const clip = findChild(strip, "editClip" + testSession.selectedClip);
        verify(!!clip);
        const starts = testSession.visibleClipsUs(0, 100000000).map(row => row.startUs);
        mousePress(clip, 100, 40, Qt.LeftButton);
        try {
            const pressedPosition = testSession.positionUs;
            mouseMove(clip, 200, 40, 20);
            verify(testSession.rangeGestureActive);
            verify(testSession.rangeGestureValid);
            compare(testSession.positionUs, pressedPosition);
        } finally {
            mouseRelease(clip, 200, 40, Qt.LeftButton);
        }
        compare(testSession.selectionMode, EditSessionAdapter.RangeSelection);
        compare(testSession.visibleClipsUs(0, 100000000).map(row => row.startUs), starts);
    }
    function test_escape_during_header_press_restores_range_without_clearing_it() {
        const strip = createTemporaryObject(timelineComponent, tests);
        verify(!!strip);
        strip.view.setVisibleInterval(0, 10000000);
        waitForRendering(strip);
        const row = testSession.visibleClipsUs(0, 100000000).find(clip => clip.video);
        verify(!!row);
        verify(testSession.setEditRangeUs(1000000, 2000000, [row.trackId], 2000000));
        const clip = findChild(strip, "editClip" + row.id);
        verify(!!clip);
        mousePress(clip, 60, 8, Qt.LeftButton);
        try {
            keyClick(Qt.Key_Escape);
            compare(testSession.selectionMode, EditSessionAdapter.RangeSelection);
            compare(testSession.editRange.startUs, 1000000);
            compare(testSession.editRange.endUs, 2000000);
            compare(testSession.editRange.anchorUs, 2000000);
        } finally {
            mouseRelease(clip, 60, 8, Qt.LeftButton);
        }
    }
    function test_empty_lane_drag_creates_range_and_keeps_initial_seek() {
        verify(testSession.applyTimeValue(0, "timelineEnd", 300000000));
        const strip = createTemporaryObject(timelineComponent, tests, {tool: "range"});
        verify(!!strip);
        strip.view.setVisibleInterval(0, 300000000);
        waitForRendering(strip);
        const scroll = findChild(strip, "editTimelineScroll");
        verify(!!scroll);
        const x = strip.labelWidth + 300;
        const y = strip.rulerHeight + 40;
        mousePress(scroll, x, y, Qt.LeftButton);
        try {
            const position = testSession.positionUs;
            mouseMove(scroll, x + 60, y, 20);
            verify(testSession.rangeGestureActive);
            verify(testSession.rangeGestureValid);
            compare(testSession.positionUs, position);
        } finally {
            mouseRelease(scroll, x + 60, y, Qt.LeftButton);
        }
        compare(testSession.selectionMode, EditSessionAdapter.RangeSelection);
    }
    function test_select_tool_marquee_selects_touched_clips_without_history() {
        verify(testSession.applyTimeValue(0, "timelineEnd", 300000000));
        const strip = createTemporaryObject(timelineComponent, tests);
        verify(!!strip);
        compare(strip.tool, "select");
        strip.view.setVisibleInterval(0, 300000000);
        waitForRendering(strip);
        const scroll = findChild(strip, "editTimelineScroll");
        const marquee = findChild(strip, "editMarquee");
        const x = strip.labelWidth + 300;
        const y = strip.rulerHeight + 40;
        testSession.clearSelection();
        const undoable = testSession.canUndo;
        const position = testSession.positionUs;
        mousePress(scroll, x, y, Qt.LeftButton);
        mouseMove(scroll, strip.labelWidth + 4, y, 20);
        verify(marquee.visible);
        verify(!testSession.rangeGestureActive);
        verify(testSession.selectedClipIds.length > 0);
        compare(testSession.positionUs, position);
        keyClick(Qt.Key_Escape);
        verify(!marquee.visible);
        compare(testSession.selectedClipIds.length, 0);
        mouseRelease(scroll, strip.labelWidth + 4, y, Qt.LeftButton);
        mousePress(scroll, x, y, Qt.LeftButton);
        mouseMove(scroll, strip.labelWidth + 4, y, 20);
        mouseRelease(scroll, strip.labelWidth + 4, y, Qt.LeftButton);
        verify(!marquee.visible);
        verify(testSession.selectedClipIds.length > 0);
        compare(testSession.selectionMode, EditSessionAdapter.ClipSelection);
        compare(testSession.canUndo, undoable);
        testSession.clearSelection();
    }
    function test_clear_cache_refreshes_unchanged_visible_media_data() {
        return [{tag: "video-combined", audio: false, combined: true},
                {tag: "video-thumbnail", audio: false, combined: false},
                {tag: "audio-combined", audio: true, combined: true}];
    }
    function test_clear_cache_refreshes_unchanged_visible_media(data) {
        verify(data.audio ? testPreviewWorkload.prepareAudio() : testPreviewWorkload.prepare(1));
        const strip = createTemporaryObject(timelineComponent, tests, {
            session: testPreviewWorkload.session,
            thumbnails: testPreviewWorkload.timeline
        });
        verify(!!strip);
        waitForRendering(strip);
        const clip = findChild(strip, "editClip" + strip.session.selectedClip);
        verify(!!clip);
        const recipe = clip.mediaRequest;
        const sourceRevision = strip.thumbnails.sourceRevision;
        verify(recipe.length > 0);
        const key = clip.waveformKey;
        const before = testPreviewWorkload.thumbnailQueries();
        if (data.audio) {
            verify(key.length > 0);
            verify(testPreviewWorkload.hasWaveformRequest(key));
        }
        if (data.combined) strip.thumbnails.clearMediaCaches();
        else strip.thumbnails.clearThumbnailCache();
        waitForRendering(strip);
        if (data.audio) {
            compare(clip.mediaRequest, recipe);
            compare(clip.waveformKey, key);
            verify(testPreviewWorkload.hasWaveformRequest(key), "Cleared waveform recipe was registered again");
        } else {
            verify(strip.thumbnails.sourceRevision > sourceRevision);
            compare(clip.mediaRequest.split("|").slice(0, -1).join("|"), recipe.split("|").slice(0, -1).join("|"));
            compare(clip.mediaRequest.split("|").pop(), String(strip.thumbnails.sourceRevision));
            compare(testPreviewWorkload.thumbnailQueries() - before, 1);
            tryVerify(() => {
                const image = findChild(clip, "editClipThumbnail");
                return !!image && image.status === Image.Ready;
            });
        }
    }
    function test_preview_queries_are_bounded_to_visible_tracks() {
        verify(testPreviewWorkload.prepare(20));
        const before = testPreviewWorkload.thumbnailQueries();
        const strip = createTemporaryObject(timelineComponent, tests, {
            session: testPreviewWorkload.session,
            thumbnails: testPreviewWorkload.timeline,
            height: 280
        });
        verify(!!strip);
        waitForRendering(strip);
        const queries = testPreviewWorkload.thumbnailQueries() - before;
        verify(queries > 0 && queries <= 4, "Preview queries: " + queries);
    }
    function test_offscreen_tracks_do_not_materialize_clip_delegates() {
        verify(testPreviewWorkload.prepare(20));
        const strip = createTemporaryObject(timelineComponent, tests, {
            session: testPreviewWorkload.session,
            thumbnails: testPreviewWorkload.timeline
        });
        verify(!!strip);
        waitForRendering(strip);
        function clipCount(item) {
            let count = /^editClip[0-9]+$/.test(item.objectName) ? 1 : 0;
            for (const child of item.children ?? [])
                count += clipCount(child);
            return count;
        }
        verify(clipCount(strip) > 0);
        verify(clipCount(strip) <= 4);
    }
    function test_zoom_batch_publishes_the_latest_view_once_data() {
        return [{tag: "restored-view", steps: 50, previews: 0},
                {tag: "new-view", steps: 51, previews: 1}];
    }
    function test_zoom_batch_publishes_the_latest_view_once(data) {
        verify(testPreviewWorkload.prepare(1));
        const strip = createTemporaryObject(timelineComponent, tests, {
            session: testPreviewWorkload.session,
            thumbnails: testPreviewWorkload.timeline
        });
        strip.zoomTo(100);
        strip.panTimeline(500);
        waitForRendering(strip);
        const model = findChild(strip, "editVisibleClips");
        verify(!!model);
        const rows = testPreviewWorkload.modelQueries(model);
        const previews = testPreviewWorkload.thumbnailQueries();
        const position = testPreviewWorkload.session.positionUs;
        for (let i = 0; i < data.steps; ++i) {
            const scale = i % 2 === 0 ? 200 : 100;
            strip.zoomTo(scale);
            compare(strip.pixelsPerSecond, scale);
        }
        waitForRendering(strip);
        compare(testPreviewWorkload.modelQueries(model) - rows, 1);
        compare(testPreviewWorkload.thumbnailQueries() - previews, data.previews);
        compare(testPreviewWorkload.session.positionUs, position);
    }
    function test_hidden_view_defers_model_and_preview_work() {
        verify(testPreviewWorkload.prepare(1));
        const strip = createTemporaryObject(timelineComponent, tests, {
            session: testPreviewWorkload.session,
            thumbnails: testPreviewWorkload.timeline
        });
        waitForRendering(strip);
        strip.visible = false;
        const model = findChild(strip, "editVisibleClips");
        const rows = testPreviewWorkload.modelQueries(model);
        const previews = testPreviewWorkload.thumbnailQueries();
        for (let i = 0; i < 51; ++i) strip.zoomTo(i % 2 === 0 ? 200 : 100);
        wait(50);
        compare(testPreviewWorkload.modelQueries(model), rows);
        compare(testPreviewWorkload.thumbnailQueries(), previews);
        strip.visible = true;
        waitForRendering(strip);
        compare(testPreviewWorkload.modelQueries(model) - rows, 1);
        compare(testPreviewWorkload.thumbnailQueries() - previews, 1);
    }
    function test_vertical_scroll_loads_only_new_visible_previews() {
        verify(testPreviewWorkload.prepare(20));
        const strip = createTemporaryObject(timelineComponent, tests, {
            session: testPreviewWorkload.session,
            thumbnails: testPreviewWorkload.timeline,
            height: 280
        });
        waitForRendering(strip);
        const before = testPreviewWorkload.thumbnailQueries();
        const scroll = findChild(strip, "editTimelineScroll");
        scroll.contentY = 5 * 64;
        waitForRendering(strip);
        const queries = testPreviewWorkload.thumbnailQueries() - before;
        verify(queries > 0 && queries <= 4, "Scrolled preview queries: " + queries);
    }
    function test_asset_drop_preserves_fractional_milliseconds() {
        while (testSession.canUndo) testSession.undo();
        testSession.setFixedTimelineEnd(true, 1000);
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.zoomTo(strip.maximumZoom);
        const scroll = findChild(strip, "editTimelineScroll");
        const point = scroll.mapFromItem(strip, 120, 80);
        const expected = strip.timeAtUs(point.x);
        verify(expected > 0 && expected % 1000 !== 0);
        verify(testEditHarness.dropAsset(strip, testSession.media[0].id, 120, 80));
        const rows = testSession.visibleClipsUs(0, testSession.durationUs);
        compare(rows.length, 2);
        for (const row of rows) compare(row.startUs, expected);
    }
    function test_library_drag_auto_scrolls_tracks_and_stops_on_leave_and_drop() {
        while (testSession.canUndo) testSession.undo();
        const strip = createTemporaryObject(timelineComponent, tests, {height: 110});
        waitForRendering(strip);
        const scroll = findChild(strip, "editTimelineScroll");
        verify(scroll.contentHeight > scroll.height, "The fixture needs more tracks than fit: " + scroll.contentHeight);
        scroll.contentY = 0;
        const lanes = strip.laneViewportRect;
        const x = lanes.x + lanes.width / 2;
        const edge = lanes.y + lanes.height - 2;
        const asset = testSession.media[0].id;
        testEditHarness.hoverAsset(strip, asset, x, edge);
        tryVerify(() => scroll.contentY > 0, 2000, "A held library drag at the lower edge scrolls the tracks");
        testEditHarness.leaveHeldDrag(strip);
        verify(!strip.pointerScrolling);
        let settled = scroll.contentY;
        wait(100);
        compare(scroll.contentY, settled);
        scroll.contentY = 0;
        testEditHarness.hoverAsset(strip, asset, x, edge);
        tryVerify(() => scroll.contentY > 0);
        const before = testSession.visibleClipsUs(0, testSession.durationUs).length;
        const expected = strip.timeAtUs(scroll.mapFromItem(strip, x, edge).x);
        verify(testEditHarness.dropHeldDrag(strip, x, edge));
        verify(!strip.pointerScrolling);
        settled = scroll.contentY;
        wait(100);
        compare(scroll.contentY, settled);
        const rows = testSession.visibleClipsUs(0, testSession.durationUs);
        verify(rows.length > before);
        verify(rows.some(row => row.startUs === expected), "The drop lands at the pointer time after scrolling");
        testSession.undo();
    }
    function test_mono_and_stereo_use_the_source_channel_layout() {
        const strip = createTemporaryObject(timelineComponent, tests, {session: testChannelSession});
        const rows = testChannelSession.visibleClipsUs(0, 2000000);
        compare(rows.length, 2);
        for (const row of rows) {
            const waveform = findChild(strip, "editClipWaveform" + row.id);
            verify(!!waveform);
            compare(waveform.channelCount, row.containerIndex === 4 ? 1 : 2);
        }
    }
    function test_intervals_data() {
        return [
            {tag: "overview", scale: 0.01, interval: 10000},
            {tag: "normal", scale: 0.04, interval: 5000},
            {tag: "detail", scale: 0.25, interval: 500},
            {tag: "minutes", scale: 0.001, interval: 120000}
        ];
    }
    function test_sample_zoom_bounds_surfaces_and_preserves_exact_playhead() {
        const strip = createTemporaryObject(timelineComponent, tests);
        const scroll = findChild(strip, "editTimelineScroll");
        strip.zoomTo(96000);
        verify(scroll.contentWidth <= scroll.width + 1);
        const rows = testSession.visibleClipsUs(0, 1000);
        verify(rows.length > 0);
        for (const row of rows) {
            const clip = findChild(strip, "editClip" + row.id);
            verify(clip.width <= scroll.width);
        }
        testSession.requestSeekUs(21);
        const playhead = findChild(strip, "editPlayhead");
        fuzzyCompare(playhead.x, strip.positionAt(0.021), 0.01);
    }
    function test_variable_tracks_share_label_and_clip_geometry() {
        const strip = createTemporaryObject(timelineComponent, tests, {height: 300});
        const rows = testSession.visibleClipsUs(0, 1000000);
        const video = rows.find(row => row.video);
        const audio = rows.find(row => !row.video);
        testSession.trackLayout.setTrackHeight(video.trackId, 28);
        testSession.trackLayout.setTrackHeight(audio.trackId, 120);
        const audioLabel = findChild(strip, "editTrackLabel1");
        const videoClip = findChild(strip, "editClip" + video.id);
        const audioClip = findChild(strip, "editClip" + audio.id);
        tryCompare(audioLabel, "y", strip.rulerHeight + 28);
        compare(videoClip.height, 28);
        compare(audioClip.y, audioLabel.y);
        compare(audioClip.height, 120);
        const icon = findChild(videoClip, "editClipMediaIcon");
        const headerHeight = testSession.trackLayout.geometry[String(video.trackId)].headerHeight;
        verify(icon.y >= 0 && icon.y + icon.height <= headerHeight);
        const nameLabel = findChild(videoClip, "editClipName" + video.id);
        verify(!!nameLabel);
        verify(nameLabel.y >= 0 && nameLabel.y + nameLabel.height <= headerHeight);
        const poster = findChild(videoClip, "editClipPoster" + video.id);
        compare(videoClip.trackBounds.contentHeight, 0);
        compare(poster.height, 0);
        verify(!poster.visible);
    }
    function test_variable_height_drop_uses_base_boundary_through_preview() {
        const strip = createTemporaryObject(timelineComponent, tests, {height: 380});
        testSession.snapping = false;
        const video = testSession.visibleClipsUs(0, 1000000).find(row => row.video);
        const audio = testSession.visibleClipsUs(0, 1000000).find(row => !row.video);
        testSession.trackLayout.setTrackHeight(video.trackId, 28);
        testSession.trackLayout.setTrackHeight(audio.trackId, 120);
        const item = findChild(strip, "editClip" + video.id);
        const point = item.mapToItem(strip, 60, 25);
        const boundary = 2 + strip.rulerHeight + 28;
        mousePress(item, 40, 25);
        mouseMove(strip, point.x, boundary, 20);
        tryCompare(testSession, "dragValid", true);
        compare(testSession.dragTracks.length, 4);
        mouseMove(strip, point.x + 1, boundary, 20);
        compare(testSession.dragTracks.length, 4);
        keyClick(Qt.Key_Escape);
        mouseRelease(strip, point.x + 1, boundary);
        verify(!testSession.dragActive);
    }
    function test_intervals(data) {
        const ruler = createTemporaryObject(rulerComponent, tests, {pixelsPerMs: data.scale});
        compare(ruler.majorMs, data.interval);
        verify(ruler.majorMs * ruler.pixelsPerMs >= ruler.labelSpacing);
        compare(ruler.label(500, 500), "00:00.500");
        compare(ruler.label(65000, 5000), "01:05");
        compare(ruler.label(3600000, 60000), "60:00");
    }
    function test_scroll_ruler_seek_and_playhead_share_coordinates() {
        const strip = createTemporaryObject(timelineComponent, tests);
        const scroll = findChild(strip, "editTimelineScroll");
        const ruler = findChild(strip, "editTimelineRuler");
        const playhead = findChild(strip, "editPlayhead");
        strip.zoomTo(40);
        waitForRendering(strip);
        compare(scroll.contentWidth, scroll.width);
        strip.panTimeline(900);
        testSession.requestSeek(25000);
        compare(playhead.x, strip.positionAt(25000));
        compare(playhead.mapToItem(strip, 0, 0).x, 2 + strip.positionAt(25000) - scroll.contentX);
        compare(ruler.pixelsPerMs, strip.pixelsPerMs);
        mouseClick(strip, 202, 16);
        compare(testSession.positionMs, strip.timeAt(scroll.contentX + 200));
        const beforeScroll = scroll.contentX;
        mousePress(strip, 202, 16);
        mouseMove(strip, 302, 16, 50);
        compare(scroll.contentX, beforeScroll);
        verify(testPlayer.scrubbing);
        compare(testSession.positionMs, strip.timeAt(scroll.contentX + 300));
        mouseRelease(strip, 302, 16);
        verify(!testPlayer.scrubbing);
        verify(ruler.tickCount < 100);
        strip.forceActiveFocus();
        keyClick(Qt.Key_Right);
        compare(playhead.x, strip.positionAtUs(testSession.positionUs));
        testSession.requestSeek(0);
        verify(!playhead.visible);
    }
    function test_zoom_keeps_visible_playhead_at_same_screen_position() {
        const strip = createTemporaryObject(timelineComponent, tests);
        const scroll = findChild(strip, "editTimelineScroll");
        const playhead = findChild(strip, "editPlayhead");
        strip.zoomTo(40);
        waitForRendering(strip);
        strip.panTimeline(900);
        testSession.requestSeek(30000);
        const before = playhead.mapToItem(strip, 0, 0).x;
        strip.zoomTo(100);
        fuzzyCompare(playhead.mapToItem(strip, 0, 0).x, before, 0.001);
        compare(testSession.positionMs, 30000);
    }
    function test_vertical_overflow_preserves_track_labels_and_pinned_ruler() {
        const strip = createTemporaryObject(timelineComponent, tests, {height: 90});
        const scroll = findChild(strip, "editTimelineScroll");
        const ruler = findChild(strip, "editTimelineRuler");
        const audioLabel = findChild(strip, "editTrackLabel1");
        const vertical = findChild(strip, "editVerticalScrollBar");
        verify(scroll.contentHeight > scroll.height);
        verify(vertical.size < 1);
        scroll.contentY = scroll.contentHeight - scroll.height;
        compare(ruler.mapToItem(strip, 0, 0).y, 2);
        compare(audioLabel.mapToItem(strip, 0, 0).y, 2 + strip.rulerHeight + testSession.trackLayout.geometry[String(testSession.tracks[1].id)].top - scroll.contentY);
        verify(audioLabel.mapToItem(strip, 0, 0).y < strip.height);
        strip.height = 220;
        tryCompare(vertical, "size", 1);
    }
    function test_ctrl_wheel_and_keyboard_zoom_preserve_position() {
        const strip = createTemporaryObject(timelineComponent, tests);
        const scroll = findChild(strip, "editTimelineScroll");
        const playhead = findChild(strip, "editPlayhead");
        strip.zoomTo(40);
        waitForRendering(strip);
        strip.panTimeline(900);
        testSession.requestSeek(30000);
        const before = strip.timeAt(scroll.contentX + 198);
        mouseWheel(strip, 200, 100, 0, 120, Qt.NoButton, Qt.ControlModifier);
        verify(strip.pixelsPerSecond > 40);
        fuzzyCompare(strip.timeAt(scroll.contentX + 198), before, 1);
        const playheadBeforeKeyboard = playhead.mapToItem(strip, 0, 0).x;
        strip.forceActiveFocus();
        keyClick(Qt.Key_Minus, Qt.ControlModifier);
        compare(strip.pixelsPerSecond, 40);
        fuzzyCompare(playhead.mapToItem(strip, 0, 0).x, playheadBeforeKeyboard, 0.001);
        strip.zoomTo(200000);
        compare(strip.pixelsPerSecond, strip.maximumZoom);
        strip.zoomTo(0);
        compare(strip.pixelsPerSecond, strip.minimumZoom);
    }
    function test_frame_scale_ruler_has_readable_subframe_ticks() {
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.zoomTo(96000);
        const ruler = findChild(strip, "editTimelineRuler");
        compare(ruler.majorMs, 1);
        verify(ruler.tickCount < 100);
    }
    function test_context_ripple_is_keyboard_accessible() {
        const strip = createTemporaryObject(timelineComponent, tests);
        testSession.requestSeek(50000);
        testSession.splitSelected();
        strip.forceActiveFocus();
        keyClick(Qt.Key_F10, Qt.ShiftModifier);
        const menu = findChild(strip, "editClipMenu");
        tryCompare(menu, "opened", true);
        menu.itemAt(0).forceActiveFocus();
        keyClick(Qt.Key_Return);
        tryCompare(menu, "visible", false);
        compare(testSession.durationMs, 50000);
        compare(testSession.visibleClips(0, 100000)[0].startMs, 0);
    }
    function test_drag_threshold_committed_geometry_and_escape() {
        const strip = createTemporaryObject(timelineComponent, tests);
        testSession.snapping = false;
        const clip = testSession.visibleClips(0, 100000)[0];
        const item = findChild(strip, "editClip" + clip.id);
        const originalX = item.x;
        mousePress(item, 100, 8);
        mouseMove(item, 101, 8, 10);
        verify(!testSession.dragActive);
        mouseMove(item, 130, 8, 10);
        verify(testSession.dragActive);
        verify(testSession.dragValid);
        compare(item.x, originalX);
        settleFrame(strip);
        const ghost = findChild(strip, "editDragGhost" + clip.id);
        verify(ghost);
        const scroll = findChild(strip, "editTimelineScroll");
        verify(ghost.x >= scroll.contentX + strip.labelWidth);
        verify(ghost.width <= scroll.width - strip.labelWidth);
        keyClick(Qt.Key_Escape);
        verify(!testSession.dragActive);
        mouseRelease(item, 130, 8);
        compare(testSession.visibleClips(0, 100000)[0].startMs, clip.startMs);
    }
    function test_drag_commit_moves_ghost_candidate_only_on_release() {
        const strip = createTemporaryObject(timelineComponent, tests);
        testSession.snapping = false;
        const clip = testSession.visibleClips(0, 100000)[0];
        const item = findChild(strip, "editClip" + clip.id);
        mousePress(item, 100, 8);
        mouseMove(item, 140, 8, 10);
        verify(testSession.dragValid);
        compare(testSession.visibleClips(0, 100000)[0].startMs, 0);
        const candidate = testSession.dragClips[0].startUs;
        mouseRelease(item, 140, 8);
        compare(testSession.visibleClipsUs(0, 100000000)[0].startUs, candidate);
        testSession.undo();
        compare(testSession.visibleClips(0, 100000)[0].startMs, 0);
    }
    function test_gap_selection_copy_paste_and_escape_preview() {
        const strip = createTemporaryObject(timelineComponent, tests);
        const id = testSession.visibleClips(0, 100000)[0].id;
        testSession.moveClip(id, 10000, false);
        strip.forceActiveFocus();
        mouseClick(strip, strip.positionAt(5000) + 2, 60);
        compare(testSession.selectedGap.startMs, 0);
        compare(testSession.selectedGap.endMs, 10000);
        keyClick(Qt.Key_Delete);
        compare(testSession.visibleClips(0, 200000)[0].startMs, 10000);
        keyClick(Qt.Key_Delete, Qt.ShiftModifier);
        verify(testSession.dragActive && testSession.dragValid);
        compare(testSession.visibleClips(0, 200000)[0].startMs, 10000);
        verify(findChild(strip, "editApplyGapPreview").visible);
        keyClick(Qt.Key_Escape);
        verify(!testSession.dragActive);
        keyClick(Qt.Key_Menu);
        const blankMenu = findChild(strip, "editGapMenu");
        tryCompare(blankMenu, "opened", true);
        blankMenu.itemAt(0).forceActiveFocus();
        keyClick(Qt.Key_Return);
        testSession.requestSeek(0);
        strip.forceActiveFocus();
        keyClick(Qt.Key_Menu);
        tryCompare(blankMenu, "opened", true);
        blankMenu.itemAt(1).forceActiveFocus();
        keyClick(Qt.Key_Return);
        compare(testSession.spacers.length, 1);
        verify(findChild(strip, "editSpacer" + testSession.spacers[0].id));
    }
    function gestureTimeline(scale) {
        const targetScale = scale || 4;
        const strip = createTemporaryObject(timelineComponent, tests, {height: 480, width: 100 + testSession.durationUs * targetScale / 1000000});
        verify(!!strip, "Component exists");
        strip.zoomTo(targetScale);
        waitForRendering(strip);
        return strip;
    }
    // Candidate ghosts are republished on the window's next afterAnimating, not
    // synchronously with the pointer event that changed the drag.
    function settleFrame(strip) {
        tryCompare(strip, "visualWorkPending", false);
    }
    function clipGeometry() {
        return testSession.visibleClips(0, 1000000).map(row => ({
            id: row.id, startMs: row.startMs, durationMs: row.durationMs,
            trackIndex: row.trackIndex, inMs: row.inMs, outMs: row.outMs
        }));
    }
    function projectedClips(strip) {
        const rows = JSON.parse(JSON.stringify(testSession.dragClips));
        verify(rows.length > 0);
        settleFrame(strip);
        const scroll = findChild(strip, "editTimelineScroll");
        verify(!!scroll, "Object exists");
        for (const row of rows) {
            const ghost = findChild(strip, "editDragGhost" + row.id);
            verify(!!ghost, "Object exists");
            compare(ghost.modelData.startMs, row.startMs);
            compare(ghost.modelData.durationMs, row.durationMs);
            compare(ghost.y, strip.rulerHeight + testSession.trackLayout.previewGeometry[String(row.trackId)].top);
            compare(ghost.x, Math.max(scroll.contentX + strip.labelWidth, strip.positionAt(row.startMs)));
            compare(ghost.width, Math.max(0, Math.min(scroll.contentX + scroll.width,
                strip.positionAt(row.startMs + row.durationMs)) - ghost.x));
        }
        return rows;
    }
    function compareCommitted(rows) {
        const committed = clipGeometry();
        for (const row of rows) {
            const actual = committed.find(value => value.id === row.id);
            verify(!!actual);
            fuzzyCompare(actual.startMs, row.startMs, 0.001);
            fuzzyCompare(actual.durationMs, row.durationMs, 0.001);
            compare(actual.trackIndex, row.trackIndex);
        }
    }
    function test_pointer_creates_linked_tracks_atomically_data() {
        return [
            {tag: "front-video", audio: false, zone: 0, targetY: 24},
            {tag: "rear-video", audio: false, zone: 1, targetY: 96},
            {tag: "new-audio", audio: true, zone: 2, targetY: 164}
        ];
    }
    function test_pointer_creates_linked_tracks_atomically(data) {
        testSession.snapping = false;
        const strip = gestureTimeline();
        const before = clipGeometry();
        const tracks = JSON.parse(JSON.stringify(testSession.tracks));
        const lead = testSession.visibleClips(0, 100000).find(row => row.video !== data.audio);
        const item = findChild(strip, "editClip" + lead.id);
        verify(!!item, "Object exists");
        const audioTop = strip.trackLayout.rows.find(row => !row.video).top;
        const boundary = data.zone === 0 ? strip.rulerHeight - 4 : data.zone === 1 ? strip.rulerHeight + audioTop : strip.rulerHeight + strip.trackLayout.baseHeight + 4;
        const destinationY = boundary - item.y;
        const checkpoint = testEditHarness.timelineCheckpoint();
        mousePress(item, 40, 8);
        mouseMove(item, 60, destinationY, 20);
        tryCompare(testSession, "dragActive", true);
        tryCompare(testSession, "dragValid", true);
        compare(clipGeometry(), before);
        compare(testSession.tracks, tracks);
        compare(testEditHarness.timelineCheckpoint(), checkpoint);
        const candidates = projectedClips(strip);
        compare(candidates.length, 2);
        compare(testSession.dragTracks.length, 4);
        const previewTracks = JSON.parse(JSON.stringify(testSession.dragTracks));
        const zone = findChild(strip, "editNewTrackDrop" + data.zone);
        verify(!!zone, "Object exists");
        verify(zone.visible);
        for (const row of candidates) compare(row.startMs, 5000);
        mouseRelease(item, 60, destinationY);
        tryCompare(testSession, "dragActive", false);
        compareCommitted(candidates);
        compare(testSession.tracks.map(row => ({id: row.id, video: row.video})), previewTracks);
        compare(testEditHarness.timelineCheckpoint().revision, checkpoint.revision + 1);
        testSession.undo();
        compare(clipGeometry(), before);
        compare(testSession.tracks, tracks);
        testSession.redo();
        compareCommitted(candidates);
        compare(testSession.tracks.length, 4);
    }
    function test_pointer_new_track_escape_has_no_history() {
        testSession.snapping = false;
        const strip = gestureTimeline();
        const before = clipGeometry();
        const checkpoint = testEditHarness.timelineCheckpoint();
        const item = findChild(strip, "editClip" + before[0].id);
        verify(!!item, "Object exists");
        mousePress(item, 40, 8);
        mouseMove(item, 60, -11, 20);
        tryCompare(testSession, "dragValid", true);
        compare(testSession.dragTracks.length, 4);
        projectedClips(strip);
        keyClick(Qt.Key_Escape);
        tryCompare(testSession, "dragActive", false);
        mouseRelease(item, 60, -11);
        compare(clipGeometry(), before);
        compare(testSession.tracks.length, 2);
        compare(testEditHarness.timelineCheckpoint(), checkpoint);
    }
    function test_pointer_existing_track_move_and_unlinked_audio() {
        testSession.snapping = false;
        const strip = gestureTimeline();
        let rows = testSession.visibleClips(0, 100000);
        const originalTrack = rows[0].trackId;
        let item = findChild(strip, "editClip" + rows[0].id);
        verify(!!item, "Object exists");
        mousePress(item, 40, 8);
        mouseMove(item, 60, -11, 20);
        tryCompare(testSession, "dragValid", true);
        mouseRelease(item, 60, -11);
        tryCompare(testSession, "dragActive", false);
        const before = clipGeometry();
        item = findChild(strip, "editClip" + rows[0].id);
        verify(!!item, "Object exists");
        const destination = strip.trackLayout.rows.find(row => row.id === originalTrack);
        const destinationY = strip.rulerHeight + destination.top + destination.height / 2 + 5 - item.y;
        mousePress(item, 40, 8);
        mouseMove(item, 60, destinationY, 20);
        tryCompare(testSession, "dragValid", true);
        const candidates = projectedClips(strip);
        const lead = candidates.find(row => row.id === rows[0].id);
        compare(lead.trackIndex, 1);
        compare(clipGeometry(), before);
        mouseRelease(item, 60, destinationY);
        tryCompare(testSession, "dragActive", false);
        compareCommitted(candidates);
        testSession.linkedSelection = false;
        rows = testSession.visibleClips(0, 1000000);
        const videoBefore = clipGeometry().find(row => row.id === rows.find(value => value.video).id);
        const audio = rows.find(row => !row.video);
        testSession.clearSelection();
        item = findChild(strip, "editClip" + audio.id);
        verify(!!item, "Object exists");
        mousePress(item, 40, 8);
        mouseMove(item, 60, 8, 20);
        tryCompare(testSession, "dragValid", true);
        const audioCandidate = projectedClips(strip);
        compare(audioCandidate.length, 1);
        compare(audioCandidate[0].id, audio.id);
        mouseRelease(item, 60, 8);
        tryCompare(testSession, "dragActive", false);
        compareCommitted(audioCandidate);
        compare(clipGeometry().find(row => row.id === videoBefore.id), videoBefore);
    }
    function test_pointer_cross_kind_track_rejection_has_no_history() {
        testSession.snapping = false;
        const strip = gestureTimeline();
        const before = clipGeometry();
        const checkpoint = testEditHarness.timelineCheckpoint();
        const item = findChild(strip, "editClip" + before[0].id);
        verify(!!item, "Object exists");
        const destination = strip.trackLayout.rows.find(row => !row.video);
        const destinationY = strip.rulerHeight + destination.top + destination.height / 2 + 5 - item.y;
        mousePress(item, 40, 8);
        mouseMove(item, 60, destinationY, 20);
        tryCompare(testSession, "dragActive", true);
        tryCompare(testSession, "dragValid", false);
        compare(clipGeometry(), before);
        mouseRelease(item, 60, destinationY);
        tryCompare(testSession, "dragActive", false);
        compare(clipGeometry(), before);
        compare(testEditHarness.timelineCheckpoint(), checkpoint);
        verify(testSession.workspaceError.length > 0);
    }
    function test_pointer_overlap_modes_and_invalid_equal_start_data() {
        return [
            {tag: "crossfade", autoCrossfade: true, delta: -40, valid: true},
            {tag: "hard-cut", autoCrossfade: false, delta: -40, valid: true},
            {tag: "equal-start", autoCrossfade: true, delta: -200, valid: false}
        ];
    }
    function test_pointer_overlap_modes_and_invalid_equal_start(data) {
        testSession.snapping = false;
        testSession.autoCrossfade = data.autoCrossfade;
        testSession.appendAsset(testSession.media[0].id);
        const strip = gestureTimeline(2);
        const before = clipGeometry();
        const incoming = testSession.visibleClips(100001, 200000).find(row => row.video);
        const item = findChild(strip, "editClip" + incoming.id);
        verify(!!item, "Object exists");
        const checkpoint = testEditHarness.timelineCheckpoint();
        mousePress(item, 40, 8);
        mouseMove(item, 40 + data.delta, 8, 20);
        tryCompare(testSession, "dragActive", true);
        tryCompare(testSession, "dragValid", data.valid);
        compare(clipGeometry(), before);
        const candidates = data.valid ? projectedClips(strip) : [];
        const junctions = JSON.parse(JSON.stringify(testSession.dragTransitions));
        if (data.valid) {
            compare(junctions.length, 2);
            for (const junction of junctions) {
                compare(junction.startMs, 80000);
                compare(junction.durationMs, 20000);
            }
        }
        mouseRelease(item, 40 + data.delta, 8);
        tryCompare(testSession, "dragActive", false);
        if (!data.valid) {
            compare(clipGeometry(), before);
            compare(testEditHarness.timelineCheckpoint(), checkpoint);
            compare(testSession.transitions.length, 0);
            return;
        }
        compareCommitted(candidates);
        compare(testSession.transitions.length, 2);
        for (const junction of testSession.transitions) {
            compare(junction.kind, data.autoCrossfade ? "crossfade" : "hardCut");
            compare(junction.startMs, 80000);
            compare(junction.endMs, 100000);
        }
        compare(testEditHarness.timelineCheckpoint().revision, checkpoint.revision + 1);
        testSession.undo();
        compare(clipGeometry(), before);
        compare(testSession.transitions.length, 0);
        testSession.redo();
        compareCommitted(candidates);
    }
    function test_pointer_snap_and_ripple_are_independent_data() {
        return [
            {tag: "fixed-unsnapped", snap: false, ripple: false, start: 20000, suffix: 100000},
            {tag: "fixed-snapped", snap: true, ripple: false, start: 20100, suffix: 100000},
            {tag: "ripple-unsnapped", snap: false, ripple: true, start: 20000, suffix: 120000},
            {tag: "ripple-snapped", snap: true, ripple: true, start: 20100, suffix: 120100}
        ];
    }
    function test_pointer_snap_and_ripple_are_independent(data) {
        testSession.appendAsset(testSession.media[0].id);
        testSession.snapping = data.snap;
        testSession.ripple = data.ripple;
        testSession.autoCrossfade = false;
        testSession.requestSeek(20100);
        const strip = gestureTimeline(2);
        const before = clipGeometry();
        const suffixId = testSession.visibleClips(100001, 200000).find(row => row.video).id;
        const item = findChild(strip, "editClip" + before[0].id);
        verify(!!item, "Object exists");
        mousePress(item, 40, 8);
        mouseMove(item, 80, 8, 20);
        tryCompare(testSession, "dragValid", true);
        compare(clipGeometry(), before);
        const candidates = projectedClips(strip);
        compare(candidates.find(row => row.id === before[0].id).startMs, data.start);
        compare(candidates.length, data.ripple ? 4 : 2);
        if (data.ripple)
            compare(candidates.find(row => row.id === suffixId).startMs, data.suffix);
        mouseRelease(item, 80, 8);
        tryCompare(testSession, "dragActive", false);
        compareCommitted(candidates);
        compare(clipGeometry().find(row => row.id === suffixId).startMs, data.suffix);
        compare(testSession.transitions.length, data.ripple ? 0 : 2);
        testSession.undo();
        compare(clipGeometry(), before);
    }
    function test_pointer_linked_trim_previews_ripple_suffix_data() {
        return [
            {tag: "fixed-left", left: true, ripple: false, start: 10000, suffix: 100000},
            {tag: "ripple-left", left: true, ripple: true, start: 0, suffix: 90000},
            {tag: "fixed-right", left: false, ripple: false, start: 0, suffix: 100000},
            {tag: "ripple-right", left: false, ripple: true, start: 0, suffix: 90000}
        ];
    }
    function test_pointer_linked_trim_previews_ripple_suffix(data) {
        testSession.appendAsset(testSession.media[0].id);
        testSession.snapping = false;
        testSession.ripple = data.ripple;
        const strip = gestureTimeline(2);
        const before = clipGeometry();
        const suffixId = testSession.visibleClips(100001, 200000).find(row => row.video).id;
        const item = findChild(strip, "editClip" + before[0].id);
        verify(!!item, "Object exists");
        const pressX = data.left ? 4 : item.width - 4;
        const releaseX = pressX + (data.left ? 20 : -20);
        const checkpoint = testEditHarness.timelineCheckpoint();
        mousePress(item, pressX, 25);
        mouseMove(item, releaseX, 25, 20);
        tryCompare(testSession, "dragValid", true);
        compare(clipGeometry(), before);
        compare(testEditHarness.timelineCheckpoint(), checkpoint);
        const candidates = projectedClips(strip);
        compare(candidates.length, data.ripple ? 4 : 2);
        const lead = candidates.find(row => row.id === before[0].id);
        compare(lead.startMs, data.start);
        compare(lead.durationMs, 90000);
        mouseRelease(item, releaseX, 25);
        tryCompare(testSession, "dragActive", false);
        compareCommitted(candidates);
        const committed = clipGeometry();
        compare(committed.find(row => row.id === suffixId).startMs, data.suffix);
        const trimmed = committed.find(row => row.id === before[0].id);
        compare(trimmed.inMs, data.left ? 10000 : 0);
        compare(trimmed.outMs, data.left ? 100000 : 90000);
        compare(testEditHarness.timelineCheckpoint().revision, checkpoint.revision + 1);
        testSession.undo();
        compare(clipGeometry(), before);
        testSession.redo();
        compareCommitted(candidates);
    }
    function test_pointer_revision_change_cannot_commit_stale_candidate() {
        testSession.snapping = false;
        const strip = gestureTimeline();
        const before = clipGeometry();
        const item = findChild(strip, "editClip" + before[0].id);
        verify(!!item, "Object exists");
        const releasePoint = item.mapToItem(strip, 60, 8);
        mousePress(item, 40, 8);
        mouseMove(item, 60, 8, 20);
        tryCompare(testSession, "dragValid", true);
        projectedClips(strip);
        keyClick(Qt.Key_Period);
        compare(clipGeometry(), before);
        const step = testSession.parseTimeValue("1f", 0).valueUs;
        testSession.nudgeSelectedUs(step);
        const changed = clipGeometry();
        compare(testSession.visibleClipsUs(0, 100000000)[0].startUs, step);
        const checkpoint = testEditHarness.timelineCheckpoint();
        verify(!testSession.dragValid);
        mouseRelease(strip, releasePoint.x, releasePoint.y);
        tryCompare(testSession, "dragActive", false);
        compare(clipGeometry(), changed);
        compare(testEditHarness.timelineCheckpoint(), checkpoint);
        testSession.undo();
        compare(clipGeometry(), before);
    }
    function test_pointer_linked_move_retained_when_resident_view_reopens() {
        testSession.snapping = false;
        const strip = gestureTimeline();
        const before = clipGeometry();
        const item = findChild(strip, "editClip" + before[0].id);
        verify(!!item, "Object exists");
        mousePress(item, 40, 8);
        mouseMove(item, 60, 8, 20);
        tryCompare(testSession, "dragValid", true);
        const candidates = projectedClips(strip);
        mouseRelease(item, 60, 8);
        tryCompare(testSession, "dragActive", false);
        compareCommitted(candidates);
        strip.zoomTo(8);
        const zoom = strip.pixelsPerSecond;
        const checkpoint = testEditHarness.timelineCheckpoint();
        strip.visible = false;
        strip.visible = true;
        strip.forceActiveFocus();
        compare(strip.pixelsPerSecond, zoom);
        compareCommitted(candidates);
        compare(testEditHarness.timelineCheckpoint(), checkpoint);
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(clipGeometry(), before);
        keyClick(Qt.Key_Z, Qt.ControlModifier | Qt.ShiftModifier);
        compareCommitted(candidates);
    }
    function test_gap_pointer_selection_reviews_all_tracks_before_keyboard_apply_data() {
        return [
            {tag: "apply", cancel: false, conflict: false},
            {tag: "escape", cancel: true, conflict: false},
            {tag: "other-track-conflict", cancel: false, conflict: true}
        ];
    }
    function test_gap_pointer_selection_reviews_all_tracks_before_keyboard_apply(data) {
        testSession.snapping = false;
        const initial = testSession.visibleClips(0, 100000);
        testSession.moveClip(initial[0].id, 10000, false);
        if (data.conflict) {
            testSession.linkedSelection = false;
            testSession.clearSelection();
            testSession.selectClip(initial.find(row => !row.video).id);
            testSession.moveClip(initial.find(row => !row.video).id, 0, false);
        }
        const strip = gestureTimeline();
        const before = clipGeometry();
        const checkpoint = testEditHarness.timelineCheckpoint();
        mouseClick(strip, strip.positionAt(5000) + 2, 60);
        tryCompare(testSession, "selectedClip", 0);
        compare(testSession.selectedGap.startMs, 0);
        compare(testSession.selectedGap.endMs, 10000);
        keyClick(Qt.Key_Delete);
        compare(clipGeometry(), before);
        compare(testEditHarness.timelineCheckpoint(), checkpoint);
        keyClick(Qt.Key_Delete, Qt.ShiftModifier);
        tryCompare(testSession, "dragActive", true);
        tryCompare(testSession, "dragValid", !data.conflict);
        const apply = findChild(strip, "editApplyGapPreview");
        verify(!!apply, "Object exists");
        verify(apply.visible);
        compare(apply.enabled, !data.conflict);
        compare(clipGeometry(), before);
        compare(testEditHarness.timelineCheckpoint(), checkpoint);
        const candidates = data.conflict ? [] : projectedClips(strip);
        if (!data.conflict) {
            compare(candidates.length, 2);
            for (const row of candidates) compare(row.startMs, 0);
        }
        keyClick(data.cancel ? Qt.Key_Escape : Qt.Key_Return);
        tryCompare(testSession, "dragActive", false);
        if (data.cancel || data.conflict) {
            compare(clipGeometry(), before);
            compare(testEditHarness.timelineCheckpoint(), checkpoint);
            return;
        }
        compareCommitted(candidates);
        compare(testEditHarness.timelineCheckpoint().revision, checkpoint.revision + 1);
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(clipGeometry(), before);
        keyClick(Qt.Key_Y, Qt.ControlModifier);
        compareCommitted(candidates);
    }
    function test_spacer_pointer_move_trim_cancel_and_delete_keep_media_fixed() {
        testSession.snapping = false;
        const initial = testSession.visibleClips(0, 100000);
        testSession.moveClip(initial[0].id, 20000, false);
        const strip = gestureTimeline();
        const mediaBefore = clipGeometry();
        mouseClick(strip, strip.positionAt(10000) + 2, 60);
        tryCompare(testSession, "selectedClip", 0);
        keyClick(Qt.Key_Menu);
        const blankMenu = findChild(strip, "editGapMenu");
        tryCompare(blankMenu, "opened", true);
        blankMenu.itemAt(0).forceActiveFocus();
        keyClick(Qt.Key_Return);
        keyClick(Qt.Key_Home);
        strip.forceActiveFocus();
        keyClick(Qt.Key_Menu);
        tryCompare(blankMenu, "opened", true);
        blankMenu.itemAt(1).forceActiveFocus();
        keyClick(Qt.Key_Return);
        compare(testSession.spacers.length, 1);
        const spacer = JSON.parse(JSON.stringify(testSession.spacers[0]));
        compare(spacer.startMs, 0);
        compare(spacer.durationMs, 20000);
        let item = findChild(strip, "editSpacer" + spacer.id);
        verify(!!item, "Object exists");
        const initialRight = item.width - 4;
        mousePress(item, initialRight, 25);
        mouseMove(item, initialRight - 40, 25, 20);
        tryCompare(testSession, "dragValid", true);
        compare(testSession.dragSpacers[0].durationMs, 10000);
        mouseRelease(item, initialRight - 40, 25);
        tryCompare(testSession, "dragActive", false);
        compare(testSession.spacers[0].durationMs, 10000);
        const trimmed = JSON.parse(JSON.stringify(testSession.spacers[0]));
        item = findChild(strip, "editSpacer" + spacer.id);
        verify(!!item, "Object exists");
        mousePress(item, 30, 25);
        mouseMove(item, 50, 25, 20);
        tryCompare(testSession, "dragValid", true);
        compare(testSession.spacers[0], trimmed);
        let candidate = JSON.parse(JSON.stringify(testSession.dragSpacers[0]));
        compare(candidate.startMs, 5000);
        settleFrame(strip);
        let ghost = findChild(strip, "editSpacerGhost" + spacer.id);
        verify(!!ghost, "Object exists");
        compare(ghost.x, strip.positionAt(candidate.startMs));
        compare(ghost.width, candidate.durationMs * strip.pixelsPerMs);
        mouseRelease(item, 50, 25);
        tryCompare(testSession, "dragActive", false);
        compare(testSession.spacers[0], candidate);
        compare(clipGeometry(), mediaBefore);
        item = findChild(strip, "editSpacer" + spacer.id);
        verify(!!item, "Object exists");
        const right = item.width - 4;
        mousePress(item, right, 25);
        mouseMove(item, right + 20, 25, 20);
        tryCompare(testSession, "dragValid", true);
        candidate = JSON.parse(JSON.stringify(testSession.dragSpacers[0]));
        compare(candidate.startMs, 5000);
        compare(candidate.durationMs, 15000);
        settleFrame(strip);
        ghost = findChild(strip, "editSpacerGhost" + spacer.id);
        verify(!!ghost, "Object exists");
        compare(ghost.width, candidate.durationMs * strip.pixelsPerMs);
        mouseRelease(item, right + 20, 25);
        tryCompare(testSession, "dragActive", false);
        compare(testSession.spacers[0], candidate);
        const checkpoint = testEditHarness.timelineCheckpoint();
        item = findChild(strip, "editSpacer" + spacer.id);
        verify(!!item, "Object exists");
        mousePress(item, 4, 25);
        mouseMove(item, 24, 25, 20);
        tryCompare(testSession, "dragValid", true);
        compare(testSession.dragSpacers[0].startMs, 10000);
        compare(testSession.dragSpacers[0].durationMs, 10000);
        keyClick(Qt.Key_Escape);
        tryCompare(testSession, "dragActive", false);
        mouseRelease(item, 24, 25);
        compare(testSession.spacers[0], candidate);
        compare(testEditHarness.timelineCheckpoint(), checkpoint);
        mouseClick(item, 30, 25);
        tryCompare(testSession, "selectedSpacer", spacer.id);
        keyClick(Qt.Key_Delete);
        compare(testSession.spacers.length, 0);
        compare(clipGeometry(), mediaBefore);
        keyClick(Qt.Key_Z, Qt.ControlModifier);
        compare(testSession.spacers[0], candidate);
        compare(clipGeometry(), mediaBefore);
    }
    function test_linked_idle_hover_selected() {
        const strip = createTemporaryObject(timelineComponent, tests);
        const clips = testSession.visibleClips(0, 100000);
        testSession.selectClip(0);
        waitForRendering(strip);
        mouseMove(tests, 620, 220);
        const video = findChild(strip, "editClip" + clips[0].id);
        const audio = findChild(strip, "editClip" + clips[1].id);
        verify(video && audio);
        compare(video.border.color, ExoTheme.line);
        compare(audio.border.color, ExoTheme.line);
        const videoBody = String(video.color);
        const audioBody = String(audio.color);
        const videoHeader = findChild(video, "editClipHeader" + clips[0].id);
        const audioHeader = findChild(audio, "editClipHeader" + clips[1].id);
        const idleVideoHeader = String(videoHeader.color);
        const idleAudioHeader = String(audioHeader.color);
        mouseMove(video, 100, 25);
        tryVerify(() => video.linkedHover && audio.linkedHover);
        verify(!Qt.colorEqual(videoHeader.color, idleVideoHeader));
        verify(!Qt.colorEqual(audioHeader.color, idleAudioHeader));
        compare(String(video.color), videoBody);
        compare(String(audio.color), audioBody);
        compare(video.border.color, ExoTheme.line);
        compare(audio.border.color, ExoTheme.line);
        mouseMove(tests, 620, 220);
        tryCompare(videoHeader, "color", idleVideoHeader);
        compare(audioHeader.color, idleAudioHeader);
        testSession.selectClip(clips[1].id);
        for (const clip of [video, audio]) {
            compare(clip.border.color, ExoTheme.line);
            verify(clip.children.some(item => item.visible && item.height === 1 && item.y === 0
                   && item.color !== undefined && Qt.colorEqual(item.color, ExoTheme.accent)));
        }
    }
    function test_linked_selection_and_missing_warning() {
        const strip = createTemporaryObject(timelineComponent, tests);
        const clips = testSession.visibleClips(0, 100000);
        compare(clips.length, 2);
        verify(clips[0].selected && clips[1].selected);
        compare(clips[0].group, clips[1].group);
        testSession.selectClip(clips[1].id);
        verify(testSession.visibleClips(0, 100000)[0].selected);
        verify(testSession.visibleClips(0, 100000)[1].selected);
        let warnings = 0;
        function countWarnings(item) {
            if (item.objectName === "editClipMediaIcon" && item.visible && item.kind === ExoGlyph.Warning) ++warnings;
            for (const child of item.children) countWarnings(child);
        }
        countWarnings(strip);
        compare(warnings, 2);
    }
    function test_clip_delegates_survive_selection_edits_and_scrolling() {
        testSession.appendAsset(testSession.media[0].id);
        const strip = createTemporaryObject(timelineComponent, tests);
        const scroll = findChild(strip, "editTimelineScroll");
        strip.fitTimeline();
        waitForRendering(strip);
        const clips = testSession.visibleClips(0, 1000000);
        compare(clips.length, 4);
        const items = clips.map(clip => findChild(strip, "editClip" + clip.id));
        verify(items.every(item => item !== null));
        const second = clips.findIndex(clip => clip.startMs > 0 && clip.video);
        const unchanged = clips.findIndex(clip => clip.startMs === 0 && clip.video);
        function retained() {
            // Released delegates are deleted later; flush that before comparing identities.
            wait(0);
            clips.forEach((clip, i) => compare(findChild(strip, "editClip" + clip.id), items[i]));
        }
        testSession.selectClip(clips[second].id);
        retained();
        verify(items[second].modelData.selected && !items[unchanged].modelData.selected);
        testSession.nudgeSelected(100, 0);
        retained();
        compare(items[second].modelData.startMs, clips[second].startMs + 100);
        compare(items[second].x, strip.positionAt(clips[second].startMs + 100));
        compare(items[unchanged].modelData.startMs, 0);
        testSession.undo();
        retained();
        compare(items[second].x, strip.positionAt(clips[second].startMs));
        strip.panTimeline(4);
        retained();
    }
    function test_clip_delegates_follow_removal_and_insertion() {
        testSession.appendAsset(testSession.media[0].id);
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.fitTimeline();
        waitForRendering(strip);
        const clips = testSession.visibleClips(0, 1000000);
        const removed = clips.filter(clip => clip.startMs > 0);
        const kept = clips.filter(clip => clip.startMs === 0);
        compare(removed.length, 2);
        const keptItems = kept.map(clip => findChild(strip, "editClip" + clip.id));
        verify(removed.every(clip => findChild(strip, "editClip" + clip.id) !== null));
        testSession.selectClip(removed[0].id);
        testSession.deleteSelected(false);
        wait(0);
        removed.forEach(clip => compare(findChild(strip, "editClip" + clip.id), null));
        kept.forEach((clip, i) => compare(findChild(strip, "editClip" + clip.id), keptItems[i]));
        testSession.undo();
        wait(0);
        removed.forEach(clip => verify(findChild(strip, "editClip" + clip.id) !== null));
        kept.forEach((clip, i) => compare(findChild(strip, "editClip" + clip.id), keptItems[i]));
    }
    function test_zoom_out_instantiates_every_clip_in_the_new_viewport() {
        testSession.appendAsset(testSession.media[0].id);
        const strip = createTemporaryObject(timelineComponent, tests);
        strip.fitTimeline();
        wait(0);
        testSession.visibleClips(0, 1000000).forEach(clip => verify(findChild(strip, "editClip" + clip.id) !== null));
    }
}
