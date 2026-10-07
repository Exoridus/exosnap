import QtQuick
import QtQuick.Controls
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: tests
    name: "EditTimelinePresentation"
    when: windowShown
    width: 640
    height: 240
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
        verify(scroll.contentWidth > scroll.width);
        scroll.contentX = 900;
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
        compare(playhead.x, strip.positionAt(testSession.positionMs));
        testSession.requestSeek(0);
        verify(!playhead.visible);
    }
    function test_zoom_keeps_visible_playhead_at_same_screen_position() {
        const strip = createTemporaryObject(timelineComponent, tests);
        const scroll = findChild(strip, "editTimelineScroll");
        const playhead = findChild(strip, "editPlayhead");
        scroll.contentX = 900;
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
            if (item.objectName === "editMissingMediaWarning" && item.visible) ++warnings;
            for (const child of item.children) countWarnings(child);
        }
        countWarnings(strip);
        compare(warnings, 1);
    }
}
