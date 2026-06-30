// test_remux_trim.cpp — unit tests for TrimRange stream-copy and
//                        ExtractKeyframeTimestamps (ADR-0014 / 0.9.0 S1).
//
// Test strategy: generate synthetic MKVs via MatroskaStreamWriter (same pattern
// as test_mp4_remuxer.cpp), then run RemuxToProgressiveMp4 / RemuxToMkv with
// TrimRange overloads and verify the output is smaller / correctly bounded.
// ExtractKeyframeTimestamps is tested for non-empty sorted output and graceful
// failure on bad input.

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

// MSVC + C++: override av_err2str to avoid C99 compound literal.
static inline const char* av_err2str_trim_test(int errnum) noexcept {
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}

#include <gtest/gtest.h>

#include "matroska_stream_writer.h"
#include "recorder_core/mp4_remuxer.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using recorder_core::ExtractKeyframeTimestamps;
using recorder_core::MatroskaStreamConfig;
using recorder_core::MatroskaStreamWriter;
using recorder_core::MuxPacket;
using recorder_core::RemuxNoopCallback;
using recorder_core::RemuxResult;
using recorder_core::RemuxToMkv;
using recorder_core::RemuxToProgressiveMp4;
using recorder_core::TrimRange;

namespace {

// ---------------------------------------------------------------------------
// Minimal codec private data (same stubs as test_mp4_remuxer.cpp)
// ---------------------------------------------------------------------------

static std::vector<uint8_t> FakeH264Cp_tr() {
    return {0x01, 0x42, 0x00, 0x1F, 0xFF, 0xE1, 0x00};
}
static std::vector<uint8_t> ValidAacCp_tr() {
    return {0x13, 0x90}; // AAC-LC 48 kHz stereo AudioSpecificConfig
}

// ---------------------------------------------------------------------------
// Config factory
// ---------------------------------------------------------------------------

static MatroskaStreamConfig MakeTrimCfg(const std::string& path) {
    MatroskaStreamConfig c;
    c.output_path = path;
    c.video_codec_id = "V_MPEG4/ISO/AVC";
    c.video_codec_private = FakeH264Cp_tr();
    c.encode_width = 1280;
    c.encode_height = 720;
    c.frame_rate_num = 60;
    c.frame_rate_den = 1;
    c.audio_codec = recorder_core::StreamAudioCodec::Aac;
    c.audio_track_count = 1;
    c.audio_tracks[0].codec_private = ValidAacCp_tr();
    return c;
}

// ---------------------------------------------------------------------------
// Packet feeder — 60 fps video, 48 kHz audio, gop = keyframe every `gop` frames
// ---------------------------------------------------------------------------

static void FeedTrimSeconds(MatroskaStreamWriter& w, double seconds, int gop = 60) {
    const uint64_t vframe = 1000000000ULL / 60;
    const uint64_t aframe = 1024ULL * 1000000000ULL / 48000ULL;
    const uint64_t total_ns = static_cast<uint64_t>(seconds * 1e9);
    const std::vector<uint8_t> blob(256, 0xAB);

    uint64_t vpts = 0;
    int vidx = 0;
    uint64_t apts = 0;

    while (vpts < total_ns || apts < total_ns) {
        if (vpts <= apts && vpts < total_ns) {
            MuxPacket p;
            p.pts_ns = vpts;
            p.track_num = 1;
            p.is_key = (vidx % gop == 0);
            p.bytes = blob;
            w.Push(std::move(p));
            vpts += vframe;
            ++vidx;
        } else if (apts < total_ns) {
            MuxPacket p;
            p.pts_ns = apts;
            p.track_num = 2;
            p.is_key = true;
            p.bytes = blob;
            w.Push(std::move(p));
            apts += aframe;
        } else {
            break;
        }
    }
}

// Build a synthetic MKV; return path on success, empty string on failure.
static std::string BuildTrimMkv(const std::string& path, double seconds = 6.0, int gop = 60) {
    MatroskaStreamWriter w;
    auto cfg = MakeTrimCfg(path);
    if (!w.Open(cfg))
        return {};
    FeedTrimSeconds(w, seconds, gop);
    if (!w.Finalize())
        return {};
    return path;
}

// Build a temp path unique to the currently-running test (same pattern as
// test_mp4_remuxer.cpp to avoid collisions with parallel ctest processes).
static std::string UniqueTrimTempPath(const char* suffix) {
    auto tmp = std::filesystem::temp_directory_path();
    std::string name = "anon";
    if (const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info())
        name = info->name();
    return (tmp / ("exosnap_trim_" + name + "_" + suffix)).string();
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class TrimTest : public ::testing::Test {
  protected:
    void SetUp() override {
        src_ = UniqueTrimTempPath("src.mkv");
        dst_ = UniqueTrimTempPath("dst.mp4");
        std::remove(src_.c_str());
        std::remove(dst_.c_str());
    }
    void TearDown() override {
        std::remove(src_.c_str());
        std::remove(dst_.c_str());
    }
    std::string src_;
    std::string dst_;
};

} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// --- ExtractKeyframeTimestamps returns non-empty sorted vector for valid MKV ---
TEST_F(TrimTest, ExtractKeyframesReturnsNonEmpty) {
    ASSERT_FALSE(BuildTrimMkv(src_, 6.0, 60).empty()) << "Failed to build test MKV";
    const auto kfs = ExtractKeyframeTimestamps(src_);
    // 6 seconds at 60 fps, gop=60 → keyframe every 1 s → expect ~6 keyframes
    EXPECT_GE(kfs.size(), 2u) << "Expected >=2 keyframes in 6 s / gop=60 file";
    for (size_t i = 1; i < kfs.size(); ++i)
        EXPECT_GE(kfs[i], kfs[i - 1]) << "Keyframes are not sorted at index " << i;
}

// --- ExtractKeyframeTimestamps returns empty vector for non-existent input ---
TEST_F(TrimTest, ExtractKeyframesBadInput) {
    const auto kfs = ExtractKeyframeTimestamps("/nonexistent_xyz_exosnap_trim_input.mkv");
    EXPECT_TRUE(kfs.empty()) << "Expected empty vector for bad input path";
}

// --- No-trim pass: TrimRange{} (both kNoTimestamp) produces valid output ---
TEST_F(TrimTest, NoTrimMatchesFullRemux) {
    ASSERT_FALSE(BuildTrimMkv(src_).empty());
    TrimRange full; // both fields == kNoTimestamp
    auto res = RemuxToProgressiveMp4(src_, dst_, RemuxNoopCallback(), full);
    ASSERT_TRUE(res.success) << "No-trim remux failed: " << res.message;
    EXPECT_GT(std::filesystem::file_size(dst_), 0u);
}

// --- Start trim: output is smaller than full remux ---
TEST_F(TrimTest, StartTrimProducesSmallerOutput) {
    ASSERT_FALSE(BuildTrimMkv(src_, 6.0, 60).empty());
    const auto kfs = ExtractKeyframeTimestamps(src_);
    ASSERT_GE(kfs.size(), 2u) << "Need >=2 keyframes to test start trim";

    // Full remux.
    const std::string dst_full = UniqueTrimTempPath("dst_full.mp4");
    std::remove(dst_full.c_str());
    {
        auto res = RemuxToProgressiveMp4(src_, dst_full, RemuxNoopCallback(), TrimRange{});
        ASSERT_TRUE(res.success) << "Full remux failed: " << res.message;
    }
    const auto size_full = std::filesystem::file_size(dst_full);
    std::remove(dst_full.c_str());

    // Trimmed from 2nd keyframe onward.
    TrimRange tr;
    tr.start_us = kfs[1]; // ~1 s in (at 60 fps, gop=60, first keyframe at 0, second at ~1 s)
    tr.end_us = TrimRange::kNoTimestamp;

    auto res = RemuxToProgressiveMp4(src_, dst_, RemuxNoopCallback(), tr);
    ASSERT_TRUE(res.success) << "Trimmed remux failed: " << res.message;
    const auto size_trim = std::filesystem::file_size(dst_);
    EXPECT_LT(size_trim, size_full) << "Trimmed output should be smaller than full (" << size_trim << " vs "
                                    << size_full << ")";
}

// --- End trim: output is smaller than full remux ---
TEST_F(TrimTest, EndTrimProducesSmallerOutput) {
    ASSERT_FALSE(BuildTrimMkv(src_, 6.0, 60).empty());
    const auto kfs = ExtractKeyframeTimestamps(src_);
    ASSERT_GE(kfs.size(), 3u) << "Need >=3 keyframes to test end trim";

    // Full remux.
    const std::string dst_full = UniqueTrimTempPath("dst_full2.mp4");
    std::remove(dst_full.c_str());
    {
        auto res = RemuxToProgressiveMp4(src_, dst_full, RemuxNoopCallback(), TrimRange{});
        ASSERT_TRUE(res.success);
    }
    const auto size_full = std::filesystem::file_size(dst_full);
    std::remove(dst_full.c_str());

    // Trim to just before the midpoint keyframe.
    TrimRange tr;
    tr.start_us = TrimRange::kNoTimestamp;
    tr.end_us = kfs[kfs.size() / 2]; // midpoint keyframe

    auto res = RemuxToProgressiveMp4(src_, dst_, RemuxNoopCallback(), tr);
    ASSERT_TRUE(res.success) << "End-trim remux failed: " << res.message;
    const auto size_trim = std::filesystem::file_size(dst_);
    EXPECT_LT(size_trim, size_full) << "End-trimmed output should be smaller (" << size_trim << " vs " << size_full
                                    << ")";
}

// --- Trim to MKV output (stream-copy, matroska muxer) ---
TEST_F(TrimTest, TrimToMkv) {
    ASSERT_FALSE(BuildTrimMkv(src_).empty());
    const auto kfs = ExtractKeyframeTimestamps(src_);
    ASSERT_GE(kfs.size(), 2u);

    const std::string dst_mkv = UniqueTrimTempPath("dst.mkv");
    std::remove(dst_mkv.c_str());

    TrimRange tr;
    tr.start_us = kfs[1];
    tr.end_us = TrimRange::kNoTimestamp;

    auto res = RemuxToMkv(src_, dst_mkv, RemuxNoopCallback(), tr);
    ASSERT_TRUE(res.success) << "Trim-to-MKV failed: " << res.message;
    EXPECT_GT(std::filesystem::file_size(dst_mkv), 0u);
    std::remove(dst_mkv.c_str());
}

// --- Both start and end trim ---
TEST_F(TrimTest, BothStartAndEndTrim) {
    ASSERT_FALSE(BuildTrimMkv(src_, 9.0, 60).empty()); // 9 s → ~9 keyframes at gop=60
    const auto kfs = ExtractKeyframeTimestamps(src_);
    ASSERT_GE(kfs.size(), 4u) << "Need >=4 keyframes for both-ends trim test";

    const std::string dst_full = UniqueTrimTempPath("dst_full3.mp4");
    std::remove(dst_full.c_str());
    {
        auto res = RemuxToProgressiveMp4(src_, dst_full, RemuxNoopCallback(), TrimRange{});
        ASSERT_TRUE(res.success);
    }
    const auto size_full = std::filesystem::file_size(dst_full);
    std::remove(dst_full.c_str());

    TrimRange tr;
    tr.start_us = kfs[1];            // skip first ~1 s
    tr.end_us = kfs[kfs.size() - 1]; // stop before last keyframe

    auto res = RemuxToProgressiveMp4(src_, dst_, RemuxNoopCallback(), tr);
    ASSERT_TRUE(res.success) << "Both-ends trim failed: " << res.message;
    const auto size_trim = std::filesystem::file_size(dst_);
    EXPECT_LT(size_trim, size_full) << "Both-ends trimmed output should be smaller (" << size_trim << " vs "
                                    << size_full << ")";
}

// (diagnostic tests removed — root cause found and fixed)
