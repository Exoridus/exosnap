import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

TestCase {
    id: testCase

    name: "ExoSessionTimeline"
    when: windowShown
    width: 800
    height: 60
    visible: true

    Component {
        id: timelineComponent

        ExoSessionTimeline {
            width: 754
            durationMs: 754000
            marks: [
                { startMs: 151000, durationMs: 2100, tone: "warn", title: "Judder", worstText: "9.6 ms" },
                { startMs: 702000, durationMs: 200, tone: "critical", title: "3 frames dropped", worstText: "n/a" }
            ]
        }
    }

    function marksOf(timeline) {
        return findChild(timeline, "sessionTimelineTrack").children.filter(function (child) {
            return child.objectName === "sessionTimelineMark";
        });
    }

    function test_a_short_mark_is_still_at_least_two_pixels_wide() {
        let timeline = createTemporaryObject(timelineComponent, testCase);
        verify(timeline);
        let marks = marksOf(timeline);
        compare(marks.length, 2);
        verify(marks[1].width >= 2, "a 200 ms mark must not vanish");
    }

    function test_mark_x_matches_its_proportional_position() {
        let timeline = createTemporaryObject(timelineComponent, testCase);
        verify(timeline);
        let track = findChild(timeline, "sessionTimelineTrack");
        let marks = marksOf(timeline);

        let expectedX = track.width * 151000 / 754000;
        fuzzyCompare(marks[0].x, expectedX, 1);
    }

    function test_tick_labels_cover_the_recording_in_three_minute_steps() {
        let timeline = createTemporaryObject(timelineComponent, testCase);
        verify(timeline);
        let ticks = findChild(timeline, "sessionTimelineTicks");
        let labels = ticks.children
            .filter(function (child) { return child.objectName === "sessionTimelineTickLabel"; })
            .map(function (label) { return label.text; });

        compare(labels, ["0:00", "3:00", "6:00", "9:00", "12:34"]);
    }

    function test_clicking_the_track_opens_at_the_clicked_position() {
        let timeline = createTemporaryObject(timelineComponent, testCase);
        verify(timeline);
        let track = findChild(timeline, "sessionTimelineTrack");

        let openedAt = -1;
        timeline.openAtRequested.connect(function (positionMs) { openedAt = positionMs; });
        mouseClick(track, Math.round(track.width / 2), 3);
        tryVerify(function () { return openedAt >= 0; });
        fuzzyCompare(openedAt, timeline.durationMs / 2, 5000);
    }

    function test_clicking_a_mark_opens_at_its_own_start() {
        let timeline = createTemporaryObject(timelineComponent, testCase);
        verify(timeline);
        let marks = marksOf(timeline);

        let openedAt = -1;
        timeline.openAtRequested.connect(function (positionMs) { openedAt = positionMs; });
        mouseClick(marks[0], 1, 6);
        compare(openedAt, 151000);
    }
}
