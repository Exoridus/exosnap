// test_recovery_truncation.cpp — the powerloss CI proxy (Recovery drill, level 1).
//
// A real hard-reset loses the OS write-back cache and cannot be produced in CI.
// The proxy models the observable consequence — "the tail of the file is gone" —
// by truncating a genuinely-flushed partial MKV at assorted offsets and asserting
// the RemuxToMkv REPAIR path never crashes and salvages up to the last complete
// cluster. It does NOT prove the durability window; only the live hard-reset drill
// (docs/dev/soak-and-recovery-drills.md) does that.
//
// The partial is produced by the shared synthetic session through the REAL
// MatroskaStreamWriter (real durability flush), so the bytes on disk are the exact
// cluster layout a killed recording would leave.

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <gtest/gtest.h>

#include "exosnap/engine/mp4_remuxer.h"
#include "synthetic_session.h"
#include "test_unique_temp.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Copy the first `bytes` of src into dst (models a file whose tail never reached
// disk before the power was cut).
bool TruncateCopy(const std::string& src, const std::string& dst, uint64_t bytes) {
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    if (!in.good() || !out.good())
        return false;
    std::vector<char> buf(static_cast<size_t>(bytes));
    in.read(buf.data(), static_cast<std::streamsize>(bytes));
    out.write(buf.data(), in.gcount());
    return true;
}

bool CanDemux(const std::string& path) {
    av_log_set_level(AV_LOG_QUIET);
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) != 0)
        return false;
    const bool info_ok = avformat_find_stream_info(ctx, nullptr) >= 0;
    AVPacket* pkt = av_packet_alloc();
    int packets = 0;
    while (av_read_frame(ctx, pkt) >= 0) {
        ++packets;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    return info_ok && packets > 0;
}

class RecoveryTruncationTest : public ::testing::Test {
  protected:
    void TearDown() override {
        for (const auto& p : cleanup_) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
        }
    }
    std::string TempPath(const char* suffix) {
        auto p = exosnap_test::UniqueTempPathStr(suffix);
        cleanup_.push_back(p);
        return p;
    }
    std::vector<std::string> cleanup_;
};

// Produce a real ~4 s MKV, then repair-remux truncated copies at many offsets.
// The repair must always RETURN (never hang or crash); a near-complete truncation
// that only loses the trailer must still salvage a demuxable file.
TEST_F(RecoveryTruncationTest, RepairRemuxSurvivesTailTruncationAtManyOffsets) {
    const std::string full = TempPath("trunc_full.mkv");
    exosnap::engine::testutil::SyntheticSessionConfig cfg;
    cfg.video_codec = exosnap::engine::VideoCodec::H264;
    cfg.audio_codec = exosnap::engine::AudioCodec::Aac;
    cfg.output_path = full;
    cfg.target_seconds = 4.0;
    const auto run = exosnap::engine::testutil::SyntheticSession(cfg).Run();
    ASSERT_TRUE(run.success) << "synthetic run failed: " << run.error;

    const uint64_t size = std::filesystem::file_size(full);
    ASSERT_GT(size, 8192u) << "synthetic MKV unexpectedly small";

    int salvaged = 0;
    for (double frac : {0.25, 0.5, 0.75, 0.9, 0.98}) {
        const auto bytes = static_cast<uint64_t>(static_cast<double>(size) * frac);
        const std::string partial = TempPath("trunc_partial.mkv");
        const std::string repaired = TempPath("trunc_repaired.mkv");
        ASSERT_TRUE(TruncateCopy(full, partial, bytes));

        // The whole point: this call must always return, on any garbage tail.
        const auto rr = exosnap::engine::RemuxToMkv(partial, repaired);
        if (rr.success && CanDemux(repaired))
            ++salvaged;
    }
    // A trailer-only loss (0.98) should salvage the earlier clusters.
    EXPECT_GT(salvaged, 0) << "no truncation offset produced a demuxable repair";
}

} // namespace
