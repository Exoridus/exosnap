// test_soak_synthetic.cpp — the soak run-loop end to end, no GPU.
//
// Drives the shared synthetic session through the exact SoakRunner wiring the real
// GPU path uses (a fake process sampler stands in for the WinAPI one), and proves:
//   * a synthetic soak writes a valid, demuxable MKV,
//   * the metric timeline is non-empty and well-formed JSON-Lines, and
//   * an injected skew ramp trips the wired abort policy.

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include "soak_process_sampler.h"
#include "soak_runner.h"
#include "synthetic_session.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using exosnap::soak::IProcessSampler;
using exosnap::soak::ProcessMetrics;
using exosnap::soak::SoakRunner;
using exosnap::soak::SoakThresholds;

namespace {

class FakeSampler : public IProcessSampler {
  public:
    ProcessMetrics metrics;
    ProcessMetrics Sample() override {
        return metrics;
    }
};

std::string UniqueTemp(const char* suffix) {
    const auto base = std::filesystem::temp_directory_path();
    const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    return (base / ("soak_" + std::to_string(ts) + "_" + suffix)).string();
}

// Compact foreign-player check: open, demux to a clean EOF, count streams.
struct DemuxFacts {
    bool open_ok = false;
    bool clean_eof = false;
    unsigned video_streams = 0;
    unsigned audio_streams = 0;
    int packets = 0;
};

DemuxFacts DemuxAndInspect(const std::string& path) {
    DemuxFacts f;
    av_log_set_level(AV_LOG_QUIET);
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) != 0)
        return f;
    f.open_ok = true;
    avformat_find_stream_info(ctx, nullptr);
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        const auto t = ctx->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO)
            ++f.video_streams;
        else if (t == AVMEDIA_TYPE_AUDIO)
            ++f.audio_streams;
    }
    AVPacket* pkt = av_packet_alloc();
    int ret = 0;
    while ((ret = av_read_frame(ctx, pkt)) >= 0) {
        ++f.packets;
        av_packet_unref(pkt);
    }
    f.clean_eof = (ret == AVERROR_EOF);
    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    return f;
}

} // namespace

TEST(SoakSynthetic, ProducesValidFileAndNonEmptyTimeline) {
    const std::string mkv = UniqueTemp("harness.mkv");
    const std::string jsonl = UniqueTemp("harness.jsonl");

    FakeSampler sampler;
    sampler.metrics = {400ULL * 1024 * 1024, 420ULL * 1024 * 1024, 500, 30, 20};

    SoakRunner runner(SoakThresholds{}, sampler, jsonl);

    exosnap::engine::testutil::SyntheticSessionConfig cfg;
    cfg.output_path = mkv;
    cfg.target_seconds = 2.5;
    cfg.realtime_pace = true;
    cfg.drive_stats_collector = true;
    exosnap::engine::testutil::SyntheticSession session(cfg);
    session.SetStatsCallback([&](const exosnap::engine::SessionStats& s) { runner.OnStats(s); });
    session.SetDiagnosticsCallback(
        [&](const exosnap::engine::RecordingDiagnosticsSnapshot& d) { runner.OnDiagnostics(d); });

    runner.Start(0.2);
    const auto result = session.Run();
    runner.Stop();

    ASSERT_TRUE(result.success) << result.error;

    const auto d = DemuxAndInspect(mkv);
    EXPECT_TRUE(d.open_ok);
    EXPECT_TRUE(d.clean_eof);
    EXPECT_EQ(d.video_streams, 1u);
    EXPECT_EQ(d.audio_streams, 1u);
    EXPECT_GT(d.packets, 0);

    const auto tl = runner.timeline();
    EXPECT_GE(tl.size(), 2u) << "timeline should hold multiple samples over a 2.5 s run";

    // The JSONL sidecar exists, is non-empty, and carries the metric columns.
    std::ifstream in(jsonl, std::ios::binary);
    ASSERT_TRUE(in.good());
    const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_FALSE(body.empty());
    EXPECT_NE(body.find("\"duration_skew_ms\""), std::string::npos);
    EXPECT_NE(body.find("\"rss_bytes\""), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(mkv, ec);
    std::filesystem::remove(jsonl, ec);
}

TEST(SoakSynthetic, InjectedSkewRampTripsWiredAbortPolicy) {
    FakeSampler sampler;
    sampler.metrics = {400ULL * 1024 * 1024, 420ULL * 1024 * 1024, 500, 30, 20};

    SoakThresholds t;
    t.sustained_samples = 5;
    t.duration_skew_abort_ms = 20.0;
    SoakRunner runner(t, sampler, /*jsonl_path=*/"");
    runner.SetSkewInjection(40.0); // +40 ms/s → over the 20 ms budget within ~1 s, growing

    runner.Start(0.05);
    // Let the ramp climb well past budget across many samples.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    runner.Stop();

    EXPECT_TRUE(runner.aborted());
    EXPECT_NE(runner.abort_decision().reason.find("skew"), std::string::npos);
}
