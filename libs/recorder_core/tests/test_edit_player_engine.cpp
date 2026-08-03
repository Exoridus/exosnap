#include <gtest/gtest.h>

#include "edit_playback_pacing.h"
#include "recorder_core/edit_player_engine.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

using recorder_core::AudioTrackDescription;
using recorder_core::DemuxedThroughUs;
using recorder_core::EditPlayerEngine;
using recorder_core::ShouldAdmitDemuxedPacket;
using recorder_core::ShouldConvertDecodedFrame;
using recorder_core::ShouldDemuxMorePackets;

// ---- A real container to ask about its audio tracks -----------------------
//
// A genuine Matroska file the engine opens through its ordinary path. The
// video track carries a valid H.264 parameter set and packets of nothing --
// Open() opens the decoders, it does not decode, and this FFmpeg build ships
// no video encoder that could produce a real bitstream (LGPL, hardware-only
// by ADR 0007). Audio is PCM for the same reason: no encoder needed.

constexpr int kTestWidth = 16;
constexpr int kTestHeight = 16;
constexpr int kTestAudioRate = 48000;
constexpr int kTestVideoFrames = 5;
constexpr int64_t kTestVideoFrameMs = 100;
constexpr int64_t kTestAudioBlockMs = 10;

// AVCDecoderConfigurationRecord for a minimal 16x16 baseline stream: one SPS,
// one PPS, 4-byte NAL length prefixes. Real parameter sets, so opening the
// H.264 decoder against them succeeds.
const uint8_t kTestAvcC[] = {
    0x01, 0x42, 0x00, 0x0A,                         // version, profile, compat, level
    0xFF,                                           // lengthSizeMinusOne = 3
    0xE1, 0x00, 0x07, 0x67, 0x42, 0x00, 0x0A, 0xF8, // 1 SPS
    0x41, 0xA2,                                     //
    0x01, 0x00, 0x04, 0x68, 0xCE, 0x38, 0x80,       // 1 PPS
};

// Silences libav's own logging for the duration of one fixture's life. The
// filler bitstream makes the H.264 decoder complain loudly and correctly; left
// on, those lines are the bulk of this binary's output and would bury a real
// failure among them.
class ScopedQuietAvLog {
  public:
    ScopedQuietAvLog() : previous_(av_log_get_level()) {
        av_log_set_level(AV_LOG_QUIET);
    }
    ~ScopedQuietAvLog() {
        av_log_set_level(previous_);
    }
    ScopedQuietAvLog(const ScopedQuietAvLog&) = delete;
    ScopedQuietAvLog& operator=(const ScopedQuietAvLog&) = delete;

  private:
    int previous_;
};

