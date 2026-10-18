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
        testSession.requestSeek(0);
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
        strip.panTimeline(900);
        testSession.requestSeek(30000);
        const before = playhead.mapToItem(strip, 0, 0).x;
        strip.zoomTo(100);
        compare(playhead.mapToItem(strip, 0, 0).x, before);
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
        compare(audioLabel.mapToItem(strip, 0, 0).y, 2 + strip.rulerHeight + strip.rowHeight - scroll.contentY);
        verify(audioLabel.mapToItem(strip, 0, 0).y < strip.height);
        strip.height = 220;
        tryCompare(vertical, "size", 1);
    }
    function test_ctrl_wheel_and_keyboard_zoom_preserve_position() {
        const strip = createTemporaryObject(timelineComponent, tests);
        const scroll = findChild(strip, "editTimelineScroll");
        const playhead = findChild(strip, "editPlayhead");
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
        compare(playhead.mapToItem(strip, 0, 0).x, playheadBeforeKeyboard);
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
        mousePress(item, 100, 25);
        mouseMove(item, 101, 25, 10);
        verify(!testSession.dragActive);
        mouseMove(item, 130, 25, 10);
        verify(testSession.dragActive);
        verify(testSession.dragValid);
        compare(item.x, originalX);
        const ghost = findChild(strip, "editDragGhost" + clip.id);
        verify(ghost);
        const scroll = findChild(strip, "editTimelineScroll");
        verify(ghost.x >= scroll.contentX + strip.labelWidth);
        verify(ghost.width <= scroll.width - strip.labelWidth);
        keyClick(Qt.Key_Escape);
        verify(!testSession.dragActive);
        mouseRelease(item, 130, 25);
        compare(testSession.visibleClips(0, 100000)[0].startMs, clip.startMs);
    }
    function test_drag_commit_moves_ghost_candidate_only_on_release() {
        const strip = createTemporaryObject(timelineComponent, tests);
        testSession.snapping = false;
        const clip = testSession.visibleClips(0, 100000)[0];
        const item = findChild(strip, "editClip" + clip.id);
        mousePress(item, 100, 25);
        mouseMove(item, 140, 25, 10);
        verify(testSession.dragValid);
        compare(testSession.visibleClips(0, 100000)[0].startMs, 0);
        const candidate = testSession.dragClips[0].startMs;
        mouseRelease(item, 140, 25);
        compare(testSession.visibleClips(0, 100000)[0].startMs, candidate);
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
        keyClick(Qt.Key_C, Qt.ControlModifier);
        testSession.requestSeek(0);
        keyClick(Qt.Key_V, Qt.ControlModifier);
        compare(testSession.spacers.length, 1);
        verify(findChild(strip, "editSpacer" + testSession.spacers[0].id));
    }
    function gestureTimeline(scale) {
        const targetScale = scale || 4;
        const strip = createTemporaryObject(timelineComponent, tests, {height: 480, width: 54 + testSession.durationUs * targetScale / 1000000});
        verify(!!strip, "Component exists");
        strip.zoomTo(targetScale);
        return strip;
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
        const scroll = findChild(strip, "editTimelineScroll");
        verify(!!scroll, "Object exists");
        for (const row of rows) {
            const ghost = findChild(strip, "editDragGhost" + row.id);
            verify(!!ghost, "Object exists");
            compare(ghost.modelData.startMs, row.startMs);
            compare(ghost.modelData.durationMs, row.durationMs);
            compare(ghost.y, strip.rulerHeight + row.trackIndex * strip.rowHeight + 3);
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
        const destinationY = data.targetY - item.y;
        const checkpoint = testEditHarness.timelineCheckpoint();
        mousePress(item, 40, 25);
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
        mousePress(item, 40, 25);
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
        let item = findChild(strip, "editClip" + rows[0].id);
        verify(!!item, "Object exists");
        mousePress(item, 40, 25);
        mouseMove(item, 60, -11, 20);
        tryCompare(testSession, "dragValid", true);
        mouseRelease(item, 60, -11);
        tryCompare(testSession, "dragActive", false);
        const before = clipGeometry();
        item = findChild(strip, "editClip" + rows[0].id);
        verify(!!item, "Object exists");
        mousePress(item, 40, 25);
        mouseMove(item, 60, 89, 20);
        tryCompare(testSession, "dragValid", true);
        const candidates = projectedClips(strip);
        const lead = candidates.find(row => row.id === rows[0].id);
        compare(lead.trackIndex, 1);
        compare(clipGeometry(), before);
        mouseRelease(item, 60, 89);
        tryCompare(testSession, "dragActive", false);
        compareCommitted(candidates);
        testSession.linkedSelection = false;
        rows = testSession.visibleClips(0, 1000000);
        const videoBefore = rows.find(row => row.video);
        const audio = rows.find(row => !row.video);
        item = findChild(strip, "editClip" + audio.id);
        verify(!!item, "Object exists");
        mousePress(item, 40, 25);
        mouseMove(item, 60, 25, 20);
        tryCompare(testSession, "dragValid", true);
        const audioCandidate = projectedClips(strip);
        compare(audioCandidate.length, 1);
        compare(audioCandidate[0].id, audio.id);
        mouseRelease(item, 60, 25);
        tryCompare(testSession, "dragActive", false);
        compareCommitted(audioCandidate);
        compare(testSession.visibleClips(0, 1000000).find(row => row.id === videoBefore.id), videoBefore);
    }
    function test_pointer_cross_kind_track_rejection_has_no_history() {
        testSession.snapping = false;
        const strip = gestureTimeline();
        const before = clipGeometry();
        const checkpoint = testEditHarness.timelineCheckpoint();
        const item = findChild(strip, "editClip" + before[0].id);
        verify(!!item, "Object exists");
        mousePress(item, 40, 25);
        mouseMove(item, 60, 89, 20);
        tryCompare(testSession, "dragActive", true);
        tryCompare(testSession, "dragValid", false);
        compare(clipGeometry(), before);
        mouseRelease(item, 60, 89);
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
        mousePress(item, 40, 25);
        mouseMove(item, 40 + data.delta, 25, 20);
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
        mouseRelease(item, 40 + data.delta, 25);
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
        mousePress(item, 40, 25);
        mouseMove(item, 80, 25, 20);
        tryCompare(testSession, "dragValid", true);
        compare(clipGeometry(), before);
        const candidates = projectedClips(strip);
        compare(candidates.find(row => row.id === before[0].id).startMs, data.start);
        compare(candidates.length, data.ripple ? 4 : 2);
        if (data.ripple)
            compare(candidates.find(row => row.id === suffixId).startMs, data.suffix);
        mouseRelease(item, 80, 25);
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
        const releasePoint = item.mapToItem(strip, 60, 25);
        mousePress(item, 40, 25);
        mouseMove(item, 60, 25, 20);
        tryCompare(testSession, "dragValid", true);
        projectedClips(strip);
        keyClick(Qt.Key_Right, Qt.AltModifier);
        const changed = clipGeometry();
        compare(changed[0].startMs, 100);
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
        mousePress(item, 40, 25);
        mouseMove(item, 60, 25, 20);
        tryCompare(testSession, "dragValid", true);
        const candidates = projectedClips(strip);
        mouseRelease(item, 60, 25);
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
        keyClick(Qt.Key_C, Qt.ControlModifier);
        keyClick(Qt.Key_Home);
        keyClick(Qt.Key_V, Qt.ControlModifier);
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
        mouseMove(tests, 620, 220);
        const video = findChild(strip, "editClip" + clips[0].id);
        const audio = findChild(strip, "editClip" + clips[1].id);
        verify(video && audio);
        compare(video.border.color, ExoTheme.line);
        compare(audio.border.color, ExoTheme.line);
        compare(video.color, ExoTheme.surfaceRaised);
        compare(audio.color, ExoTheme.surfaceRaised);
        mouseMove(video, 100, 25);
        tryCompare(video, "color", ExoTheme.surfaceHover);
        compare(audio.color, ExoTheme.surfaceHover);
        compare(video.border.color, ExoTheme.line);
        compare(audio.border.color, ExoTheme.line);
        mouseMove(tests, 620, 220);
        tryCompare(video, "color", ExoTheme.surfaceRaised);
        compare(audio.color, ExoTheme.surfaceRaised);
        testSession.selectClip(clips[1].id);
        compare(findChild(strip, "editClip" + clips[0].id).border.color, ExoTheme.accent);
        compare(findChild(strip, "editClip" + clips[1].id).border.color, ExoTheme.accent);
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
