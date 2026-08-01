#include <gtest/gtest.h>

#include "edit_playback_pacing.h"
#include "recorder_core/edit_player_engine.h"

#include <atomic>
#include <filesystem>
#include <fstream>

namespace {

using recorder_core::DemuxedThroughUs;
using recorder_core::EditPlayerEngine;
using recorder_core::ShouldAdmitDemuxedPacket;
using recorder_core::ShouldConvertDecodedFrame;
using recorder_core::ShouldDemuxMorePackets;

TEST(EditPlayerEngine, OpenNonexistentFileFails) {
    EditPlayerEngine engine;
    std::string err;
    EXPECT_FALSE(engine.Open(std::filesystem::path("Z:/does/not/exist.mkv"), err));
    EXPECT_FALSE(err.empty());
}

TEST(EditPlayerEngine, OpenGarbageFileFails) {
    // A file that exists but is not a container libavformat can probe.
    const auto path = std::filesystem::temp_directory_path() / "edit_player_engine_garbage_test.bin";
    {
        std::ofstream f(path, std::ios::binary);
        f << "not a media file, just some bytes 1234567890";
    }
    EditPlayerEngine engine;
    std::string err;
    EXPECT_FALSE(engine.Open(path, err));
    EXPECT_FALSE(err.empty());
    std::filesystem::remove(path);
}

TEST(EditPlayerEngine, ClosedEngineReportsNoStreams) {
    EditPlayerEngine engine;
    EXPECT_FALSE(engine.HasVideoStream());
    EXPECT_FALSE(engine.HasAudioStream());
}

TEST(EditPlayerEngine, ClosedEngineReportsNoFrameSize) {
    EditPlayerEngine engine;
    EXPECT_EQ(engine.VideoWidth(), 0);
    EXPECT_EQ(engine.VideoHeight(), 0);
}

// The caller paces video off the audio clock whenever it believes audio is
// playing. Building the playback resampler can fail after the file has opened
// with a perfectly good audio stream, and then no audio is delivered at all --
// so "this file has an audio stream" and "this playback run produces audio"
// are different questions, and the caller has to be able to ask the second one.
TEST(EditPlayerEngine, PlaybackDeliversNoAudioBeforeAnyPlaybackStarts) {
    EditPlayerEngine engine;
    EXPECT_FALSE(engine.PlaybackDeliversAudio());
}

TEST(EditPlayerEngine, PlaybackDeliversNoAudioAfterStartingWithoutAnOpenFile) {
    EditPlayerEngine engine;
    engine.StartPlaybackDecode(
        0, [](recorder_core::DecodedVideoFrame) {}, [](recorder_core::DecodedAudioBlock) {},
        [] { return int64_t{-1}; });
    EXPECT_FALSE(engine.PlaybackDeliversAudio());
    engine.StopPlaybackDecode();
}

TEST(EditPlayerEngine, DecodeFrameAtWithoutOpenReturnsNullopt) {
    EditPlayerEngine engine;
    EXPECT_FALSE(engine.DecodeFrameAt(0).has_value());
}

TEST(EditPlayerEngine, StartStopPlaybackDecodeWithoutOpenIsSafeNoOp) {
    EditPlayerEngine engine;
    std::atomic<int> video_calls{0};
    engine.StartPlaybackDecode(
        0, [&](recorder_core::DecodedVideoFrame) { ++video_calls; }, [](recorder_core::DecodedAudioBlock) {},
        [] { return int64_t{-1}; });
    engine.StopPlaybackDecode();
    EXPECT_EQ(video_calls.load(), 0);
}

TEST(EditPlayerEngine, StopPlaybackDecodeWithoutStartIsSafeNoOp) {
    EditPlayerEngine engine;
    engine.StopPlaybackDecode(); // must not crash / hang
    SUCCEED();
}

TEST(EditPlayerEngine, RepeatedStartStopPlaybackDecodeWithoutOpenIsSafe) {
    // Start/stop are lifecycle-idempotent while nothing is open: repeated
    // starts, double-stops, and a Close in the middle must all be safe no-ops.
    EditPlayerEngine engine;
    for (int i = 0; i < 3; ++i) {
        engine.StartPlaybackDecode(
            0, [](recorder_core::DecodedVideoFrame) {}, [](recorder_core::DecodedAudioBlock) {}, {});
        engine.StopPlaybackDecode();
        engine.StopPlaybackDecode(); // double-stop must be safe
    }
    engine.Close();
    SUCCEED();
}

// ---- ShouldConvertDecodedFrame (the video thread's pre-conversion gate) ----

TEST(EditPlaybackPacing, FrameStillAheadOfTheClockIsConverted) {
    EXPECT_TRUE(ShouldConvertDecodedFrame(1'000'000, 900'000));
}

TEST(EditPlaybackPacing, FrameExactlyOnTheClockIsConverted) {
    // The frame due right now is the one to show, not one to throw away.
    EXPECT_TRUE(ShouldConvertDecodedFrame(1'000'000, 1'000'000));
}

TEST(EditPlaybackPacing, FrameAlreadyPastIsNotConverted) {
    EXPECT_FALSE(ShouldConvertDecodedFrame(900'000, 1'000'000));
}

TEST(EditPlaybackPacing, NoClockAvailableNeverDiscards) {
    // A negative reading means "nothing is presenting this" -- with no clock,
    // nothing is known to be late, so nothing may be discarded.
    EXPECT_TRUE(ShouldConvertDecodedFrame(0, -1));
    EXPECT_TRUE(ShouldConvertDecodedFrame(900'000, -1));
    EXPECT_TRUE(ShouldConvertDecodedFrame(-5, -1));
}

TEST(EditPlaybackPacing, ClockAtZeroStillDiscardsEarlierFrames) {
    // Boundary: clock 0 is a real clock reading, not the "no clock" sentinel.
    EXPECT_TRUE(ShouldConvertDecodedFrame(0, 0));
    EXPECT_FALSE(ShouldConvertDecodedFrame(-1, 0));
}

// ---- ShouldAdmitDemuxedPacket (the demuxer's soft/hard queue rule) --------

// Soft 1s / hard 8s / 8192 packets / peer low water 250ms -- the shipped
// shape, restated here so a change to the constants shows up as a test edit.
constexpr recorder_core::PacketQueueLimits kLimits{
    /*soft_capacity_us=*/1'000'000,
    /*hard_capacity_us=*/8'000'000,
    /*hard_capacity_packets=*/8192,
    /*peer_low_water_us=*/250'000,
};

// A queue comfortably below the soft limit, with a peer that is well fed.
recorder_core::PacketAdmissionState MakeState(int64_t queued_us, int64_t peer_queued_us) {
    recorder_core::PacketAdmissionState state;
    state.queued_us = queued_us;
    state.queued_packets = 100;
    state.peer_consuming = true;
    state.peer_queued_us = peer_queued_us;
    return state;
}

TEST(EditPlaybackPacing, BelowTheSoftLimitAlwaysAdmits) {
    // Even with the other stream fully buffered, there is nothing to wait for.
    EXPECT_TRUE(ShouldAdmitDemuxedPacket(MakeState(0, 1'000'000), kLimits));
    EXPECT_TRUE(ShouldAdmitDemuxedPacket(MakeState(999'999, 1'000'000), kLimits));
}

TEST(EditPlaybackPacing, SoftLimitReachedWithAFedPeerWaits) {
    // The normal memory bound: this stream has its second of packets and the
    // other stream is not going hungry, so waiting here costs nothing.
    EXPECT_FALSE(ShouldAdmitDemuxedPacket(MakeState(1'000'000, 250'000), kLimits));
    EXPECT_FALSE(ShouldAdmitDemuxedPacket(MakeState(4'000'000, 900'000), kLimits));
}

TEST(EditPlaybackPacing, SoftLimitReachedWithAStarvingPeerKeepsGoing) {
    // The whole point: waiting here would also withhold the packets sitting
    // right behind this one in the container, which is what the other stream
    // is starving for.
    EXPECT_TRUE(ShouldAdmitDemuxedPacket(MakeState(1'000'000, 249'999), kLimits));
    EXPECT_TRUE(ShouldAdmitDemuxedPacket(MakeState(7'999'999, 0), kLimits));
}

TEST(EditPlaybackPacing, TheRuleIsSymmetricBetweenStreams) {
    // Nothing in the rule names video or audio: the same numbers decide the
    // same way whichever stream is the one at its soft limit. Video full /
    // audio starving and audio full / video starving are the identical call.
    const recorder_core::PacketAdmissionState video_full_audio_starving = MakeState(1'500'000, 100'000);
    const recorder_core::PacketAdmissionState audio_full_video_starving = MakeState(1'500'000, 100'000);
    EXPECT_TRUE(ShouldAdmitDemuxedPacket(video_full_audio_starving, kLimits));
    EXPECT_TRUE(ShouldAdmitDemuxedPacket(audio_full_video_starving, kLimits));
    // ...and both wait once the other side is fed again.
    EXPECT_FALSE(ShouldAdmitDemuxedPacket(MakeState(1'500'000, 400'000), kLimits));
}

TEST(EditPlaybackPacing, HardLimitAlwaysWaitsEvenWithAStarvingPeer) {
    EXPECT_FALSE(ShouldAdmitDemuxedPacket(MakeState(8'000'000, 0), kLimits));
    EXPECT_FALSE(ShouldAdmitDemuxedPacket(MakeState(9'000'000, 0), kLimits));
}

TEST(EditPlaybackPacing, HardPacketCountBackstopBindsWhenDurationsAreUnknown) {
    // Containers that declare no packet durations keep queued_us at 0, so the
    // duration bounds can never trip -- the count is the only thing left.
    recorder_core::PacketAdmissionState state = MakeState(0, 0);
    state.queued_packets = 8191;
    EXPECT_TRUE(ShouldAdmitDemuxedPacket(state, kLimits));
    state.queued_packets = 8192;
    EXPECT_FALSE(ShouldAdmitDemuxedPacket(state, kLimits));
}

TEST(EditPlaybackPacing, AbortBeatsEverything) {
    recorder_core::PacketAdmissionState state = MakeState(0, 0);
    state.aborted = true;
    EXPECT_FALSE(ShouldAdmitDemuxedPacket(state, kLimits));
}

TEST(EditPlaybackPacing, NoPeerStreamFallsBackToTheSoftLimit) {
    // A video-only clip has nothing that could starve, so the soft limit is
    // the bound -- the overshoot allowance must not fire on an absent peer.
    recorder_core::PacketAdmissionState state;
    state.queued_us = 1'000'000;
    state.queued_packets = 100;
    state.peer_consuming = false;
    state.peer_queued_us = 0;
    EXPECT_FALSE(ShouldAdmitDemuxedPacket(state, kLimits));
}

// ---- ShouldDemuxMorePackets (the demuxer's read-ahead budget) -------------

TEST(EditPlaybackPacing, ReadAheadStopsOneBudgetAheadOfTheClock) {
    EXPECT_TRUE(ShouldDemuxMorePackets(1'500'000, 1'000'000, 1'000'000));
    EXPECT_FALSE(ShouldDemuxMorePackets(2'000'000, 1'000'000, 1'000'000));
    EXPECT_FALSE(ShouldDemuxMorePackets(5'000'000, 1'000'000, 1'000'000));
}

TEST(EditPlaybackPacing, ReadAheadResumesAsTheClockAdvances) {
    // The property the audio continuity rests on: a consumer stuck inside a
    // long callback does not stop the demuxer -- only the clock does, and the
    // clock keeps running.
    constexpr int64_t position = 2'000'000;
    EXPECT_FALSE(ShouldDemuxMorePackets(position, 1'000'000, 1'000'000));
    EXPECT_TRUE(ShouldDemuxMorePackets(position, 1'000'001, 1'000'000));
}

TEST(EditPlaybackPacing, NoClockMeansNoReadAheadPacing) {
    // Throughput probes and video-only sessions pass no clock; they must run
    // at full speed, bounded only by the queue limits.
    EXPECT_TRUE(ShouldDemuxMorePackets(999'000'000, -1, 1'000'000));
}

TEST(EditPlaybackPacing, DemuxedThroughIsTheTrailingStream) {
    // Media is buffered through the point BOTH streams have reached; stopping
    // at the leading one would leave the trailing stream's interleaved packets
    // unread, which is a gap in that stream's delivery.
    EXPECT_EQ(DemuxedThroughUs(2'000'000, 1'800'000), 1'800'000);
    EXPECT_EQ(DemuxedThroughUs(1'800'000, 2'000'000), 1'800'000);
    EXPECT_EQ(DemuxedThroughUs(1'800'000, 1'800'000), 1'800'000);
}

TEST(EditPlaybackPacing, DemuxedThroughIsUnknownUntilBothStreamsHaveATimestamp) {
    EXPECT_EQ(DemuxedThroughUs(recorder_core::kUnknownDemuxPositionUs, 2'000'000),
              recorder_core::kUnknownDemuxPositionUs);
    EXPECT_EQ(DemuxedThroughUs(2'000'000, recorder_core::kUnknownDemuxPositionUs),
              recorder_core::kUnknownDemuxPositionUs);
}

TEST(EditPlaybackPacing, UnknownDemuxPositionNeverPaces) {
    // Before the first packet with a usable timestamp there is nothing to
    // measure the budget against.
    EXPECT_TRUE(ShouldDemuxMorePackets(recorder_core::kUnknownDemuxPositionUs, 0, 1'000'000));
    EXPECT_TRUE(ShouldDemuxMorePackets(recorder_core::kUnknownDemuxPositionUs, 600'000'000, 1'000'000));
}

} // namespace
