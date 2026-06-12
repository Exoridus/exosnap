// test_mp4_remuxer.cpp — unit tests for the MP4 remux engine (ADR-0014)
//
// Fixture strategy: generate a minimal MKV at test time using MatroskaStreamWriter
// with the existing synthetic packet helpers from test_matroska_stream_writer.cpp.
// The AAC codec private (AudioSpecificConfig) written by the fixture is valid ISO
// 14496-3 data so libavformat can parse the stream headers correctly.
//
// Tests:
//   1. SuccessfulRemux        — basic MKV→MP4, re-open output and read packets,
//                               duration plausible, moov-before-mdat verified.
//   2. ProgressCallbackFires  — callback is called and values are monotone in [0,1].
//   3. CancelAborts           — cancel mid-way: returns failure, output file removed.
//   4. BadInputReturnsError   — non-existent input → structured failure (av_error_code).
//   5. MultiTrackRemux        — two audio tracks; output has ≥2 audio streams.

// libavformat
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

// MSVC + C++: av_err2str fix
static inline const char* av_err2str_cpp_test(int errnum) noexcept {
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}

#include <gtest/gtest.h>

#include "matroska_stream_writer.h"
#include "recorder_core/mp4_remuxer.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using recorder_core::MatroskaStreamConfig;
using recorder_core::MatroskaStreamWriter;
using recorder_core::MuxPacket;
using recorder_core::RemuxNoopCallback;
using recorder_core::RemuxResult;
using recorder_core::RemuxToMkv;
using recorder_core::RemuxToProgressiveMp4;

// ---------------------------------------------------------------------------
// Codec private data helpers
// ---------------------------------------------------------------------------

// Minimal valid AVCC record (SPS + PPS stubs that will parse without error).
// This is the same stub used by test_matroska_stream_writer.cpp.
std::vector<uint8_t> FakeH264Cp() {
    return {0x01, 0x42, 0x00, 0x1F, 0xFF, 0xE1, 0x00};
}

// Valid 2-byte AudioSpecificConfig for AAC-LC 48 kHz stereo:
//   Object type = 2 (AAC-LC)  → bits [0..4]: 00010
//   Sampling freq index = 3 (48000 Hz) → bits [5..8]: 0011
//   Channel config = 2 (stereo) → bits [9..12]: 0010
//   Frame length flag = 0       → bit 13: 0
//   Depends on core coder = 0   → bit 14: 0
//   Extension flag = 0          → bit 15: 0
//
//   Byte 0: 0001 0011  = 0x13
//   Byte 1: 0010 0000  = 0x90  (channel config=2 in bits[1..4], rest 0)
//
// Bit layout:
//   [0-4]  = audioObjectType - 1  = 00010 - 1 → stored as 00010 (2 = AAC-LC)
//   [5-8]  = samplingFreqIndex    = 0011 (48000 Hz)
//   [9-12] = channelConfig        = 0010 (stereo)
//   [13]   = frameLengthFlag      = 0
//   [14]   = dependsOnCoreCoder   = 0
//   [15]   = extensionFlag        = 0
//
// Packing:
//   0001 0011 = 0x13
//   0010 0000 = 0x90
static std::vector<uint8_t> ValidAacCp() {
    return {0x13, 0x90};
}

// ---------------------------------------------------------------------------
// MatroskaStreamWriter config factory
// ---------------------------------------------------------------------------

MatroskaStreamConfig MakeConfig(const std::string& path, uint32_t audio_track_count = 1) {
    MatroskaStreamConfig c;
    c.output_path = path;
    c.video_codec_id = "V_MPEG4/ISO/AVC";
    c.video_codec_private = FakeH264Cp();
    c.encode_width = 1280;
    c.encode_height = 720;
    c.frame_rate_num = 60;
    c.frame_rate_den = 1;
    c.audio_is_opus = false; // AAC
    c.audio_track_count = audio_track_count;
    for (uint32_t i = 0; i < audio_track_count; ++i) {
        c.audio_tracks[i].codec_private = ValidAacCp();
    }
    return c;
}

// ---------------------------------------------------------------------------
// Packet feeder — identical pattern to test_matroska_stream_writer.cpp
// ---------------------------------------------------------------------------

