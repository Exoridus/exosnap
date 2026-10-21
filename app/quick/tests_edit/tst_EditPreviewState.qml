import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    name: "EditPreviewState"
    when: windowShown

    function test_transport_intent_data() {
        return [
            {tag: "pending-toggle", action: "toggle", desired: false},
            {tag: "playing-seek", action: "seek", desired: true},
            {tag: "nested-scrub-pending-play", action: "scrub", desired: true},
            {tag: "explicit-pause-during-scrub", action: "pauseDuringScrub", desired: false},
            {tag: "frame-step-pauses", action: "frameStep", desired: false},
            {tag: "authored-edit-preserves-play", action: "authoredEdit", desired: true},
            {tag: "edit-during-scrub-preserves-play", action: "editDuringScrub", desired: true},
            {tag: "stale-position", action: "oldPosition", desired: true},
            {tag: "stale-error", action: "oldError", desired: true},
            {tag: "stale-open-error-same-generation", action: "oldOpenError", desired: true},
            {tag: "stale-state-completion-end", action: "oldAcknowledgement", desired: false},
            {tag: "current-error-clears-intent", action: "currentError", desired: false},
            {tag: "runtime-failure-clears-intent", action: "runtimeFailure", desired: false}
        ];
    }

    function test_transport_intent(data) {
        const probe = testMediaPreview.transportIntentProbe(data.action);
        compare(probe.desiredPlaying, data.desired);
        if (data.action === "authoredEdit" || data.action === "editDuringScrub")
            compare(probe.workspaceCopyNs, 0);
        if (data.action === "seek")
            compare(probe.requested, 20000000);
        if (data.action === "oldPosition")
            compare(probe.playhead, 20000000);
        if (data.action === "oldOpenError")
            compare(probe.placeholderUnchanged, true);
        if (data.action === "oldAcknowledgement")
            compare(probe.acknowledgedPlaying, false);
    }

    function test_blank_timeline_transport_preserves_seek_intent() {
        const probe = testMediaPreview.blankTransportProbe();
        compare(probe.authored, true);
        compare(probe.started, true);
        compare(probe.continued, true);
        compare(probe.paused, true);
    }

    function test_authored_audio_admission_data() {
        return [
            {tag: "raw", composed: false, file: "red.mkv", video: true, enabledAudio: 1, unresolved: false},
            {tag: "composed", composed: true, file: "red.mkv", video: true, enabledAudio: 1, unresolved: false},
            {tag: "raw-unresolved", composed: false, file: "transport-ambiguous.mkv", video: true, enabledAudio: 0, unresolved: true},
            {tag: "composed-unresolved", composed: true, file: "transport-ambiguous.mkv", video: true, enabledAudio: 0, unresolved: true},
            {tag: "audio-only", composed: false, file: "transport-audio-only.mka", video: false, enabledAudio: 1, unresolved: false}
        ];
    }

    function test_authored_audio_admission(data) {
        if (!testMediaPreview.available || !testMediaPreview.fixtureAvailable(data.file))
            skip("Media fixture unavailable: " + data.file);
        const probe = testMediaPreview.authoredAudioAdmissionProbe(data.composed, data.file);
        compare(probe.imported, true, probe.error);
        compare(probe.intentRetained, true);
        compare(probe.advanced, true, probe.error);
        compare(probe.selectionIndependent, true);
        compare(probe.playingSeekContinued, true, probe.error);
        compare(probe.pausedSeekStayedPaused, true, probe.error);
        compare(probe.restartedAtEnd, true, probe.error);
        verify(probe.frames >= 2);
        compare(probe.sawVideo, data.video);
        compare(probe.enabledAudio, data.enabledAudio);
        compare(probe.audioChoiceRequired, data.unresolved);
    }

    function test_obsolete_transport_burst_does_not_decode_every_intermediate_intent() {
        const probe = testMediaPreview.transportBurstProbe();
        compare(probe.desiredPlaying, false);
        compare(probe.completed, 1);
    }

    function test_session_replacement_does_not_reuse_an_unrelated_revision() {
        const probe = testMediaPreview.sessionReplacementProbe();
        compare(probe.duration, 5000000);
        compare(probe.closedAfterDestroy, true);
    }

    function test_pause_does_not_rewind_while_presentation_is_pending() {
        const probe = testMediaPreview.pauseBeforePresentationProbe();
        compare(probe.started, true);
        compare(probe.presentationPending, true);
        verify(probe.workerAt >= probe.pauseAt);
    }

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

    function test_single_video_preview_reads_edited_timeline_audio() {
        if (!testMediaPreview.available)
            skip("Set EXOSNAP_EDIT_RENDER_FIXTURES for real media.");
        const probe = testMediaPreview.timelineAudioEditsProbe();
        compare(probe.readOk, true, probe.error);
        verify(probe.original > 0.001);
        verify(Math.abs(probe.quieter / probe.original - Math.pow(10, -6 / 20)) < 0.01);
        compare(probe.muted, 0);
        compare(probe.deleted, 0);
        compare(probe.beforeMovedAudio, 0);
        verify(Math.abs(probe.moved / probe.original - 1) < 0.01);
        compare(probe.usesVideoComposition, false);
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
    function test_paused_compatible_audio_edit_keeps_the_actual_ready_picture_state() {
        if (!testMediaPreview.available)
            skip("Set EXOSNAP_EDIT_RENDER_FIXTURES for real media.");
        const probe = testMediaPreview.retainedPausedPictureProbe();
        console.log(JSON.stringify(probe));
        compare(probe.initialReady, true, probe.error);
        compare(probe.editApplied, true);
        compare(probe.pausedReadyRetained, true);
        compare(probe.generationRetained, true);
        compare(probe.shownTimeRetained, true);
        compare(probe.seekPending, false);
        compare(probe.paused, true);
        compare(probe.initialDegraded, true);
        compare(probe.secondEditApplied, true);
        compare(probe.degradedStateRetained, true);
        compare(probe.degradedReasonRetained, true);
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
    function test_measurement_reads_are_cached_bounded_and_session_bound() {
        const probe = testMediaPreview.measurementBridgeProbe();
        console.log(JSON.stringify(probe));
        compare(probe.gated, true);
        compare(probe.nonblocking, true);
        compare(probe.pending, true);
        compare(probe.coalesced, true);
        compare(probe.captured, true);
        compare(probe.byCause, true);
        compare(probe.cadence, true);
        compare(probe.stale, true);
        compare(probe.oldSessionRejected, true);
        compare(probe.refreshed, true);
    }

    function test_actual_presented_revision_comes_from_frame_payload_after_render_acceptance() {
        const probe = testMediaPreview.presentationStampProbe();
        console.log(JSON.stringify(probe));
        compare(probe.submittedBeforePaint, true);
        compare(probe.unavailableBeforePaint, true);
        compare(probe.firstPaintStamped, true);
        compare(probe.stalePaintRejected, true);
        compare(probe.payloadNotRequestedRevision, true);
        compare(probe.retainedOldPictureRevision, true);
        compare(probe.clearMakesUnavailable, true);
    }
    function test_structural_preparation_keeps_valid_old_audio_and_mapping_until_adoption() {
        if (!testMediaPreview.available)
            skip("Set EXOSNAP_EDIT_RENDER_FIXTURES for real media.");
        const probe = testMediaPreview.stagedRevisionProbe();
        console.log(JSON.stringify(probe));
        compare(probe.imported, true);
        compare(probe.originalReady, true);
        compare(probe.editApplied, true);
        compare(probe.replacementGated, true);
        compare(probe.oldMappingRetained, true);
        compare(probe.oldAudioContinues, true);
        compare(probe.authoredGainWhilePending, true);
        compare(probe.oldAudioMappingRevisionRetained, true);
        compare(probe.muteWhilePending, true);
        compare(probe.pauseCancelledReplacement, true);
    }
    function test_staged_revision_adopts_current_clock_and_latest_seek_cancels_it() {
        if (!testMediaPreview.available)
            skip("Set EXOSNAP_EDIT_RENDER_FIXTURES for real media.");
        const probe = testMediaPreview.stagedRevisionCommitProbe();
        console.log(JSON.stringify(probe));
        const facts = ["imported", "originalReady", "editsApplied", "firstReplacementGated",
            "supersededReplacementCancelled", "newestRevisionAdopted", "adoptedAtCurrentClock",
            "newestAudioApplied", "unavailableOutputPreserved", "boundedProducers", "oneMappingHandoff", "seekReplacementGated",
            "latestSeekCancelledPreparation", "latestSeekRetainsAuthoredRevision", "latestSeekPosition", "latestSeekPaused"];
        for (const fact of facts)
            compare(probe[fact], true, fact);
    }
    function test_video_open_stall_keeps_late_controls_pcm_and_pause_independent() {
        if (!testMediaPreview.available)
            skip("Set EXOSNAP_EDIT_RENDER_FIXTURES for real media.");
        const probe = testMediaPreview.videoOpenStallProbe();
        console.log(JSON.stringify(probe));
        for (const fact of ["imported", "originalReady", "editApplied", "openGated", "commandWhileOpenStalled",
            "audibleWhileOpenStalled", "mutedWhileOpenStalled", "pauseCancelledOpen"])
            compare(probe[fact], true, fact);
    }
    function test_video_stall_keeps_command_owner_and_source_pcm_controls_live() {
        if (!testMediaPreview.available)
            skip("Set EXOSNAP_EDIT_RENDER_FIXTURES for real media.");
        const probe = testMediaPreview.videoStallProbe();
        console.log(JSON.stringify(probe));
        compare(probe.imported, true);
        compare(probe.gated, true);
        compare(probe.commandWhileVideoStalled, true);
        compare(probe.sourceReady, true);
        compare(probe.audibleBeforeMute, true);
        compare(probe.mutedWhileVideoStalled, true);
        compare(probe.controlApplied, true);
        compare(probe.seekCancelledVideo, true);
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