// One audio track per entry; an EMPTY name writes no container track name at
// all, which is exactly what a recording made before names were muxed looks
// like.
bool WriteTestMkv(const std::filesystem::path& path, const std::vector<std::string>& audio_names) {
    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "matroska", nullptr) < 0 || fmt == nullptr)
        return false;

    AVStream* vst = avformat_new_stream(fmt, nullptr);
    if (vst == nullptr) {
        avformat_free_context(fmt);
        return false;
    }
    vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codecpar->codec_id = AV_CODEC_ID_H264;
    vst->codecpar->width = kTestWidth;
    vst->codecpar->height = kTestHeight;
    vst->codecpar->extradata = static_cast<uint8_t*>(av_mallocz(sizeof(kTestAvcC) + AV_INPUT_BUFFER_PADDING_SIZE));
    if (vst->codecpar->extradata == nullptr) {
        avformat_free_context(fmt);
        return false;
    }
    std::memcpy(vst->codecpar->extradata, kTestAvcC, sizeof(kTestAvcC));
    vst->codecpar->extradata_size = static_cast<int>(sizeof(kTestAvcC));
    vst->time_base = AVRational{1, 1000};

    for (const std::string& name : audio_names) {
        AVStream* ast = avformat_new_stream(fmt, nullptr);
        if (ast == nullptr) {
            avformat_free_context(fmt);
            return false;
        }
        ast->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        ast->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
        ast->codecpar->format = AV_SAMPLE_FMT_S16;
        ast->codecpar->sample_rate = kTestAudioRate;
        ast->codecpar->bits_per_coded_sample = 16;
        av_channel_layout_default(&ast->codecpar->ch_layout, 2);
        ast->time_base = AVRational{1, 1000};
        if (!name.empty())
            av_dict_set(&ast->metadata, "title", name.c_str(), 0);
    }

    if (avio_open(&fmt->pb, path.string().c_str(), AVIO_FLAG_WRITE) < 0) {
        avformat_free_context(fmt);
        return false;
    }

    bool ok = avformat_write_header(fmt, nullptr) >= 0;
    AVPacket* pkt = av_packet_alloc();
    ok = ok && pkt != nullptr;

    // AVCC-shaped: a 4-byte big-endian length followed by that many bytes, the
    // layout the container declares. The payload itself is filler.
    constexpr int kNalBytes = 32;
    const int video_bytes = 4 + kNalBytes;
    const int audio_bytes = static_cast<int>(kTestAudioRate * kTestAudioBlockMs / 1000) * 2 * 2;
    const int blocks_per_frame = static_cast<int>(kTestVideoFrameMs / kTestAudioBlockMs);

    for (int i = 0; i < kTestVideoFrames && ok; ++i) {
        const int64_t frame_ms = i * kTestVideoFrameMs;
        ok = av_new_packet(pkt, video_bytes) >= 0;
        if (!ok)
            break;
        pkt->data[0] = 0;
        pkt->data[1] = 0;
        pkt->data[2] = 0;
        pkt->data[3] = static_cast<uint8_t>(kNalBytes);
        std::memset(pkt->data + 4, 0x65, static_cast<size_t>(kNalBytes));
        pkt->stream_index = 0;
        pkt->pts = pkt->dts = frame_ms;
        pkt->duration = kTestVideoFrameMs;
        pkt->flags |= AV_PKT_FLAG_KEY;
        ok = av_interleaved_write_frame(fmt, pkt) >= 0;
        av_packet_unref(pkt);

        for (size_t track = 0; track < audio_names.size() && ok; ++track) {
            for (int block = 0; block < blocks_per_frame && ok; ++block) {
                ok = av_new_packet(pkt, audio_bytes) >= 0;
                if (!ok)
                    break;
                std::memset(pkt->data, 0, static_cast<size_t>(audio_bytes));
                pkt->stream_index = static_cast<int>(track) + 1;
                pkt->pts = pkt->dts = frame_ms + block * kTestAudioBlockMs;
                pkt->duration = kTestAudioBlockMs;
                pkt->flags |= AV_PKT_FLAG_KEY;
                ok = av_interleaved_write_frame(fmt, pkt) >= 0;
                av_packet_unref(pkt);
            }
        }
    }

    av_packet_free(&pkt);
    if (ok)
        ok = av_write_trailer(fmt) >= 0;
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    return ok;
}

// Names the file and removes it again, however the test ends.
class ScopedTestMkv {
  public:
    explicit ScopedTestMkv(const char* stem) : path_(std::filesystem::temp_directory_path() / stem) {
    }
    ~ScopedTestMkv() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    ScopedTestMkv(const ScopedTestMkv&) = delete;
    ScopedTestMkv& operator=(const ScopedTestMkv&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    ScopedQuietAvLog quiet_;
    std::filesystem::path path_;
};

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
        0, [](recorder_core::RawDecodedVideoFrame) {}, [](recorder_core::DecodedAudioBlock) {},
        [] { return int64_t{-1}; });
    EXPECT_FALSE(engine.PlaybackDeliversAudio());
    engine.StopPlaybackDecode();
}

// ---- AudioTracks: what the file carries ----------------------------------