void FeedSeconds(MatroskaStreamWriter& w, double seconds, int gop, size_t payload_bytes,
                 uint32_t audio_track_count = 1) {
    const uint64_t vframe = 1000000000ULL / 60;
    const uint64_t aframe = 1024ULL * 1000000000ULL / 48000ULL;
    const uint64_t total_ns = static_cast<uint64_t>(seconds * 1e9);
    const std::vector<uint8_t> blob(payload_bytes, 0xAB);

    uint64_t vpts = 0;
    int vidx = 0;

    // Per-track audio PTS: track 0 = audio track 1, track 1 = audio track 2, etc.
    std::vector<uint64_t> atrack_pts(audio_track_count, 0u);

    while (true) {
        // Find the earliest stream.
        uint64_t min_pts = vpts < total_ns ? vpts : UINT64_MAX;
        for (uint32_t t = 0; t < audio_track_count; ++t) {
            if (atrack_pts[t] < total_ns)
                min_pts = std::min(min_pts, atrack_pts[t]);
        }
        if (min_pts == UINT64_MAX)
            break;

        if (vpts <= min_pts && vpts < total_ns) {
            MuxPacket p;
            p.pts_ns = vpts;
            p.track_num = 1;
            p.is_key = (vidx % gop == 0);
            p.bytes = blob;
            w.Push(std::move(p));
            vpts += vframe;
            ++vidx;
        } else {
            for (uint32_t t = 0; t < audio_track_count; ++t) {
                if (atrack_pts[t] == min_pts && atrack_pts[t] < total_ns) {
                    MuxPacket p;
                    p.pts_ns = atrack_pts[t];
                    p.track_num = 2u + t; // track_num 2, 3, 4 ...
                    p.is_key = true;
                    p.bytes = blob;
                    w.Push(std::move(p));
                    atrack_pts[t] += aframe;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Build a test MKV from synthetic packets, return the path.
// ---------------------------------------------------------------------------
std::string BuildTestMkv(const std::string& path, double seconds = 3.0, uint32_t audio_tracks = 1) {
    MatroskaStreamWriter w;
    auto cfg = MakeConfig(path, audio_tracks);
    if (!w.Open(cfg))
        return {};
    FeedSeconds(w, seconds, /*gop=*/60, /*payload=*/256, audio_tracks);
    if (!w.Finalize())
        return {};
    return path;
}

// ---------------------------------------------------------------------------
// Box scanner: verify moov appears before mdat in the output file.
// ---------------------------------------------------------------------------
struct BoxHeader {
    uint32_t type_tag = 0; // big-endian 4-byte FourCC as uint32
    std::string type;
    size_t offset = 0;
    uint64_t size = 0;
};

// Read the first ~8 top-level ISO BMFF box headers from a file.
std::vector<BoxHeader> ScanBoxHeaders(const std::string& path, size_t max_boxes = 8) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};

    std::vector<BoxHeader> out;
    size_t offset = 0;

    for (size_t i = 0; i < max_boxes; ++i) {
        uint8_t hdr[8] = {};
        if (!f.read(reinterpret_cast<char*>(hdr), 8))
            break;

        BoxHeader b;
        b.offset = offset;

        // size field (4 bytes big-endian)
        uint32_t sz32 = (static_cast<uint32_t>(hdr[0]) << 24) | (static_cast<uint32_t>(hdr[1]) << 16) |
                        (static_cast<uint32_t>(hdr[2]) << 8) | static_cast<uint32_t>(hdr[3]);

        b.type.assign(reinterpret_cast<const char*>(hdr + 4), 4);

        if (sz32 == 1) {
            // Extended size: next 8 bytes
            uint8_t ext[8] = {};
            if (!f.read(reinterpret_cast<char*>(ext), 8))
                break;
            b.size = (static_cast<uint64_t>(ext[0]) << 56) | (static_cast<uint64_t>(ext[1]) << 48) |
                     (static_cast<uint64_t>(ext[2]) << 40) | (static_cast<uint64_t>(ext[3]) << 32) |
                     (static_cast<uint64_t>(ext[4]) << 24) | (static_cast<uint64_t>(ext[5]) << 16) |
                     (static_cast<uint64_t>(ext[6]) << 8) | static_cast<uint64_t>(ext[7]);
            offset += b.size;
            // Seek to next box
            f.seekg(static_cast<std::streamoff>(b.size - 16), std::ios::cur);
        } else {
            b.size = sz32;
            offset += b.size;
            f.seekg(static_cast<std::streamoff>(b.size - 8), std::ios::cur);
        }

        out.push_back(std::move(b));
    }
    return out;
}

// Returns true if "moov" appears before "mdat" in the box sequence.
bool MoovBeforeMdat(const std::string& path) {
    auto boxes = ScanBoxHeaders(path, 12);
    int moov_idx = -1, mdat_idx = -1;
    for (int i = 0; i < static_cast<int>(boxes.size()); ++i) {
        if (boxes[i].type == "moov")
            moov_idx = i;
        if (boxes[i].type == "mdat")
            mdat_idx = i;
    }
    return (moov_idx >= 0 && mdat_idx >= 0 && moov_idx < mdat_idx);
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class RemuxerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        auto tmp = std::filesystem::temp_directory_path();
        mkv_path_ = (tmp / "exosnap_remux_test_src.mkv").string();
        mp4_path_ = (tmp / "exosnap_remux_test_out.mp4").string();
        std::remove(mkv_path_.c_str());
        std::remove(mp4_path_.c_str());
    }
    void TearDown() override {
        std::remove(mkv_path_.c_str());
        std::remove(mp4_path_.c_str());
    }
    std::string mkv_path_;
    std::string mp4_path_;
};

// ---------------------------------------------------------------------------
// Test 1: successful remux — open output, read packets, duration plausible,
//         moov-before-mdat confirmed.
// ---------------------------------------------------------------------------
TEST_F(RemuxerTest, SuccessfulRemux) {
    ASSERT_FALSE(BuildTestMkv(mkv_path_).empty()) << "Failed to build test MKV fixture";

    const auto result = RemuxToProgressiveMp4(mkv_path_, mp4_path_);
    ASSERT_TRUE(result.success) << "Remux failed: " << result.message << " (av_err=" << result.av_error_code << ")";

    // Output file must exist and be non-empty.
    ASSERT_GT(std::filesystem::file_size(mp4_path_), 0u);

    // moov must appear before mdat (+faststart confirmed).
    EXPECT_TRUE(MoovBeforeMdat(mp4_path_)) << "moov not before mdat — +faststart may not be working";

    // Re-open with libavformat and verify we can read packets.
    AVFormatContext* ctx = nullptr;
    int ret = avformat_open_input(&ctx, mp4_path_.c_str(), nullptr, nullptr);
    ASSERT_EQ(ret, 0) << "avformat_open_input on output failed: " << av_err2str_cpp_test(ret);
    ASSERT_NE(ctx, nullptr);

    ret = avformat_find_stream_info(ctx, nullptr);
    EXPECT_GE(ret, 0) << "avformat_find_stream_info failed";

    // Duration should be plausible (we fed 3 seconds).
    if (ctx->duration != AV_NOPTS_VALUE && ctx->duration > 0) {
        const double dur = static_cast<double>(ctx->duration) / AV_TIME_BASE;
        EXPECT_GE(dur, 1.0) << "Output duration unexpectedly short";
        EXPECT_LE(dur, 10.0) << "Output duration unexpectedly long";
    }

    // Must have at least one stream.
    EXPECT_GE(ctx->nb_streams, 1u);

    // Read a few packets successfully.
    AVPacket* pkt = av_packet_alloc();
    ASSERT_NE(pkt, nullptr);
    int packet_count = 0;
    while (av_read_frame(ctx, pkt) == 0) {
        ++packet_count;
        av_packet_unref(pkt);
        if (packet_count >= 10)
            break;
    }
    av_packet_free(&pkt);
    avformat_close_input(&ctx);

    EXPECT_GE(packet_count, 1) << "Could not read any packets from the output MP4";
}

// ---------------------------------------------------------------------------
// Test 2: progress callback fires with monotone values in [0, 1].
// ---------------------------------------------------------------------------
TEST_F(RemuxerTest, ProgressCallbackFiresMonotone) {
    ASSERT_FALSE(BuildTestMkv(mkv_path_).empty());

    std::vector<float> progress_values;
    auto cb = [&progress_values](float p) -> bool {
        progress_values.push_back(p);
        return true; // keep going
    };

    const auto result = RemuxToProgressiveMp4(mkv_path_, mp4_path_, cb);
    ASSERT_TRUE(result.success) << result.message;

    // At least one callback.
    EXPECT_GE(progress_values.size(), 1u);

    // All values must be in [0, 1].
    for (float v : progress_values) {
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
    }

    // Values must be non-decreasing (monotone).
    for (size_t i = 1; i < progress_values.size(); ++i) {
        EXPECT_GE(progress_values[i], progress_values[i - 1]) << "Progress decreased from " << progress_values[i - 1]
                                                              << " to " << progress_values[i] << " at index " << i;
    }

    // Final call must be 1.0 (100%).
    EXPECT_FLOAT_EQ(progress_values.back(), 1.0f);
}

// ---------------------------------------------------------------------------
// Test 3: cancel mid-way aborts and removes the output file.
// ---------------------------------------------------------------------------
TEST_F(RemuxerTest, CancelAbortsCleansUp) {
    // Build a longer fixture so we have time to cancel mid-stream.
    ASSERT_FALSE(BuildTestMkv(mkv_path_, 5.0).empty());

    std::atomic<int> call_count{0};
    auto cb = [&call_count](float) -> bool {
        ++call_count;
        // Cancel after first call.
        return call_count.load() < 2;
    };

    const auto result = RemuxToProgressiveMp4(mkv_path_, mp4_path_, cb);

    EXPECT_FALSE(result.success) << "Expected failure on cancel";
    // Partial output file must have been removed.
    EXPECT_FALSE(std::filesystem::exists(mp4_path_)) << "Partial output file was not cleaned up after cancel";
}

// ---------------------------------------------------------------------------
// Test 4: non-existent input returns a structured error.
// ---------------------------------------------------------------------------
TEST_F(RemuxerTest, BadInputReturnsStructuredError) {
    const auto result = RemuxToProgressiveMp4("/nonexistent_path_xyz_exosnap_test_abc.mkv", mp4_path_);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.av_error_code, 0) << "Expected a non-zero av_error_code on open failure";
    EXPECT_FALSE(result.message.empty()) << "Expected a non-empty error message";
}

// ---------------------------------------------------------------------------
// Test 5: multi-track (2 audio tracks) — output has the correct stream count.
// ---------------------------------------------------------------------------
TEST_F(RemuxerTest, MultiTrackRemux) {
    constexpr uint32_t kAudioTracks = 2;
    ASSERT_FALSE(BuildTestMkv(mkv_path_, 3.0, kAudioTracks).empty()) << "Failed to build multi-track test MKV";

    const auto result = RemuxToProgressiveMp4(mkv_path_, mp4_path_);
    ASSERT_TRUE(result.success) << "Multi-track remux failed: " << result.message;

    // Verify stream count in output.
    AVFormatContext* ctx = nullptr;
    ASSERT_EQ(avformat_open_input(&ctx, mp4_path_.c_str(), nullptr, nullptr), 0);
    ASSERT_EQ(avformat_find_stream_info(ctx, nullptr) >= 0, true);

    // 1 video + 2 audio = 3 streams total.
    EXPECT_EQ(ctx->nb_streams, 1u + kAudioTracks);

    // Count audio streams.
    unsigned audio_stream_count = 0;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            ++audio_stream_count;
    }
    EXPECT_EQ(audio_stream_count, kAudioTracks);

    avformat_close_input(&ctx);
}

