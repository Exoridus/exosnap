import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    name: "EditPreviewState"
    when: windowShown

    function test_raw_generation_rejection_keeps_source_and_timeline_times_separate() {
        const probe = testMediaPreview.rawGenerationProbe();
        compare(probe.staleRejected, true);
        compare(probe.sourceTime, 33367);
        compare(probe.timelineTime, 1033367);
    }

    function test_supersede_preserves_picture_and_accepted_presentation_time() {
        const probe = testMediaPreview.supersedeProbe();
        compare(probe.unavailableBeforeRender, true);
        compare(probe.hadFrame, true);
        compare(probe.shownTime, 1033367);
        compare(probe.clearRequested, false);
        compare(probe.hasFrame, true);
        compare(probe.retainedTime, 1033367);
    }

    function test_requested_time_is_precise_and_unmeasured_values_are_unavailable() {
        const probe = testMediaPreview.previewProjectionProbe();
        compare(probe.initiallyEmpty, true);
        compare(probe.requestedTime, 33367);
        compare(probe.hasPresentedTime, false);
        compare(probe.seekPending, true);
        compare(probe.buffering, false);
        compare(probe.hasDropMeasurement, false);
    }

    function test_open_timeline_with_unavailable_media_is_not_ready() {
        const probe = testMediaPreview.degradedOpenProbe();
        compare(probe.staleIgnored, true);
        compare(probe.clipOpen, true);
        compare(probe.failed, true);
        compare(probe.placeholder.length > 0, true);
    }

    function test_seek_into_blank_time_presents_black_and_converges() {
        const probe = testMediaPreview.gapSeekProbe();
        compare(probe.blankLayers, 0);
        verify(probe.blankWidth > 0);
        compare(probe.ready, true);
        compare(probe.seekPending, false);
        compare(probe.presented, 110000000);
        compare(probe.requested, 110000000);
        compare(probe.missingUnavailable, true);
        verify(probe.missingPlaceholder.length > 0);
    }

    function test_seeks_against_a_busy_worker_keep_one_running_and_the_newest_pending() {
        const probe = testMediaPreview.seekCoalescingProbe();
        compare(probe.heldActive, true);
        compare(probe.heldPending, 4000);
        compare(probe.drained, true);
        compare(probe.requested, 4000);
    }
}