TEST(EditPlayerEngine, ClosedEngineReportsNoAudioTracks) {
    EditPlayerEngine engine;
    EXPECT_TRUE(engine.AudioTracks().empty());
}

TEST(EditPlayerEngine, AudioTracksListsEveryTrackWithItsContainerName) {
    // The case this whole change exists for: a recording that kept system and
    // microphone sound apart carries TWO tracks, and both of them are the
    // file's -- picking one would drop half the recording on the floor.
    ScopedTestMkv file("edit_player_engine_two_named_tracks.mkv");
    ASSERT_TRUE(WriteTestMkv(file.path(), {"System", "Microphone"}));

    EditPlayerEngine engine;
    std::string err;
    ASSERT_TRUE(engine.Open(file.path(), err)) << err;

    const std::vector<AudioTrackDescription> tracks = engine.AudioTracks();
    ASSERT_EQ(tracks.size(), 2u);
    EXPECT_EQ(tracks[0].name, "System");
    EXPECT_EQ(tracks[1].name, "Microphone");
    EXPECT_NE(tracks[0].stream_index, tracks[1].stream_index);
    EXPECT_GE(tracks[0].stream_index, 0);
    EXPECT_GE(tracks[1].stream_index, 0);
    EXPECT_TRUE(engine.HasAudioStream());
}

TEST(EditPlayerEngine, AudioTracksLeaveUnnamedTracksEmptyRatherThanGuessing) {
    // Recordings written before track names were muxed carry none. The name
    // stays empty: the positional mapping is our own muxing convention, not
    // something the container promises, and a track can merge sources -- so
    // deriving "System" from "it is the first one" would be an invention.
    ScopedTestMkv file("edit_player_engine_two_unnamed_tracks.mkv");
    ASSERT_TRUE(WriteTestMkv(file.path(), {"", ""}));

    EditPlayerEngine engine;
    std::string err;
    ASSERT_TRUE(engine.Open(file.path(), err)) << err;

    const std::vector<AudioTrackDescription> tracks = engine.AudioTracks();
    ASSERT_EQ(tracks.size(), 2u);
    EXPECT_TRUE(tracks[0].name.empty());
    EXPECT_TRUE(tracks[1].name.empty());
}

TEST(EditPlayerEngine, AudioTracksMixesNamedAndUnnamedTracksWithoutFillingGaps) {
    ScopedTestMkv file("edit_player_engine_partly_named_tracks.mkv");
    ASSERT_TRUE(WriteTestMkv(file.path(), {"System", ""}));

    EditPlayerEngine engine;
    std::string err;
    ASSERT_TRUE(engine.Open(file.path(), err)) << err;

    const std::vector<AudioTrackDescription> tracks = engine.AudioTracks();
    ASSERT_EQ(tracks.size(), 2u);
    EXPECT_EQ(tracks[0].name, "System");
    EXPECT_TRUE(tracks[1].name.empty());
}

TEST(EditPlayerEngine, AFileWithoutAudioReportsNoTracks) {
    ScopedTestMkv file("edit_player_engine_no_audio.mkv");
    ASSERT_TRUE(WriteTestMkv(file.path(), {}));

    EditPlayerEngine engine;
    std::string err;
    ASSERT_TRUE(engine.Open(file.path(), err)) << err;

    EXPECT_TRUE(engine.AudioTracks().empty());
    EXPECT_FALSE(engine.HasAudioStream());
    EXPECT_TRUE(engine.HasVideoStream());
}

TEST(EditPlayerEngine, ClosingForgetsThePreviousFilesTracks) {
    ScopedTestMkv file("edit_player_engine_tracks_reopen.mkv");
    ASSERT_TRUE(WriteTestMkv(file.path(), {"System", "Microphone"}));

    EditPlayerEngine engine;
    std::string err;
    ASSERT_TRUE(engine.Open(file.path(), err)) << err;
    ASSERT_EQ(engine.AudioTracks().size(), 2u);

    engine.Close();
    EXPECT_TRUE(engine.AudioTracks().empty());

    // ...and a second Open must not append to the first one's list.
    ScopedTestMkv single("edit_player_engine_tracks_reopen_single.mkv");
    ASSERT_TRUE(WriteTestMkv(single.path(), {"System"}));
    ASSERT_TRUE(engine.Open(single.path(), err)) << err;
    EXPECT_EQ(engine.AudioTracks().size(), 1u);
}