// ---------------------------------------------------------------------------
// RemuxToMkv tests — output opens, duration plausible, streams intact, cancel.
// ---------------------------------------------------------------------------

class MkvRemuxerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        auto tmp = std::filesystem::temp_directory_path();
        src_mkv_path_ = (tmp / "exosnap_mkv_remux_src.mkv").string();
        out_mkv_path_ = (tmp / "exosnap_mkv_remux_out.mkv").string();
        std::remove(src_mkv_path_.c_str());
        std::remove(out_mkv_path_.c_str());
    }
    void TearDown() override {
        std::remove(src_mkv_path_.c_str());
        std::remove(out_mkv_path_.c_str());
    }
    std::string src_mkv_path_;
    std::string out_mkv_path_;
};

// Test 6: successful MKV→MKV remux — output opens with avformat, streams
//         intact, duration plausible, output is seekable.
TEST_F(MkvRemuxerTest, SuccessfulMkvRemux) {
    ASSERT_FALSE(BuildTestMkv(src_mkv_path_).empty()) << "Failed to build source MKV fixture";

    const auto result = RemuxToMkv(src_mkv_path_, out_mkv_path_);
    ASSERT_TRUE(result.success) << "RemuxToMkv failed: " << result.message << " (av_err=" << result.av_error_code
                                << ")";

    ASSERT_GT(std::filesystem::file_size(out_mkv_path_), 0u);

    AVFormatContext* ctx = nullptr;
    int ret = avformat_open_input(&ctx, out_mkv_path_.c_str(), nullptr, nullptr);
    ASSERT_EQ(ret, 0) << "avformat_open_input on MKV output failed: " << av_err2str_cpp_test(ret);
    ASSERT_NE(ctx, nullptr);

    ret = avformat_find_stream_info(ctx, nullptr);
    EXPECT_GE(ret, 0) << "avformat_find_stream_info failed on MKV output";

    // Duration should be plausible (we fed 3 seconds).
    if (ctx->duration != AV_NOPTS_VALUE && ctx->duration > 0) {
        const double dur = static_cast<double>(ctx->duration) / AV_TIME_BASE;
        EXPECT_GE(dur, 1.0) << "MKV output duration unexpectedly short";
        EXPECT_LE(dur, 10.0) << "MKV output duration unexpectedly long";
    }

    // Must have at least one stream.
    EXPECT_GE(ctx->nb_streams, 1u);

    // Read a few packets.
    AVPacket* pkt = av_packet_alloc();
    ASSERT_NE(pkt, nullptr);
    int packet_count = 0;
    while (av_read_frame(ctx, pkt) == 0) {
        ++packet_count;
        av_packet_unref(pkt);
        if (packet_count >= 10)
            break;
    }
    av_packet_free(&pkt);
    avformat_close_input(&ctx);

    EXPECT_GE(packet_count, 1) << "Could not read any packets from the MKV output";
}

