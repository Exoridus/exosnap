#pragma once

// synthetic_session — the deterministic, GPU-free session driver.
//
// This is the in-test "video feeder" from test_session_e2e_real_file.cpp lifted
// into a reusable seam. It drives the REAL AudioThread (real libopus/FFmpeg AAC),
// the REAL MuxThread + MatroskaStreamWriter + Finalize, and a deterministic
// in-process feeder that plays VideoThread's SessionState role — writing a real
// MKV with no GPU, no WGC, no NVENC. Shared by:
//   * test_session_e2e_real_file (the original owner; behaviour unchanged),
//   * exosnap-soak --synthetic (the CI-able soak twin), and
//   * the recovery drills (a realistic partial/finalized MKV to recover).
//
// It optionally drives the engine's SessionStatsCollector so the public
// Stats/Diagnostics callbacks fire — the soak twin needs a non-empty metric
// timeline, and only Record() instantiates that collector in production.

#include "synthetic_output_fault.h"
#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/pipeline_diagnostics.h>
#include <exosnap/engine/session_stats.h>

#include <cstdint>
#include <memory>
#include <string>

namespace exosnap::engine::testutil {

struct SyntheticSessionConfig {
    VideoCodec video_codec = VideoCodec::Av1;
    AudioCodec audio_codec = AudioCodec::Opus;
    std::string output_path; // the MKV the pipeline writes
    double target_seconds = 2.0;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t fps = 60;
    uint32_t gop = 30;

    // When true, pace audio and video to wall-clock (one packet/frame per interval)
    // so a short run produces a spread of stats samples. When false, feed as fast
    // as possible ("2 h of media in seconds") for harness-scale validation.
    bool realtime_pace = false;

    // When true, instantiate a SessionStatsCollector against the shared state so
    // the Stats/Diagnostics callbacks below actually fire (soak twin needs this).
    bool drive_stats_collector = false;

    // Test-tool-only injection into the real mux writer's output operations.
    std::optional<SyntheticOutputFaultSpec> output_fault;
};

struct SyntheticSessionResult {
    bool success = false; // clean finalize, file present, no session failure
    bool finalized = false;
    uint64_t output_bytes = 0;
    std::string error;
    bool output_fault_triggered = false;
    uint64_t output_fault_visits = 0;
};

class SyntheticSession {
  public:
    explicit SyntheticSession(SyntheticSessionConfig config);
    ~SyntheticSession();

    SyntheticSession(const SyntheticSession&) = delete;
    SyntheticSession& operator=(const SyntheticSession&) = delete;

    // Set before Run(). Used only when config.drive_stats_collector is true.
    void SetStatsCallback(StatsCallback cb);
    void SetDiagnosticsCallback(DiagnosticsCallback cb);

    // Runs to a clean finalize (or a recorded failure). Blocking.
    SyntheticSessionResult Run();

    // Cooperative stop from another thread — mirrors RecorderSession::Stop(): the
    // feeder stops producing, drains, and the pipeline finalizes what it has.
    void RequestStop();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace exosnap::engine::testutil