TEST(EditPlayerEngine, DecodeFrameAtWithoutOpenReturnsNullopt) {
    EditPlayerEngine engine;
    EXPECT_FALSE(engine.DecodeFrameAt(0).has_value());
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
            0, [](recorder_core::RawDecodedVideoFrame) {}, [](recorder_core::DecodedAudioBlock) {}, {});
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

// ---- Raw-frame decode path: DecodeFrameAtRaw / StartPlaybackDecode -----
//
// Everything above opens a synthetic MKV whose video track carries no real
// compressed bitstream (this FFmpeg build ships no video encoder -- ADR
// 0007), so it exercises Open()/track-discovery only. Actually decoding a
// frame needs a REAL bitstream, which these tests get from checked-in-locally
// (gitignored, .workspace/ is scratch -- see .gitignore) fixture clips.
// Skipped gracefully (GTEST_SKIP) when a fixture is not present on the host,
// same convention as test_analyze_encode_perf.cpp's Python-interpreter check.

#ifndef EXOSNAP_SOURCE_DIR
#define EXOSNAP_SOURCE_DIR "."
#endif

std::filesystem::path TestFixturePath(const char* filename) {
    return std::filesystem::path(EXOSNAP_SOURCE_DIR) / ".workspace" / "test-fixtures" / filename;
}

// Sums the Y plane's first row. Not a meaningful image checksum by itself --
// it exists only to prove the bytes behind y_plane are still the SAME real
// decoded pixel data across two points in time (a freed/reused buffer would
// almost certainly read differently, and a zeroed one would read as 0).
uint64_t ChecksumFirstRow(const recorder_core::RawDecodedVideoFrame& frame) {
    uint64_t sum = 0;
    for (uint32_t x = 0; x < frame.width; ++x)
        sum += frame.y_plane[x];
    return sum;
}

TEST(EditPlayerEngine, DecodeFrameAtRawWithoutOpenReturnsNullopt) {
    EditPlayerEngine engine;
    EXPECT_FALSE(engine.DecodeFrameAtRaw(0).has_value());
}

TEST(EditPlayerEngine, StartStopPlaybackDecodeWithoutOpenIsSafeNoOp) {
    EditPlayerEngine engine;
    std::atomic<int> video_calls{0};
    engine.StartPlaybackDecode(
        0, [&](recorder_core::RawDecodedVideoFrame) { ++video_calls; }, [](recorder_core::DecodedAudioBlock) {},
        [] { return int64_t{-1}; });
    engine.StopPlaybackDecode();
    EXPECT_EQ(video_calls.load(), 0);
}

