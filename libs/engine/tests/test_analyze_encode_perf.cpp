#include <cstdint>
#include <gtest/gtest.h>

#include "perf_histogram.h"

#include <exosnap/engine/logging/logging.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>

// End-to-end check of `exo-dev perf-analyze`: write a synthetic engine.jsonl
// fixture through the real logging layer (so the on-disk JSON shape matches
// production), run its --json mode, and assert the percentiles it recomputes
// from the summary histogram match what the C++ LatencyHistogram would
// report. Invokes the already-built exo-dev binary directly, never `cargo`,
// so this test cannot trigger a rebuild of a binary that may be running.

namespace {

namespace logging = exosnap::engine::logging;
using exosnap::engine::LatencyHistogram;

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

// The already-built exo-dev binary this test invokes directly: the same
// resolution order `exo-dev test` uses to hand a CTest child its own path
// (EXOSNAP_TEST_TOOL_EXE) when this test runs inside `cargo exo-dev test`,
// falling back to the conventional build locations for a bare `ctest` run.
// Empty when none of them resolves to an existing file.
std::string FindExoDevExe() {
    char* fromEnv = nullptr;
    std::size_t fromEnvLen = 0;
    if (_dupenv_s(&fromEnv, &fromEnvLen, "EXOSNAP_TEST_TOOL_EXE") == 0 && fromEnv != nullptr) {
        std::string value(fromEnv);
        std::free(fromEnv);
        if (!value.empty() && std::filesystem::exists(value)) {
            return value;
        }
    }
    const std::filesystem::path root = EXOSNAP_SOURCE_DIR;
    for (const char* config : {"debug", "release"}) {
        for (const char* target : {"tools/target/exo-dev-host", "tools/target"}) {
            std::filesystem::path candidate = root / target / config / "exo-dev.exe";
            if (std::filesystem::exists(candidate)) {
                return candidate.string();
            }
        }
    }
    return "";
}

TEST(AnalyzeEncodePerf, SummaryHistogramRoundTripsThroughExoDev) {
    const std::string exoDev = FindExoDevExe();
    ASSERT_FALSE(exoDev.empty()) << "no exo-dev executable found: set EXOSNAP_TEST_TOOL_EXE, or build "
                                    "tools/target/exo-dev-host/{debug,release}/exo-dev.exe or "
                                    "tools/target/{debug,release}/exo-dev.exe";

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

    std::string out;
    // cmd.exe's own quoting rule for a command line that starts with a quote
    // pairs the first quote with the LAST one in the whole string rather than
    // the next one, which breaks a command with more than one quoted
    // argument. Wrapping the whole line in an extra pair of quotes works
    // around that: cmd /c strips exactly one matching outer pair before
    // parsing the rest.
    const std::string cmd = "\"\"" + exoDev + "\" perf-analyze \"" + tmp.string() + "\" --json\"";
    ASSERT_TRUE(RunCapture(cmd, out)) << "exo-dev perf-analyze failed; output:\n" << out;

    // Coarse assertions on the JSON output (no JSON lib needed in the test): the
    // run must have found one session with a summary and matching percentiles.
    EXPECT_NE(out.find("\"sessions\""), std::string::npos) << out;
    EXPECT_NE(out.find("\"has_summary\": true"), std::string::npos) << out;
    EXPECT_NE(out.find("\"codec\": \"av1\""), std::string::npos) << out;
    EXPECT_NE(out.find("\"preset\": \"P4\""), std::string::npos) << out;

    // Extract encode_p50_ms / encode_p99_ms and compare to the C++ histogram.
    // Both recompute from the same buckets with identical geometric-bucket
    // interpolation, so they must agree exactly.
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
