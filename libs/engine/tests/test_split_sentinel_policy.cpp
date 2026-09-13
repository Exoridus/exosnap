// Pure unit tests for ShouldEmitSplitSentinel (split_sentinel_policy.h).
// No GPU/NVENC session, no video thread.

#include "split_sentinel_policy.h"

#include <exosnap/engine/recorder_session.h>

#include "av_epoch_align.h"

#include <string>

#include <gtest/gtest.h>

using namespace exosnap::engine;

namespace {

TEST(ShouldEmitSplitSentinel, NotArmed_NeverEmits) {
    // Even an exact keyframe/PTS match must not emit when no split is armed.
    EXPECT_FALSE(ShouldEmitSplitSentinel(/*armed=*/false, /*forced_pts=*/100, /*keyframe=*/true, /*pkt_pts=*/100));
}

TEST(ShouldEmitSplitSentinel, ArmedButNotKeyframe_DoesNotEmit) {
    EXPECT_FALSE(ShouldEmitSplitSentinel(/*armed=*/true, /*forced_pts=*/100, /*keyframe=*/false, /*pkt_pts=*/100));
}

TEST(ShouldEmitSplitSentinel, ArmedKeyframeAtExactForcedPts_Emits) {
    // Sync-mode-equivalent case: the routed packet IS the just-submitted
    // forced frame.
    EXPECT_TRUE(ShouldEmitSplitSentinel(/*armed=*/true, /*forced_pts=*/100, /*keyframe=*/true, /*pkt_pts=*/100));
}

TEST(ShouldEmitSplitSentinel, ArmedKeyframeBeforeForcedPts_DoesNotEmit) {
    // The async-submit-ahead hazard: an EARLIER, already-in-flight natural
    // GOP keyframe must not absorb the split.
    EXPECT_FALSE(ShouldEmitSplitSentinel(/*armed=*/true, /*forced_pts=*/100, /*keyframe=*/true, /*pkt_pts=*/99));
}

TEST(ShouldEmitSplitSentinel, ArmedKeyframeAfterForcedPts_StillEmits) {
    // Defensive >=: if the exact forced-PTS packet is somehow never observed
    // as a keyframe, the next keyframe after it still carries the sentinel
    // rather than losing it.
    EXPECT_TRUE(ShouldEmitSplitSentinel(/*armed=*/true, /*forced_pts=*/100, /*keyframe=*/true, /*pkt_pts=*/101));
}

// ---------------------------------------------------------------------------
// Binding a split request's sequence to the trigger that caused it
// ---------------------------------------------------------------------------
//
// The two lived in separate atomics read in separate loads, so a request landing
// between the reads made the consumer pair sequence N with the trigger of
// sequence N+1: a manual split logged as an automatic size split. The split
// itself was always correct; the reason recorded for it was not, and the reason
// is why it is recorded.

namespace {

constexpr uint32_t kManual = static_cast<uint32_t>(SplitTriggerSource::ManualButton);
constexpr uint32_t kHotkey = static_cast<uint32_t>(SplitTriggerSource::Hotkey);
constexpr uint32_t kAutoSize = static_cast<uint32_t>(SplitTriggerSource::AutomaticSize);

TEST(SplitRequest, PackAndUnpackRoundTrip) {
    const SplitRequestState state{/*sequence=*/12345, kAutoSize, /*coalesced=*/0b1001};
    const SplitRequestState back = UnpackSplitRequest(PackSplitRequest(state));
    EXPECT_EQ(back.sequence, state.sequence);
    EXPECT_EQ(back.primary_trigger, state.primary_trigger);
    EXPECT_EQ(back.coalesced_triggers, state.coalesced_triggers);
}

TEST(SplitRequest, ARequestAdvancesTheSequenceAndOwnsItsTrigger) {
    const SplitRequestState after = UnpackSplitRequest(SplitRequestWith(0, kManual));
    EXPECT_EQ(after.sequence, 1u);
    EXPECT_EQ(after.primary_trigger, kManual);
}

TEST(SplitRequest, ASequenceAndItsTriggerCannotBeReadApart) {
    // The defect, as the interleaving that produced it. Manual becomes sequence
    // 10; the size monitor immediately makes 11. A consumer that observes
    // sequence 10 must see the manual trigger with it -- which is only possible
    // because one load yields both.
    uint64_t word = PackSplitRequest({/*sequence=*/9, kAutoSize, 0});
    word = SplitRequestWith(word, kManual); // sequence 10, manual
    const SplitRequestState observed = UnpackSplitRequest(word);
    ASSERT_EQ(observed.sequence, 10u);
    ASSERT_EQ(observed.primary_trigger, kManual);

    // The size request lands. The already-observed value is unchanged: the
    // consumer cannot be handed 11's reason for 10.
    const uint64_t later = SplitRequestWith(word, kAutoSize);
    EXPECT_EQ(UnpackSplitRequest(later).sequence, 11u);
    EXPECT_EQ(UnpackSplitRequest(later).primary_trigger, kAutoSize);
    EXPECT_EQ(observed.sequence, 10u) << "an observed request must not change under a later one";
    EXPECT_EQ(observed.primary_trigger, kManual);
}

TEST(SplitRequest, CoalescedRequestsAreAllRecordedNotJustTheLast) {
    // Several requests before the boundary collapse into one split, which is
    // correct. What was lost is that they happened: the log named one arbitrary
    // winner.
    uint64_t word = 0;
    word = SplitRequestWith(word, kManual);
    word = SplitRequestWith(word, kAutoSize);
    const SplitRequestState state = UnpackSplitRequest(word);
    EXPECT_EQ(state.sequence, 2u);
    EXPECT_EQ(state.primary_trigger, kAutoSize) << "the most recent request is the primary one";
    EXPECT_NE(state.coalesced_triggers & (1u << (kManual & 0x7u)), 0u) << "the manual request must still be visible";
    EXPECT_NE(state.coalesced_triggers & (1u << (kAutoSize & 0x7u)), 0u);
    EXPECT_EQ(SplitTriggerMaskText(state.coalesced_triggers), "automatic-size+manual-button");
}

TEST(SplitRequest, ConsumingClearsTheMaskButKeepsTheIdentity) {
    // The next boundary must report its own requests, not inherit this one's.
    uint64_t word = SplitRequestWith(SplitRequestWith(0, kManual), kHotkey);
    const SplitRequestState consumed = UnpackSplitRequest(SplitRequestConsumed(UnpackSplitRequest(word)));
    EXPECT_EQ(consumed.sequence, 2u) << "the sequence identifies what was consumed";
    EXPECT_EQ(consumed.primary_trigger, kHotkey);
    EXPECT_EQ(consumed.coalesced_triggers, 0u);
    EXPECT_EQ(SplitTriggerMaskText(consumed.coalesced_triggers), "none");
}

TEST(SplitRequest, AConsumeDoesNotSwallowARequestThatArrivedSince) {
    // The consumer pins its clear to the word it read. A request that landed in
    // between leaves the word different, the compare-exchange fails, and that
    // request survives for the next boundary.
    const SplitRequestState read = UnpackSplitRequest(SplitRequestWith(0, kManual));
    const uint64_t meanwhile = SplitRequestWith(PackSplitRequest(read), kAutoSize);
    EXPECT_NE(meanwhile, PackSplitRequest(read)) << "the pinned compare-exchange must fail here";
    EXPECT_EQ(UnpackSplitRequest(meanwhile).sequence, 2u) << "the newer request is still pending";
}

TEST(SplitRequest, TheSequenceCannotWrapInARecording) {
    // 48 bits. Stated as a test because the packing is what makes it finite.
    const uint64_t huge = (1ULL << 48) - 2ULL;
    const SplitRequestState state = UnpackSplitRequest(PackSplitRequest({huge, kManual, 0xF}));
    EXPECT_EQ(state.sequence, huge);
    EXPECT_EQ(state.primary_trigger, kManual);
    EXPECT_EQ(state.coalesced_triggers, 0xFu);
}

TEST(SplitRequest, TriggerMaskTextIsStableAndOrdered) {
    // Two log entries have to be comparable, so the order is the enum's, not the
    // order the requests happened to arrive in.
    EXPECT_EQ(SplitTriggerMaskText(0), "none");
    EXPECT_EQ(SplitTriggerMaskText(1u << (kAutoSize & 0x7u)), "automatic-size");
    const uint32_t both = (1u << (kAutoSize & 0x7u)) | (1u << (kManual & 0x7u));
    EXPECT_EQ(SplitTriggerMaskText(both), "automatic-size+manual-button");
}

} // namespace

// ---------------------------------------------------------------------------
// The split boundary as one contract
// ---------------------------------------------------------------------------
//
// The pieces above are each correct on their own; this drives them together in
// the order the engine does, because two separately-green contracts can still
// disagree at the boundary they share. One deterministic pass over a split, no
// encoder and no muxer.

namespace {

// A boundary as the engine builds one: a request is made, the video thread arms
// on a frame, the sentinel routes on that frame, the muxer opens the next
// segment from it, and audio is placed against the new epoch.
struct SplitRun {
    uint64_t request_word = 0;
    uint64_t consumed_sequence = 0;
    SplitTriggerSource armed_trigger = SplitTriggerSource::AutomaticDuration;
    std::string coalesced;
    uint64_t segment_epoch_ns = 0;
    bool epoch_set = false;
};

TEST(SplitBoundaryContract, OneRequestYieldsOneBoundaryWithItsOwnReasonAndTimeline) {
    SplitRun run;

    // A manual request, and a size request that lands before the boundary.
    run.request_word = SplitRequestWith(run.request_word, static_cast<uint32_t>(SplitTriggerSource::ManualButton));
    run.request_word = SplitRequestWith(run.request_word, static_cast<uint32_t>(SplitTriggerSource::AutomaticSize));

    // The video thread arms on the frame at 600 s -- one load, so the reason it
    // records belongs to the sequence it consumes.
    const SplitRequestState observed = UnpackSplitRequest(run.request_word);
    const uint64_t forced_pts = 600'000'000'000ULL;
    run.consumed_sequence = observed.sequence;
    run.armed_trigger = static_cast<SplitTriggerSource>(observed.primary_trigger);
    run.coalesced = SplitTriggerMaskText(observed.coalesced_triggers);
    run.request_word = SplitRequestConsumed(observed);

    EXPECT_EQ(run.consumed_sequence, 2u) << "both requests coalesce into one boundary";
    EXPECT_EQ(run.armed_trigger, SplitTriggerSource::AutomaticSize) << "the reason is the request that caused it";
    EXPECT_EQ(run.coalesced, "automatic-size+manual-button") << "and what coalesced into it is not lost";
    EXPECT_EQ(UnpackSplitRequest(run.request_word).coalesced_triggers, 0u)
        << "the next boundary must not inherit this one's requests";

    // The sentinel routes on the forced frame, not an earlier keyframe.
    EXPECT_FALSE(ShouldEmitSplitSentinel(true, forced_pts, /*keyframe=*/true, forced_pts - 33'000'000ULL));
    EXPECT_TRUE(ShouldEmitSplitSentinel(true, forced_pts, /*keyframe=*/true, forced_pts));

    // The muxer opens the next segment; its epoch is that forced keyframe, and
    // the boundary keyframe belongs to the NEW segment at local 0.
    run.segment_epoch_ns = forced_pts;
    run.epoch_set = true;
    const SegmentLocalPts boundary_frame = PlaceOnSegmentTimeline(forced_pts, run.epoch_set, run.segment_epoch_ns);
    EXPECT_EQ(boundary_frame.placement, SegmentPlacement::Write);
    EXPECT_EQ(boundary_frame.local_pts_ns, 0ULL);

    // Audio from before the split, arriving after the boundary: trimmed, not
    // stacked at local 0 where it would play at the start of the new file.
    const SegmentLocalPts late_audio =
        PlaceOnSegmentTimeline(forced_pts - 20'000'000ULL, run.epoch_set, run.segment_epoch_ns);
    EXPECT_EQ(late_audio.placement, SegmentPlacement::Trim);

    // Audio from after it: on the new timeline, monotonically after the keyframe.
    const SegmentLocalPts next_audio =
        PlaceOnSegmentTimeline(forced_pts + 21'333'000ULL, run.epoch_set, run.segment_epoch_ns);
    ASSERT_EQ(next_audio.placement, SegmentPlacement::Write);
    EXPECT_GT(next_audio.local_pts_ns, boundary_frame.local_pts_ns) << "segment times must stay monotonic";
}

TEST(SplitBoundaryContract, ASecondBoundaryReportsItsOwnReasonNotThePreviousOne) {
    // Two splits in a row. The second is a duration split with no manual request
    // behind it, and must not inherit the first boundary's manual reason.
    uint64_t word = SplitRequestWith(0, static_cast<uint32_t>(SplitTriggerSource::ManualButton));
    const SplitRequestState first = UnpackSplitRequest(word);
    word = SplitRequestConsumed(first);

    const SplitRequestState second = UnpackSplitRequest(word);
    EXPECT_EQ(second.sequence, first.sequence) << "no new request: the sequence has not moved";
    EXPECT_EQ(SplitTriggerMaskText(second.coalesced_triggers), "none")
        << "the second boundary has no requests of its own";
}

TEST(SplitBoundaryContract, AudioBeforeTheFirstSegmentEverGotAnEpochIsHeldNotWritten) {
    // The other end of the same contract: a recording whose audio reaches the
    // muxer before any video. There is no timeline yet, so the packet is held --
    // writing its session PTS into a segment-local timeline is what put
    // ten-minute timestamps into a fresh file.
    const SegmentLocalPts held = PlaceOnSegmentTimeline(21'333'000ULL, /*epoch_set=*/false, 0);
    EXPECT_EQ(held.placement, SegmentPlacement::Defer);
}

} // namespace

} // namespace
