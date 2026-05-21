#pragma once

#include <recorder_core/audio_track_model.h>

#include "codec_types.h"
#include "error_types.h"
#include "session_stats.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace recorder_core {

// ---------------------------------------------------------------------------
// CaptureTarget
// ---------------------------------------------------------------------------

struct CaptureTarget {
    enum class Kind { Monitor, Window };

    Kind kind = Kind::Monitor;
    // Platform-native handle stored as an opaque integer.
    // On Windows: HMONITOR or HWND cast via reinterpret_cast<uintptr_t>.
    uintptr_t native_id = 0;
    std::string description;
};

// Microphone channel mapping policy for MIC capture in M4 Phase 4.2.
enum class MicChannelMode {
    Auto,
    PreserveStereo,
    MonoMix,
    LeftToStereo,
    RightToStereo,
};

// ---------------------------------------------------------------------------
// RecorderConfig
// ---------------------------------------------------------------------------

struct RecorderConfig {
    // Output file path
    std::filesystem::path output_path;

    // Capture source
    CaptureTarget target;

    // Format — M3.1 only supports the primary path below.
    // Validate() rejects any other combination.
    Container container = Container::Matroska;
    VideoCodec video_codec = VideoCodec::Av1Nvenc;
    AudioCodec audio_codec = AudioCodec::AacMf;
    ChromaSubsampling chroma = ChromaSubsampling::Cs420;
    BitDepth bit_depth = BitDepth::Bit8;

    // Frame rate (numerator/denominator)
    uint32_t frame_rate_num = 60;
    uint32_t frame_rate_den = 1;

    // Resolved output audio tracks from the APP/MIC/SYS source model.
    // Phase 2 legacy compatibility: empty plan means single audio track.
    AudioTrackPlan audio_track_plan;

    // MIC input channel mapping policy (applies to explicit MIC capture plan).
    MicChannelMode mic_channel_mode = MicChannelMode::Auto;
};

// ---------------------------------------------------------------------------
// RecorderResult
// ---------------------------------------------------------------------------

struct RecorderResult {
    bool succeeded = false;
    // Platform error code stored as a signed 32-bit integer.
    // On Windows: HRESULT. S_OK == 0. Negative values indicate failure.
    int32_t error_code = 0;
    ErrorPhase error_phase = ErrorPhase::None;
    SessionStats stats;
    std::string error_detail;
};

// ---------------------------------------------------------------------------
// RecorderSession
// ---------------------------------------------------------------------------

class RecorderSession {
  public:
    RecorderSession();
    ~RecorderSession();

    RecorderSession(const RecorderSession&) = delete;
    RecorderSession& operator=(const RecorderSession&) = delete;

    // Enumerate available capture targets (monitors and top-level windows).
    static std::vector<CaptureTarget> EnumerateTargets();

    // Validate a config before recording. Returns false and populates out_result
    // when the config is rejected (out_result may be null).
    bool Validate(const RecorderConfig& config, RecorderResult* out_result);

    // Start recording.  Blocks until Stop() is called or a fatal error occurs.
    // Returns a fully populated RecorderResult.
    RecorderResult Record(const RecorderConfig& config);

    // Thread-safe cooperative stop.  Safe to call from any thread while
    // Record() is running.  No-op if not recording.
    void Stop();

    // Register a stats callback invoked approximately every 250 ms from an
    // internal worker thread.  Must be set before calling Record().
    void SetStatsCallback(StatsCallback cb);

  private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace recorder_core
