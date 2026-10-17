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
    function test_obsolete_timeline_revisions_do_not_reinitialize_playback() {
        const probe = testMediaPreview.timelineBurstProbe();
        compare(probe.revisions, 12);
        compare(probe.updates, 1);
    }
    function test_split_segments_reuse_source_and_remap_decoded_timestamps() {
        if (!testMediaPreview.available)
            skip("Set EXOSNAP_EDIT_RENDER_FIXTURES for real media.");
        const probe = testMediaPreview.splitSourceReuseProbe();
        console.log(JSON.stringify(probe));
        compare(probe.imported, true);
        compare(probe.split, true);
        compare(probe.initial, true);
        compare(probe.beforeMove, true);
        compare(probe.mapped, true);
        verify(Math.abs(probe.mappedSourceUs - 1500000) < 50000);
        compare(probe.back, true);
        verify(Math.abs(probe.backSourceUs - 250000) < 50000);
        compare(probe.monitoringConfirmed, true);
        compare(probe.monitoringPreserved, true);
        compare(probe.monitoringRejections, 0);
        compare(probe.opens, 0);
    }
    function test_timeline_coalescing_preserves_seek_ordering() {
        const probe = testMediaPreview.timelineSeekOrderingProbe();
        compare(probe.atSeekRevision, probe.revisionA);
        compare(probe.finalRevision, probe.revisionB);
        verify(probe.revisionB > probe.revisionA);
    }
    function test_decoder_reuse_preserves_mode_changes_gaps_and_source_changes() {
        if (!testMediaPreview.available)
            skip("Set EXOSNAP_EDIT_RENDER_FIXTURES for real media.");
        const probe = testMediaPreview.playbackModeChangesProbe();
        console.log(JSON.stringify(probe));
        compare(probe.imported, true);
        compare(probe.twoClips, true);
        compare(probe.initial, true);
        compare(probe.composed, true);
        compare(probe.rawRestored, true);
        compare(probe.gap, true);
        compare(probe.afterGap, true);
        compare(probe.otherSource, true);
        compare(probe.otherSourceOpens, 1);
    }
    function test_superseded_open_retains_real_decode_and_playback_readiness_data() {
        return [{tag: "single-clip", composed: false}, {tag: "composed", composed: true}];
    }
    function test_superseded_open_retains_real_decode_and_playback_readiness(data) {
        if (!testMediaPreview.available)
            skip("Set EXOSNAP_EDIT_RENDER_FIXTURES for real media.");
        const probe = testMediaPreview.supersededOpenReadinessProbe(data.composed);
        console.log(JSON.stringify(probe));
        compare(probe.imported, true);
        compare(probe.supersededOpen, true);
        compare(probe.generationSuperseded, true);
        compare(probe.decoded, true);
        compare(probe.clipOpen, true);
        compare(probe.playbackAdvanced, true);
        verify(probe.playbackFrames >= 3);
        compare(probe.volume, 0);
        compare(probe.hasPresentedTime, false);
    }
    function test_imported_thumbnail_source_keys_survive_edits_and_disk_reopen() {
        if (!testMediaPreview.available)
            skip("Set EXOSNAP_EDIT_RENDER_FIXTURES for real media.");
        const probe = testMediaPreview.thumbnailEditReuseProbe();
        console.log(JSON.stringify(probe));
        compare(probe.failure, "");
        compare(probe.imported, true);
        compare(probe.initial, true);
        compare(probe.sources, 2);
        verify(probe.sourceKeys > 0);
        verify(probe.coldDecodes > 0);
        compare(probe.trimmed, true);
        compare(probe.split, true);
        compare(probe.moved, true);
        compare(probe.warmDecodes, probe.coldDecodes);
        compare(probe.diskReused, true);
        compare(probe.reopenDecodes, 0);
        verify(probe.diskHits > 0);
    }
}
