#include <algorithm>
#include <array>
#include <cstdint>
#include <exosnap/engine/color_metadata.h>
#include <exosnap/engine/edit_types.h>
#include <exosnap/engine/performance_measurements.h>
#include <gtest/gtest.h>
#include <ios>
#include <iostream>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>

#include "edit_playback_pacing.h"
#include "exosnap/engine/edit_player_engine.h"
#include "exosnap/engine/edit_player_session.h"
#include "media_timestamp_origin.h"
#include "test_timestamp_media.h"
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/mp4_remuxer.h>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/defs.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

using exosnap::engine::AudioTrackDescription;
using exosnap::engine::DemuxedThroughUs;
using exosnap::engine::DemuxReadAheadWaitUs;
using exosnap::engine::EditPlayerEngine;
using exosnap::engine::ExtrapolatedClockUs;
using exosnap::engine::PresentationHoldUs;
using exosnap::engine::ShouldAdmitDemuxedPacket;
using exosnap::engine::ShouldConvertDecodedFrame;
using exosnap::engine::ShouldDemuxMorePackets;

// ---- A real container to ask about its audio tracks -----------------------
//
// A genuine Matroska file the engine opens through its ordinary path. The
// video track carries a valid H.264 parameter set and packets of nothing --
// Open() opens the decoders, it does not decode, and this FFmpeg build ships
// no video encoder that could produce a real bitstream (LGPL, hardware-only
// by the distribution policy). Audio is PCM for the same reason: no encoder needed.

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
bool WriteTestMkv(const std::filesystem::path& path, const std::vector<std::string>& audio_names,
                  int audio_rate = kTestAudioRate, int64_t impulse_sample = -1, int64_t audio_offset_ms = 0) {
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
        ast->codecpar->sample_rate = audio_rate;
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
    const int audio_bytes = static_cast<int>(audio_rate * kTestAudioBlockMs / 1000) * 2 * 2;
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
                const auto first = (frame_ms + block * kTestAudioBlockMs) * audio_rate / 1000;
                if (impulse_sample >= first && impulse_sample < first + audio_bytes / 4) {
                    const std::array<int16_t, 2> impulse{16384, 16384};
                    std::memcpy(pkt->data + (impulse_sample - first) * 4, impulse.data(), sizeof(impulse));
                }
                pkt->stream_index = static_cast<int>(track) + 1;
                pkt->pts = pkt->dts =
                    frame_ms + block * kTestAudioBlockMs + (track + 1 == audio_names.size() ? audio_offset_ms : 0);
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
        std::filesystem::remove(std::filesystem::path(path_.wstring() + L".exosnap.json"), ec);
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

TEST(EditPlayerSessionAdmission, ExternalVideoClockDoesNotAdmitUnconfirmedSourceAudio) {
    ScopedTestMkv file("edit_player_external_clock_admission.mkv");
    ASSERT_TRUE(WriteTestMkv(file.path(), {""}));
    exosnap::engine::EditPlayerSession session;
    std::string error;
    ASSERT_TRUE(session.Open(file.path(), error)) << error;
    ASSERT_TRUE(session.MonitoringConfirmationRequired());
    session.Play(100'000);
    EXPECT_LT(session.ClockSnapshotUs(), 0);
    int64_t clock = 250'000;
    session.PlayWithExternalClock(clock, 1, [&]() { return clock; });
    EXPECT_EQ(session.CurrentPositionUs(), 250'000);
    clock = 350'000;
    EXPECT_EQ(session.CurrentPositionUs(), 350'000);
    EXPECT_TRUE(session.MonitoringConfirmationRequired());
    session.Pause();
    EXPECT_EQ(session.CurrentPositionUs(), 350'000);
    EXPECT_LT(session.ClockSnapshotUs(), 0);
    session.Play(100'000);
    EXPECT_LT(session.ClockSnapshotUs(), 0);
}

TEST(EditPlayerEngine, DecodedAudioCoverageDistinguishesSilenceFromTailPadding) {
    ScopedTestMkv file("edit_player_audio_coverage.mkv");
    ASSERT_TRUE(WriteTestMkv(file.path(), {""}));
    EditPlayerEngine engine;
    std::string error;
    ASSERT_TRUE(engine.OpenAudioStream(file.path(), 1, error)) << error;
    std::vector<float> samples;
    ASSERT_TRUE(engine.ReadAudioStreamSequential(1, 23800, 600, samples, error)) << error;
    ASSERT_EQ(samples.size(), 1200u);
    EXPECT_TRUE(std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0; }));
    const std::vector<exosnap::engine::AudioSampleSpan> expected{{23800, 24000}};
    EXPECT_EQ(engine.AudioDecodedCoverage(), expected);
    ASSERT_TRUE(engine.ReadAudioStreamSequential(1, 24400, 100, samples, error)) << error;
    EXPECT_TRUE(engine.AudioDecodedCoverage().empty());
    engine.Close();
    EXPECT_TRUE(engine.AudioDecodedCoverage().empty());
}

TEST(EditPlayerEngine, DecodedAudioCoverageExcludesALeadingSourceGap) {
    ScopedTestMkv file("edit_player_audio_coverage_gap.mkv");
    ASSERT_TRUE(WriteTestMkv(file.path(), {"anchor", "delayed"}, 48000, -1, 50));
    const auto metadata = exosnap::engine::ProbeEditMedia(file.path());
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    ASSERT_TRUE(metadata.timestamp_origin_us);
    EXPECT_EQ(*metadata.timestamp_origin_us, 0);
    EditPlayerEngine engine;
    std::string error;
    ASSERT_TRUE(engine.OpenAudioStream(file.path(), 2, error)) << error;
    std::vector<float> samples;
    ASSERT_TRUE(engine.ReadAudioStreamSequential(2, 0, 4800, samples, error)) << error;
    ASSERT_EQ(samples.size(), 9600u);
    EXPECT_TRUE(std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0; }));
    const std::vector<exosnap::engine::AudioSampleSpan> expected{{2400, 4800}};
    EXPECT_EQ(engine.AudioDecodedCoverage(), expected);
}

TEST(EditPlayerEngine, DecodedAudioCoverageUsesTheCommonMediaOrigin) {
    ScopedTestMkv file("edit_player_audio_shifted_origin.mkv");
    ASSERT_TRUE(WriteTestMkv(file.path(), {"delayed"}, 48000, -1, 50));
    const auto metadata = exosnap::engine::ProbeEditMedia(file.path());
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    ASSERT_TRUE(metadata.timestamp_origin_us);
    EXPECT_EQ(*metadata.timestamp_origin_us, 50000);
    EditPlayerEngine engine;
    std::string error;
    ASSERT_TRUE(engine.OpenAudioStream(file.path(), 1, error)) << error;
    std::vector<float> samples;
    ASSERT_TRUE(engine.ReadAudioStreamSequential(1, 0, 4800, samples, error)) << error;
    const std::vector<exosnap::engine::AudioSampleSpan> expected{{0, 4800}};
    EXPECT_EQ(engine.AudioDecodedCoverage(), expected);
}

TEST(EditPlayerEngine, NormalizedDetailWindowKeepsImpulseAlignmentAcrossSourceRates) {
    for (const auto rate : {44100, 48000, 96000}) {
        SCOPED_TRACE(rate);
        ScopedTestMkv file("edit_player_audio_detail_alignment.mkv");
        ASSERT_TRUE(WriteTestMkv(file.path(), {""}, rate, rate / 4));
        EditPlayerEngine reference;
        EditPlayerEngine detail;
        std::string error;
        ASSERT_TRUE(reference.OpenAudioStream(file.path(), 1, error)) << error;
        ASSERT_TRUE(detail.OpenAudioStream(file.path(), 1, error)) << error;
        std::vector<float> canonical, window;
        ASSERT_TRUE(reference.ReadAudioStreamSequential(1, 0, 24000, canonical, error)) << error;
        ASSERT_TRUE(detail.ReadAudioStreamSequential(1, 11800, 600, window, error)) << error;
        ASSERT_EQ(window.size(), 1200u);
        const std::vector<exosnap::engine::AudioSampleSpan> expected{{11800, 12400}};
        EXPECT_EQ(detail.AudioDecodedCoverage(), expected);
        for (std::size_t i = 0; i < window.size(); ++i)
            ASSERT_NEAR(window[i], canonical[23600 + i], 1e-6) << "sample=" << i / 2;
        const auto peak = std::max_element(window.begin(), window.end());
        ASSERT_NE(peak, window.end());
        EXPECT_GT(*peak, 0.1f);
        EXPECT_NEAR(static_cast<double>(std::distance(window.begin(), peak) / 2), 200, 1);
    }
}

TEST(EditPlayerEngine, OpenNonexistentFileFails) {
    EditPlayerEngine engine;
    std::string err;
    EXPECT_FALSE(engine.Open(std::filesystem::path("Z:/does/not/exist.mkv"), err));
    EXPECT_FALSE(err.empty());
}

TEST(EditPlayerEngine, OpeningCancellationDoesNotSurviveIntoPlayback) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    EditPlayerEngine engine;
    std::string error;
    {
        std::atomic<bool> cancelled{true};
        EXPECT_FALSE(engine.Open(fixture, error, &cancelled));
        EXPECT_FALSE(engine.HasVideoStream());
        cancelled.store(false);
        ASSERT_TRUE(engine.Open(fixture, error, &cancelled)) << error;
        cancelled.store(true);
    }
    EXPECT_TRUE(engine.DecodeFrameAt(0));
    EXPECT_TRUE(engine.DecodeFrameAt(1000000));
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
        0, [](exosnap::engine::RawDecodedVideoFrame) {}, [](exosnap::engine::DecodedAudioBlock) {},
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
            0, [](exosnap::engine::RawDecodedVideoFrame) {}, [](exosnap::engine::DecodedAudioBlock) {}, {});
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

// ---- Presentation hold (video waits for the audio clock) -----------------

TEST(EditPlaybackPacing, FrameAheadOfTheClockIsHeldForItsLead) {
    EXPECT_EQ(PresentationHoldUs(1'500'000, 1'000'000), 500'000);
}

TEST(EditPlaybackPacing, DueOrPastFrameIsNotHeld) {
    EXPECT_EQ(PresentationHoldUs(1'000'000, 1'000'000), 0);
    EXPECT_EQ(PresentationHoldUs(900'000, 1'000'000), 0);
}

TEST(EditPlaybackPacing, NoClockNeverHolds) {
    EXPECT_EQ(PresentationHoldUs(1'000'000, -1), 0);
}

TEST(EditPlaybackPacing, ClockIsExtrapolatedBetweenSamplesUpToTheBound) {
    EXPECT_EQ(ExtrapolatedClockUs(1'000'000, 12'000, 100'000), 1'012'000);
    // A clock that stopped advancing must not let video run on without it.
    EXPECT_EQ(ExtrapolatedClockUs(1'000'000, 5'000'000, 100'000), 1'100'000);
    EXPECT_EQ(ExtrapolatedClockUs(1'000'000, -3'000, 100'000), 1'000'000);
    EXPECT_EQ(ExtrapolatedClockUs(-1, 12'000, 100'000), -1);
}

// ---- ShouldAdmitDemuxedPacket (the demuxer's soft/hard queue rule) --------

// Soft 1s / hard 8s / 8192 packets / peer low water 250ms -- the shipped
// shape, restated here so a change to the constants shows up as a test edit.
constexpr exosnap::engine::PacketQueueLimits kLimits{
    /*soft_capacity_us=*/1'000'000,
    /*hard_capacity_us=*/8'000'000,
    /*hard_capacity_packets=*/8192,
    /*peer_low_water_us=*/250'000,
};

// A queue comfortably below the soft limit, with a peer that is well fed.
exosnap::engine::PacketAdmissionState MakeState(int64_t queued_us, int64_t peer_queued_us) {
    exosnap::engine::PacketAdmissionState state;
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
    const exosnap::engine::PacketAdmissionState video_full_audio_starving = MakeState(1'500'000, 100'000);
    const exosnap::engine::PacketAdmissionState audio_full_video_starving = MakeState(1'500'000, 100'000);
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
    exosnap::engine::PacketAdmissionState state = MakeState(0, 0);
    state.queued_packets = 8191;
    EXPECT_TRUE(ShouldAdmitDemuxedPacket(state, kLimits));
    state.queued_packets = 8192;
    EXPECT_FALSE(ShouldAdmitDemuxedPacket(state, kLimits));
}

TEST(EditPlaybackPacing, AbortBeatsEverything) {
    exosnap::engine::PacketAdmissionState state = MakeState(0, 0);
    state.aborted = true;
    EXPECT_FALSE(ShouldAdmitDemuxedPacket(state, kLimits));
}

TEST(EditPlaybackPacing, NoPeerStreamFallsBackToTheSoftLimit) {
    // A video-only clip has nothing that could starve, so the soft limit is
    // the bound -- the overshoot allowance must not fire on an absent peer.
    exosnap::engine::PacketAdmissionState state;
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

TEST(EditPlaybackPacingWait, AnOpenGateIsNotWaitedOn) {
    EXPECT_EQ(DemuxReadAheadWaitUs(1'500'000, 1'000'000, 1'000'000, 2'000, 50'000), 0);
    // No clock: the gate never closes, so there is never anything to wait for.
    EXPECT_EQ(DemuxReadAheadWaitUs(9'000'000, -1, 1'000'000, 2'000, 50'000), 0);
}

TEST(EditPlaybackPacingWait, TheWaitIsTheSurplusTheDemuxerIsAheadBy) {
    // 10 ms past the budget: the clock needs 10 ms of real time to catch up.
    EXPECT_EQ(DemuxReadAheadWaitUs(2'010'000, 1'000'000, 1'000'000, 2'000, 50'000), 10'000);
}

TEST(EditPlaybackPacingWait, TheCapBoundsAClockThatMayHaveStopped) {
    // A second ahead would be a second of not looking at the gate at all.
    EXPECT_EQ(DemuxReadAheadWaitUs(3'000'000, 1'000'000, 1'000'000, 2'000, 50'000), 50'000);
}

TEST(EditPlaybackPacingWait, TheFloorKeepsABarelyClosedGateFromSpinning) {
    // Exactly at the budget closes the gate with a surplus of zero, which
    // without the floor would be a wait of nothing at all.
    EXPECT_FALSE(ShouldDemuxMorePackets(2'000'000, 1'000'000, 1'000'000));
    EXPECT_EQ(DemuxReadAheadWaitUs(2'000'000, 1'000'000, 1'000'000, 2'000, 50'000), 2'000);
    EXPECT_EQ(DemuxReadAheadWaitUs(2'000'001, 1'000'000, 1'000'000, 2'000, 50'000), 2'000);
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
    EXPECT_EQ(DemuxedThroughUs(exosnap::engine::kUnknownDemuxPositionUs, 2'000'000),
              exosnap::engine::kUnknownDemuxPositionUs);
    EXPECT_EQ(DemuxedThroughUs(2'000'000, exosnap::engine::kUnknownDemuxPositionUs),
              exosnap::engine::kUnknownDemuxPositionUs);
}

TEST(EditPlaybackPacing, UnknownDemuxPositionNeverPaces) {
    // Before the first packet with a usable timestamp there is nothing to
    // measure the budget against.
    EXPECT_TRUE(ShouldDemuxMorePackets(exosnap::engine::kUnknownDemuxPositionUs, 0, 1'000'000));
    EXPECT_TRUE(ShouldDemuxMorePackets(exosnap::engine::kUnknownDemuxPositionUs, 600'000'000, 1'000'000));
}

// ---- Raw-frame decode path: DecodeFrameAtRaw / StartPlaybackDecode -----
//
// Track-discovery fixtures have no compressed video bitstream. Decode tests
// require the optional local media fixtures; a missing fixture is reported as
// GTEST_SKIP rather than as successful decode coverage.

#ifndef EXOSNAP_SOURCE_DIR
#define EXOSNAP_SOURCE_DIR "."
#endif

std::filesystem::path TestFixturePath(const char* filename) {
    return std::filesystem::path(EXOSNAP_SOURCE_DIR) / ".workspace" / "test-fixtures" / filename;
}

TEST(EditPlayerEngine, HardwareSeekReadsBackOnlySelectedAndLookaheadFrames) {
    using namespace exosnap::engine;
    const auto fixture = TestFixturePath("audiotest_60fps.mkv");
    if (!std::filesystem::exists(fixture))
        GTEST_SKIP() << "optional long-GOP hardware fixture unavailable";
    struct MeasurementsScope {
        MeasurementsScope() {
            EnablePerformanceMeasurements(true);
        }
        ~MeasurementsScope() {
            EnablePerformanceMeasurements(false);
        }
    } scope;
    EditPlayerEngine engine;
    std::string error;
    ASSERT_TRUE(engine.Open(fixture, error)) << error;
    const auto measure = [&](const char* path, auto decode) {
        const auto readback = ReadPerformanceMeasurement(PerformanceStage::DecodeReadback);
        const auto normalize = ReadPerformanceMeasurement(PerformanceStage::DecodeNormalize);
        const auto start = std::chrono::steady_clock::now();
        const bool succeeded = decode();
        const auto wall = std::chrono::steady_clock::now() - start;
        const auto after = ReadPerformanceMeasurement(PerformanceStage::DecodeReadback);
        std::cout << "seek_measurement path=" << path << " readbacks=" << after.calls - readback.calls
                  << " readback_ms=" << (after.total_ns - readback.total_ns) / 1e6 << " normalize_ms="
                  << (ReadPerformanceMeasurement(PerformanceStage::DecodeNormalize).total_ns - normalize.total_ns) / 1e6
                  << " wall_ms=" << std::chrono::duration<double, std::milli>(wall).count() << '\n';
        EXPECT_TRUE(succeeded);
        return after.calls - readback.calls;
    };
    const auto cpu_readbacks = measure("bgra", [&] {
        const auto frame = engine.DecodeFrameAt(800'000);
        EXPECT_TRUE(frame);
        if (frame) {
            EXPECT_GE(frame->pts_us, 800'000);
            EXPECT_LT(frame->pts_us, 817'000);
        }
        return frame.has_value();
    });
    if (!cpu_readbacks)
        GTEST_SKIP() << "D3D11 decode unavailable; software fallback is not a hardware measurement";
    EXPECT_LE(cpu_readbacks, 1u);
    std::optional<RawDecodedVideoFrame> held;
    const auto raw_readbacks = measure("raw", [&] {
        held = engine.ReadFrameAtRaw(800'000, VideoReadMode::Sequential, 1);
        return held.has_value();
    });
    EXPECT_LE(raw_readbacks, 2u);
    ASSERT_TRUE(held);
    EXPECT_LE(held->pts_us, 800'500);
    EXPECT_LT(800'000 - held->pts_us, 17'000);
    const auto pixel = held->y_plane[0];
    const auto before = ReadPerformanceMeasurement(PerformanceStage::DecodeReadback).calls;
    const auto cached = engine.ReadFrameAtRaw(800'001, VideoReadMode::Sequential, 1);
    ASSERT_TRUE(cached);
    EXPECT_EQ(cached->pts_us, held->pts_us);
    EXPECT_EQ(ReadPerformanceMeasurement(PerformanceStage::DecodeReadback).calls, before);
    ASSERT_TRUE(engine.ReadFrameAtRaw(100'000, VideoReadMode::RandomAccess));
    engine.Close();
    EXPECT_EQ(held->y_plane[0], pixel);
}

// Sums the Y plane's first row. Not a meaningful image checksum by itself --
// it exists only to prove the bytes behind y_plane are still the SAME real
// decoded pixel data across two points in time (a freed/reused buffer would
// almost certainly read differently, and a zeroed one would read as 0).
uint64_t ChecksumFirstRow(const exosnap::engine::RawDecodedVideoFrame& frame) {
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
        0, [&](exosnap::engine::RawDecodedVideoFrame) { ++video_calls; }, [](exosnap::engine::DecodedAudioBlock) {},
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
    std::vector<exosnap::engine::RawDecodedVideoFrame> frames;
    constexpr size_t kWanted = 5;

    {
        EditPlayerEngine engine;
        std::string err;
        ASSERT_TRUE(engine.Open(fixture, err)) << err;
        ASSERT_TRUE(engine.HasVideoStream());

        const auto metadata = engine.AudioMetadata();
        exosnap::engine::MonitorSelectionSnapshot selection;
        selection.media_id = metadata.selection_media_id;
        selection.revision = {1};
        selection.overlap_confirmed = true;
        selection.unverified_asset_binding = metadata.unverified_asset_binding;
        ASSERT_TRUE(engine.SetMonitorSelection(selection, err)) << err;

        // No media clock: nothing is presenting these frames, so nothing is
        // discarded before conversion (ShouldConvertDecodedFrame) -- every
        // decoded frame from t=0 is delivered, matching probe_edit_playback's
        // step B throughput-measurement convention.
        engine.StartPlaybackDecode(
            0,
            [&](exosnap::engine::RawDecodedVideoFrame frame) {
                std::lock_guard<std::mutex> lock(mu);
                if (frames.size() < kWanted) {
                    frames.push_back(std::move(frame));
                    if (frames.size() == kWanted)
                        cv.notify_one();
                }
            },
            [](exosnap::engine::DecodedAudioBlock) {}, {});

        {
            std::unique_lock<std::mutex> lock(mu);
            ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(30), [&] { return frames.size() >= kWanted; }))
                << "timed out waiting for " << kWanted << " raw video frames from " << fixture.string();
        }
        engine.StopPlaybackDecode();

        ASSERT_EQ(frames.size(), kWanted);
        for (size_t i = 0; i < kWanted; ++i) {
            const exosnap::engine::RawDecodedVideoFrame& f = frames[i];
            EXPECT_NE(f.backing_frame, nullptr) << "frame " << i;
            ASSERT_NE(f.y_plane, nullptr) << "frame " << i;
            ASSERT_NE(f.u_plane, nullptr) << "frame " << i;
            ASSERT_NE(f.v_plane, nullptr) << "frame " << i;
            EXPECT_GT(f.width, 0u) << "frame " << i;
            EXPECT_GT(f.height, 0u) << "frame " << i;
            EXPECT_EQ(f.format, exosnap::engine::DecodedPixelFormat::Yuv420P8) << "frame " << i;
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

    const std::optional<exosnap::engine::RawDecodedVideoFrame> frame = engine.DecodeFrameAtRaw(0);
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->is_pq_source);
    EXPECT_EQ(frame->format, exosnap::engine::DecodedPixelFormat::Yuv420P10);
    EXPECT_NE(frame->backing_frame, nullptr);
    ASSERT_NE(frame->y_plane, nullptr);
    EXPECT_GT(frame->width, 0u);
    EXPECT_GT(frame->height, 0u);
}

// The fixture's 1/15360 s time base: a stored timestamp stands for any instant
// within half a tick of it.
constexpr int64_t kFixtureHalfTickUs = 32;

TEST(EditPlayerEngine, ForwardPlaybackDoesNotReseekEachFrame) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    EditPlayerEngine engine;
    std::string error;
    ASSERT_TRUE(engine.Open(fixture, error)) << error;
    for (int64_t index = 0; index < 30; ++index) {
        const auto target = index * 1'000'000 / 30;
        const auto frame = engine.ReadFrameAtRaw(target, exosnap::engine::VideoReadMode::Sequential, 1);
        ASSERT_TRUE(frame) << index;
        EXPECT_LE(frame->pts_us, target + kFixtureHalfTickUs);
        EXPECT_LT(target - frame->pts_us, 34'000);
    }
    const auto measurements = engine.VideoReadStats();
    EXPECT_EQ(measurements.seeks, 1u);
    EXPECT_EQ(measurements.flushes, 1u);
    EXPECT_EQ(measurements.selected, 30u);
    EXPECT_LE(measurements.decoded, 31u);
}

TEST(EditPlayerEngine, ForwardCursorRetainsOwnedFrameAndReseeksOnDiscontinuity) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    EditPlayerEngine engine;
    std::string error;
    ASSERT_TRUE(engine.Open(fixture, error)) << error;
    const auto first = engine.ReadFrameAtRaw(0, exosnap::engine::VideoReadMode::Sequential, 1);
    ASSERT_TRUE(first);
    const auto pixel = first->y_plane[0];
    ASSERT_TRUE(engine.ReadFrameAtRaw(1'000'000, exosnap::engine::VideoReadMode::Sequential, 1));
    ASSERT_TRUE(engine.ReadFrameAtRaw(100'000, exosnap::engine::VideoReadMode::Sequential, 1));
    EXPECT_EQ(engine.VideoReadStats().seeks, 2u);
    ASSERT_TRUE(engine.ReadFrameAtRaw(200'000, exosnap::engine::VideoReadMode::Sequential, 2));
    EXPECT_EQ(engine.VideoReadStats().seeks, 3u);
    engine.Close();
    EXPECT_EQ(first->y_plane[0], pixel);
}

TEST(EditPlayerEngine, ForwardCursorHoldsSelectedFrameWithoutDecodingAgain) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    EditPlayerEngine engine;
    std::string error;
    ASSERT_TRUE(engine.Open(fixture, error)) << error;
    const auto first = engine.ReadFrameAtRaw(1, exosnap::engine::VideoReadMode::Sequential);
    ASSERT_TRUE(first);
    const auto decoded = engine.VideoReadStats().decoded;
    const auto held = engine.ReadFrameAtRaw(2, exosnap::engine::VideoReadMode::Sequential);
    ASSERT_TRUE(held);
    EXPECT_EQ(first->pts_us, held->pts_us);
    EXPECT_EQ(engine.VideoReadStats().decoded, decoded);
    EXPECT_EQ(engine.VideoReadStats().held, 1u);
}

TEST(EditPlayerEngine, ForwardCursorNeverSelectsPastRequestedSourceTime) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    EditPlayerEngine engine;
    std::string error;
    ASSERT_TRUE(engine.Open(fixture, error)) << error;
    constexpr int64_t source_in = 1'000'000;
    for (const auto mode : {exosnap::engine::VideoReadMode::RandomAccess, exosnap::engine::VideoReadMode::Sequential}) {
        const auto first = engine.ReadFrameAtRaw(source_in, mode, 7);
        ASSERT_TRUE(first);
        ASSERT_EQ(first->pts_us, source_in);
        const auto before_next = engine.ReadFrameAtRaw(source_in + 33'333 - kFixtureHalfTickUs - 1, mode, 7);
        ASSERT_TRUE(before_next);
        EXPECT_EQ(before_next->pts_us, source_in);
        const auto after_next = engine.ReadFrameAtRaw(source_in + 33'334, mode, 7);
        ASSERT_TRUE(after_next);
        EXPECT_GT(after_next->pts_us, source_in);
        EXPECT_LE(after_next->pts_us, source_in + 33'334);
        const auto backward = engine.ReadFrameAtRaw(source_in + 1, mode, 8);
        ASSERT_TRUE(backward);
        EXPECT_EQ(backward->pts_us, source_in);
        EXPECT_EQ(first->y_plane[0], backward->y_plane[0]);
    }
}

// Stream-copies `source` into Matroska with its default millisecond time base,
// the way most third-party Matroska files store a 30 fps stream: every third
// frame's instant (66.667 ms, 166.667 ms, ...) is stamped rounded UP.
bool RemuxToMillisecondMatroska(const std::filesystem::path& source, const std::filesystem::path& destination) {
    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, source.string().c_str(), nullptr, nullptr) < 0)
        return false;
    bool ok = avformat_find_stream_info(input, nullptr) >= 0;
    AVFormatContext* output = nullptr;
    ok = ok && avformat_alloc_output_context2(&output, nullptr, "matroska", nullptr) >= 0 && output != nullptr;
    int video = -1;
    for (unsigned index = 0; ok && index < input->nb_streams; ++index) {
        if (input->streams[index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || video >= 0)
            continue;
        AVStream* stream = avformat_new_stream(output, nullptr);
        ok = stream != nullptr && avcodec_parameters_copy(stream->codecpar, input->streams[index]->codecpar) >= 0;
        if (ok) {
            stream->codecpar->codec_tag = 0;
            stream->time_base = AVRational{1, 1000};
            video = static_cast<int>(index);
        }
    }
    ok = ok && video >= 0 && avio_open(&output->pb, destination.string().c_str(), AVIO_FLAG_WRITE) >= 0;
    ok = ok && avformat_write_header(output, nullptr) >= 0;
    AVPacket* packet = av_packet_alloc();
    while (ok && packet != nullptr && av_read_frame(input, packet) >= 0) {
        if (packet->stream_index == video) {
            av_packet_rescale_ts(packet, input->streams[video]->time_base, output->streams[0]->time_base);
            packet->stream_index = 0;
            packet->pos = -1;
            ok = av_interleaved_write_frame(output, packet) >= 0;
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    if (ok)
        ok = av_write_trailer(output) >= 0;
    if (output != nullptr) {
        if (output->pb != nullptr)
            avio_closep(&output->pb);
        avformat_free_context(output);
    }
    avformat_close_input(&input);
    return ok;
}

TEST(EditPlayerEngine, MillisecondStampsStillYieldEveryFrameOfTheSourceGrid) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    ScopedTestMkv remuxed("exosnap-ms-stamped-30fps.mkv");
    ASSERT_TRUE(RemuxToMillisecondMatroska(fixture, remuxed.path()));
    EditPlayerEngine engine;
    std::string error;
    ASSERT_TRUE(engine.Open(remuxed.path(), error)) << error;
    // The frame grid of a 30 fps output, rounded to whole microseconds the way
    // a timeline position is. Strictly at-or-before selection held the
    // previous frame wherever the stamp was rounded up, showing one frame
    // twice and the next one never.
    int64_t previous = -1;
    for (int64_t index = 0; index < 30; ++index) {
        const auto target = (index * 1'000'000 + 15) / 30;
        const auto frame = engine.ReadFrameAtRaw(target, exosnap::engine::VideoReadMode::Sequential, 1);
        ASSERT_TRUE(frame) << index;
        EXPECT_GT(frame->pts_us, previous) << "frame " << index << " repeated its predecessor";
        EXPECT_LE(std::abs(frame->pts_us - target), 500) << index;
        previous = frame->pts_us;
    }
}

TEST(EditPlayerEngine, SourceRelativeTimePreservesSignedOrigins) {
    EXPECT_EQ(exosnap::engine::SourceRelativeTimeUs(-9'800'000, -10'000'000), 200'000);
    EXPECT_EQ(exosnap::engine::SourceTimestampUs(200'000, -10'000'000), -9'800'000);
    EXPECT_EQ(exosnap::engine::SourceRelativeTimeUs(10'200'000, 10'000'000), 200'000);
    EXPECT_EQ(exosnap::engine::SourceTimestampUs(200'000, 10'000'000), 10'200'000);
    EXPECT_EQ(exosnap::engine::SourceRelativeFrameTimeUs(-9800, {1, 1000}, -10'000'000), 200'000);
    EXPECT_EQ(exosnap::engine::SourceRelativeFrameTimeUs(10200, {1, 1000}, 10'000'000), 200'000);
    EXPECT_EQ(exosnap::engine::SourceRelativeFrameTimeUs(AV_NOPTS_VALUE, {1, 1000}, 10'000'000), 0);
    EXPECT_EQ(exosnap::engine::SourceRelativeFrameTimeUs(AV_NOPTS_VALUE, {1, 1000}, -10'000'000), 0);
}

TEST(EditPlayerEngine, SourceRelativeVideoAndAudioIgnoreShiftedContainerOrigins) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "timestamp_av.mkv";
    EditPlayerEngine reference;
    std::string error;
    ASSERT_TRUE(reference.Open(fixture, error)) << error;
    for (const int64_t offset : {0LL, 10'000'000LL}) {
        ScopedTestMkv shifted("exosnap-timestamp-origin.mkv");
        ASSERT_TRUE(RemuxTimestampMedia(shifted.path(), offset));
        const auto metadata = exosnap::engine::ProbeEditMedia(shifted.path());
        ASSERT_TRUE(metadata.timestamp_origin_us);
        EXPECT_EQ(*metadata.timestamp_origin_us > 0, offset > 0);
        const auto keyframes = exosnap::engine::ExtractKeyframeTimestamps(shifted.path());
        ASSERT_FALSE(keyframes.empty());
        EXPECT_EQ(keyframes.front(), 0);
        EditPlayerEngine engine;
        ASSERT_TRUE(engine.Open(shifted.path(), error)) << error;
        for (const int64_t target : {200'000LL, 700'000LL, 300'000LL}) {
            const auto expected = reference.ReadFrameAtRaw(target, exosnap::engine::VideoReadMode::RandomAccess);
            const auto actual = engine.ReadFrameAtRaw(target, exosnap::engine::VideoReadMode::RandomAccess);
            ASSERT_TRUE(expected);
            ASSERT_TRUE(actual) << offset << ": " << target;
            EXPECT_EQ(actual->pts_us, expected->pts_us);
            EXPECT_EQ(actual->y_plane[0], expected->y_plane[0]);
            const auto thumbnail = engine.DecodeFrameAt(target);
            ASSERT_TRUE(thumbnail);
            EXPECT_GE(thumbnail->pts_us, target);
            EXPECT_LT(thumbnail->pts_us, target + 34'000);
        }
        std::vector<float> expected_audio, actual_audio;
        ASSERT_TRUE(reference.DecodeAudioStreamRange(1, 9600, 480, expected_audio, error)) << error;
        ASSERT_TRUE(engine.DecodeAudioStreamRange(1, 9600, 480, actual_audio, error)) << error;
        ASSERT_EQ(actual_audio.size(), expected_audio.size());
        for (size_t i = 0; i < actual_audio.size(); ++i)
            ASSERT_NEAR(actual_audio[i], expected_audio[i], 0.0005f) << offset << ": " << i;
        const auto audio = engine.AudioMetadata();
        ASSERT_EQ(audio.actual_tracks.size(), 1u);
        const exosnap::engine::MonitorSelectionSnapshot selection{audio.selection_media_id,
                                                                  {1},
                                                                  {{audio.actual_tracks.front().track_id, 1.0f, false}},
                                                                  true,
                                                                  audio.unverified_asset_binding};
        ASSERT_TRUE(engine.SetMonitorSelection(selection, error)) << error;
        ASSERT_TRUE(engine.DecodeAudioRange(9600, 480, actual_audio, error)) << error;
        ASSERT_EQ(actual_audio.size(), expected_audio.size());
        for (size_t i = 0; i < actual_audio.size(); ++i)
            ASSERT_NEAR(actual_audio[i], expected_audio[i], 0.0005f) << offset << ": " << i;
    }
}

TEST(EditPlayerEngine, StreamCopyTrimUsesSourceRelativeBoundariesWithShiftedOrigins) {
    for (const int64_t offset : {0LL, 10'000'000LL}) {
        ScopedTestMkv shifted("exosnap-copy-origin-source.mkv");
        ScopedTestMkv copied("exosnap-copy-origin-result.mp4");
        ASSERT_TRUE(RemuxTimestampMedia(shifted.path(), offset));
        ASSERT_TRUE(exosnap::engine::ProbeEditMedia(shifted.path()).timestamp_origin_us);
        const auto result = exosnap::engine::RemuxToProgressiveMp4(shifted.path(), copied.path(),
                                                                   exosnap::engine::RemuxNoopCallback(), {0, 200'000});
        ASSERT_TRUE(result.success) << result.message;
        const auto metadata = exosnap::engine::ProbeEditMedia(copied.path());
        EXPECT_GT(metadata.duration_us, 0);
        EXPECT_LE(metadata.duration_us, 1'100'000);
        EditPlayerEngine engine;
        std::string error;
        ASSERT_TRUE(engine.Open(copied.path(), error)) << error;
        ASSERT_TRUE(engine.ReadFrameAtRaw(100'000, exosnap::engine::VideoReadMode::RandomAccess));
    }
}

TEST(EditPlayerEngine, LegacyRawSeekStillSelectsAtOrAfterRequestedTime) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    EditPlayerEngine engine;
    std::string error;
    ASSERT_TRUE(engine.Open(fixture, error)) << error;
    const auto frame = engine.DecodeFrameAtRaw(1'033'332);
    ASSERT_TRUE(frame);
    EXPECT_GE(frame->pts_us, 1'033'332);
}

TEST(EditPlayerEngine, Decodes420And444WithHardwarePermittedAndForcedSoftware) {
    struct DecodeMode {
        std::optional<std::string> previous;
        DecodeMode() {
            if (const char* value = std::getenv("EXOSNAP_EDIT_SOFTWARE_DECODE"))
                previous = value;
        }
        ~DecodeMode() {
            _putenv_s("EXOSNAP_EDIT_SOFTWARE_DECODE", previous ? previous->c_str() : "");
        }
    } decode_mode;
    using exosnap::engine::DecodedPixelFormat;
    struct Fixture {
        const char* filename;
        DecodedPixelFormat format;
        exosnap::engine::ColorRange range = exosnap::engine::ColorRange::Limited;
    };
    const Fixture fixtures[] = {
        {"h264_420_8.mp4", DecodedPixelFormat::Yuv420P8},
        {"h264_420_8_full.mp4", DecodedPixelFormat::Yuv420P8, exosnap::engine::ColorRange::Full},
        {"hevc_420_10.mp4", DecodedPixelFormat::Yuv420P10},
        {"h264_444_8.mp4", DecodedPixelFormat::Yuv444P8},
        {"hevc_444_8.mp4", DecodedPixelFormat::Yuv444P8},
    };
    for (const bool software : {false, true}) {
        ASSERT_EQ(_putenv_s("EXOSNAP_EDIT_SOFTWARE_DECODE", software ? "1" : ""), 0);
        for (const auto& [filename, format, range] : fixtures) {
            SCOPED_TRACE(filename);
            SCOPED_TRACE(software ? "forced software" : "hardware permitted");
            const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / filename;
            EditPlayerEngine engine;
            std::string error;
            ASSERT_TRUE(engine.Open(path, error)) << error;
            for (const int64_t target : {0LL, 100'000LL, 200'000LL}) {
                const auto raw = engine.DecodeFrameAtRaw(target);
                ASSERT_TRUE(raw) << target;
                EXPECT_EQ(raw->format, format);
                EXPECT_EQ(raw->range, range);
                EXPECT_EQ(raw->width, 32u);
                EXPECT_EQ(raw->height, 32u);
                ASSERT_NE(raw->y_plane, nullptr);
                ASSERT_NE(raw->u_plane, nullptr);
                ASSERT_NE(raw->v_plane, nullptr);
                if (range == exosnap::engine::ColorRange::Full) {
                    EXPECT_EQ(raw->y_plane[0], 64);
                    EXPECT_EQ(raw->y_plane[16], 192);
                    const auto* decoded = static_cast<const AVFrame*>(raw->backing_frame.get());
                    EXPECT_EQ(decoded->color_range, AVCOL_RANGE_JPEG);
                    EXPECT_EQ(decoded->colorspace, AVCOL_SPC_BT709);
                    if (software)
                        EXPECT_EQ(decoded->format, AV_PIX_FMT_YUVJ420P);
                }
            }
            const auto frame = engine.DecodeFrameAt(0);
            ASSERT_TRUE(frame);
            ASSERT_NE(frame->bgra, nullptr);
            if (range == exosnap::engine::ColorRange::Full) {
                for (const int channel : {0, 1, 2}) {
                    EXPECT_NEAR(frame->bgra[channel], 64, 1);
                    EXPECT_NEAR(frame->bgra[16 * 4 + channel], 192, 1);
                }
            } else {
                EXPECT_LT(frame->bgra[0], 80);
                EXPECT_LT(frame->bgra[1], 80);
                EXPECT_GT(frame->bgra[2], 180);
            }
        }
    }
}
} // namespace
