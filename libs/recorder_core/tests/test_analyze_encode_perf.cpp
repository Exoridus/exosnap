#include <gtest/gtest.h>

#include "perf_histogram.h"

#include <recorder_core/logging/logging.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

// End-to-end check of scripts/dev/analyze-encode-perf.py: write a synthetic
// engine.jsonl fixture through the real logging layer (so the on-disk JSON shape
// matches production), run the script's --json mode, and assert the percentiles it
// recomputes from the summary histogram match what the C++ LatencyHistogram would
// report. Skips gracefully when no Python interpreter is available on the host.

namespace {

namespace logging = recorder_core::logging;
using recorder_core::LatencyHistogram;

#ifndef EXOSNAP_SOURCE_DIR
#define EXOSNAP_SOURCE_DIR "."
#endif

std::string BucketsCsv(const std::array<uint64_t, LatencyHistogram::kBucketCount>& b) {
    std::string out;
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out += std::to_string(b[i]);
    }
    return out;
}

std::string Num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return std::string(buf);
}

// Run a command line, capturing stdout. Returns false if the process could not
// be launched at all.
bool RunCapture(const std::string& cmd, std::string& out) {
    out.clear();
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        return false;
    }
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        out += buf;
    }
    const int rc = _pclose(pipe);
    return rc == 0;
}

// Find a working Python launcher on the host, or empty string.
std::string FindPython() {
    for (const char* cand : {"python", "py -3", "python3"}) {
        std::string out;
        if (RunCapture(std::string(cand) + " --version", out)) {
            return cand;
        }
    }
    return "";
}

TEST(AnalyzeEncodePerf, SummaryHistogramRoundTripsThroughScript) {
    const std::string python = FindPython();
    if (python.empty()) {
        GTEST_SKIP() << "no Python interpreter on host";
    }

    // Build a known encode-latency distribution and mirror tick times.
    LatencyHistogram encode;
    LatencyHistogram tick;
    for (int i = 0; i < 200; ++i) {
        encode.Add(4.0);
    }
    for (int i = 0; i < 100; ++i) {
        encode.Add(40.0); // a fatter tail
    }
    for (int i = 0; i < 300; ++i) {
        tick.Add(12.0);
    }
    const double expect_enc_p50 = encode.Quantile(0.50);
    const double expect_enc_p99 = encode.Quantile(0.99);

    const auto tmp = std::filesystem::temp_directory_path() / "exosnap_perf_fixture.jsonl";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    logging::LoggerConfig cfg;
    cfg.filePath = tmp;
    cfg.minimumLevel = logging::LogLevel::Info;
    logging::initialize(cfg);

    // One window record + the closing summary, exactly as the collector emits.
    {
        const std::vector<logging::LogField> win = {
            {"perf_schema", "1"},
            {"encode_p50_ms", "4.0"},
            {"encode_p95_ms", "40.0"},
            {"encode_p99_ms", "40.0"},
            {"tick_p99_ms", "12.0"},
            {"tick_budget_ms", "16.667"},
            {"dropped_backpressure", "3"},
            {"slot_stalls", "1"},
            {"preset", "P4"},
            {"codec", "av1"},
            {"resolution", "1920x1080@60"},
        };
        logging::log(logging::LogLevel::Info, "perf", "video-pipeline-window",
                     std::span<const logging::LogField>(win.data(), win.size()));
    }
    {
        const std::vector<logging::LogField> sum = {
            {"perf_schema", "1"},
            {"hist_lo_ms", Num(LatencyHistogram::kLoMs)},
            {"hist_hi_ms", Num(LatencyHistogram::kHiMs)},
            {"hist_buckets", std::to_string(LatencyHistogram::kBucketCount)},
            {"encode_count", std::to_string(encode.count())},
            {"encode_hist", BucketsCsv(encode.BucketCounts())},
            {"tick_count", std::to_string(tick.count())},
            {"tick_hist", BucketsCsv(tick.BucketCounts())},
            {"dropped_backpressure", "3"},
            {"slot_stalls", "1"},
            {"preset", "P4"},
            {"codec", "av1"},
            {"resolution", "1920x1080@60"},
        };
        logging::log(logging::LogLevel::Info, "perf", "session-perf-summary",
                     std::span<const logging::LogField>(sum.data(), sum.size()));
    }
    logging::shutdown();

    ASSERT_TRUE(std::filesystem::exists(tmp));

    const std::string script = std::string(EXOSNAP_SOURCE_DIR) + "/scripts/dev/analyze-encode-perf.py";
    ASSERT_TRUE(std::filesystem::exists(script)) << script;

    std::string out;
    const std::string cmd = python + " \"" + script + "\" \"" + tmp.string() + "\" --json";
    ASSERT_TRUE(RunCapture(cmd, out)) << "script failed; output:\n" << out;

    // Coarse assertions on the JSON output (no JSON lib needed in the test): the
    // script must have found one session with a summary and matching percentiles.
    EXPECT_NE(out.find("\"sessions\""), std::string::npos) << out;
    EXPECT_NE(out.find("\"has_summary\": true"), std::string::npos) << out;
    EXPECT_NE(out.find("\"codec\": \"av1\""), std::string::npos) << out;
    EXPECT_NE(out.find("\"preset\": \"P4\""), std::string::npos) << out;

    // Extract encode_p50_ms / encode_p99_ms and compare to the C++ histogram — the
    // script recomputes them from the same buckets, so they must agree exactly
    // (both use identical geometric-bucket interpolation).
    auto extract = [&out](const std::string& key) -> double {
        const auto pos = out.find("\"" + key + "\":");
        if (pos == std::string::npos) {
            return -1.0;
        }
        return std::atof(out.c_str() + pos + key.size() + 3);
    };
    EXPECT_NEAR(extract("encode_p50_ms"), expect_enc_p50, 1e-3);
    EXPECT_NEAR(extract("encode_p99_ms"), expect_enc_p99, 1e-3);

    std::filesystem::remove(tmp, ec);
}

} // namespace