// The actual regression this guards: RawDecodedVideoFrame's whole point is
// that backing_frame (a ref-counted AVFrame, not a copy) keeps the decoder's
// buffer alive for as long as ANY copy of the struct survives -- including
// past StopPlaybackDecode() and past the engine itself being destroyed. The
// old BGRA path's shared_ptr<uint8_t[]> gave callers exactly this guarantee;
// this test is the same guarantee for the raw path.
TEST(EditPlayerEngine, DecodeRawDeliversRefcountedFramesThatOutliveEngineTeardown) {
    const std::filesystem::path fixture = TestFixturePath("audiotest_60fps.mkv");
    if (!std::filesystem::exists(fixture))
        GTEST_SKIP() << "fixture not present on this host: " << fixture.string();

    std::mutex mu;
    std::condition_variable cv;
    std::vector<recorder_core::RawDecodedVideoFrame> frames;
    constexpr size_t kWanted = 5;

    {
        EditPlayerEngine engine;
        std::string err;
        ASSERT_TRUE(engine.Open(fixture, err)) << err;
        ASSERT_TRUE(engine.HasVideoStream());

        // No media clock: nothing is presenting these frames, so nothing is
        // discarded before conversion (ShouldConvertDecodedFrame) -- every
        // decoded frame from t=0 is delivered, matching probe_edit_playback's
        // step B throughput-measurement convention.
        engine.StartPlaybackDecode(
            0,
            [&](recorder_core::RawDecodedVideoFrame frame) {
                std::lock_guard<std::mutex> lock(mu);
                if (frames.size() < kWanted) {
                    frames.push_back(std::move(frame));
                    if (frames.size() == kWanted)
                        cv.notify_one();
                }
            },
            [](recorder_core::DecodedAudioBlock) {}, {});

        {
            std::unique_lock<std::mutex> lock(mu);
            ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(30), [&] { return frames.size() >= kWanted; }))
                << "timed out waiting for " << kWanted << " raw video frames from " << fixture.string();
        }
        engine.StopPlaybackDecode();

        ASSERT_EQ(frames.size(), kWanted);
        for (size_t i = 0; i < kWanted; ++i) {
            const recorder_core::RawDecodedVideoFrame& f = frames[i];
            EXPECT_NE(f.backing_frame, nullptr) << "frame " << i;
            ASSERT_NE(f.y_plane, nullptr) << "frame " << i;
            ASSERT_NE(f.u_plane, nullptr) << "frame " << i;
            ASSERT_NE(f.v_plane, nullptr) << "frame " << i;
            EXPECT_GT(f.width, 0u) << "frame " << i;
            EXPECT_GT(f.height, 0u) << "frame " << i;
            EXPECT_EQ(f.format, recorder_core::DecodedPixelFormat::Yuv420P8) << "frame " << i;
        }
        // engine goes out of scope here -- Close()/~EditPlayerEngine() frees
        // the video AVCodecContext and AVFormatContext. `frames` is declared
        // OUTSIDE this block and survives it.
    }

    std::vector<uint64_t> checksums(kWanted);
    for (size_t i = 0; i < kWanted; ++i)
        checksums[i] = ChecksumFirstRow(frames[i]);

    // Re-read every retained frame's planes a second time. If backing_frame
    // did not keep the decoder's buffer alive independent of the (now
    // destroyed) engine, this reads freed memory -- a checksum mismatch (or a
    // crash under ASan) is the failure signal.
    for (size_t i = 0; i < kWanted; ++i)
        EXPECT_EQ(ChecksumFirstRow(frames[i]), checksums[i]) << "frame " << i << " planes changed after teardown";
}

// A natively-HDR10 (PQ) source must be flagged for the caller's GPU converter
// to take the tone-map path instead of the ordinary matrix/range conversion
// (IsPqTonemapSource's contract in edit_player_engine.cpp).
TEST(EditPlayerEngine, DecodeFrameAtRawDetectsNativeHdr10Source) {
    const std::filesystem::path fixture = TestFixturePath("hdr10_test_1080p60.mp4");
    if (!std::filesystem::exists(fixture))
        GTEST_SKIP() << "no natively-HDR10 fixture present on this host: " << fixture.string();

    EditPlayerEngine engine;
    std::string err;
    ASSERT_TRUE(engine.Open(fixture, err)) << err;
    ASSERT_TRUE(engine.HasVideoStream());

    const std::optional<recorder_core::RawDecodedVideoFrame> frame = engine.DecodeFrameAtRaw(0);
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->is_pq_source);
    EXPECT_EQ(frame->format, recorder_core::DecodedPixelFormat::Yuv420P10);
    EXPECT_NE(frame->backing_frame, nullptr);
    ASSERT_NE(frame->y_plane, nullptr);
    EXPECT_GT(frame->width, 0u);
    EXPECT_GT(frame->height, 0u);
}

} // namespace