// Test 7: progress callback fires with monotone values for MKV output.
TEST_F(MkvRemuxerTest, MkvProgressCallbackFiresMonotone) {
    ASSERT_FALSE(BuildTestMkv(src_mkv_path_).empty());

    std::vector<float> progress_values;
    auto cb = [&progress_values](float p) -> bool {
        progress_values.push_back(p);
        return true;
    };

    const auto result = RemuxToMkv(src_mkv_path_, out_mkv_path_, cb);
    ASSERT_TRUE(result.success) << result.message;

    EXPECT_GE(progress_values.size(), 1u);
    for (float v : progress_values) {
        EXPECT_GE(v, 0.0f);
        EXPECT_LE(v, 1.0f);
    }
    for (size_t i = 1; i < progress_values.size(); ++i) {
        EXPECT_GE(progress_values[i], progress_values[i - 1]);
    }
    EXPECT_FLOAT_EQ(progress_values.back(), 1.0f);
}

// Test 8: cancel mid-way aborts MKV remux and removes partial output.
TEST_F(MkvRemuxerTest, MkvCancelAbortsCleansUp) {
    ASSERT_FALSE(BuildTestMkv(src_mkv_path_, 5.0).empty());

    std::atomic<int> call_count{0};
    auto cb = [&call_count](float) -> bool {
        ++call_count;
        return call_count.load() < 2;
    };

    const auto result = RemuxToMkv(src_mkv_path_, out_mkv_path_, cb);

    EXPECT_FALSE(result.success) << "Expected failure on cancel";
    EXPECT_FALSE(std::filesystem::exists(out_mkv_path_)) << "Partial output file was not cleaned up after cancel";
}

// Test 9: bad input path returns structured error for MKV output.
TEST_F(MkvRemuxerTest, MkvBadInputReturnsStructuredError) {
    const auto result = RemuxToMkv("/nonexistent_path_xyz_exosnap_mkv_test.mkv", out_mkv_path_);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.av_error_code, 0);
    EXPECT_FALSE(result.message.empty());
}

} // namespace
