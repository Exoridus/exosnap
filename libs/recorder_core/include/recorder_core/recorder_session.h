#pragma once

#include <recorder_core/audio_track_model.h>

#include "codec_types.h"
#include "error_types.h"
#include "session_stats.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace recorder_core {

// ---------------------------------------------------------------------------
// WebcamFrameProvider — injected into RecorderConfig for compositing
// ---------------------------------------------------------------------------

// Called by VideoThread to pull the latest webcam BGRA frame.
// Implementations must be thread-safe: TryGetFrame is called from VideoThread.
// The provider must remain alive for the duration of Record().
struct WebcamFrameProvider {
    // Returns true and fills out_width/out_height/out_bgra when a new (or latest)
    // frame is available.  out_bgra is BGRA (B8G8R8A8 byte order), row-major.
    // Returns false when no frame has been captured yet or webcam failed.
    virtual bool TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra) = 0;
    virtual ~WebcamFrameProvider() = default;
};

// ---------------------------------------------------------------------------
// WebcamConfig
// ---------------------------------------------------------------------------

struct WebcamConfig {
    bool enabled = false;

    // Not owned — must outlive the recording session.  nullptr = disabled.
    WebcamFrameProvider* frame_provider = nullptr;

    // Overlay placement as fraction [0,1] of encode frame dimensions.
    float overlay_x_norm = 0.0f;
    float overlay_y_norm = 0.0f;
    float overlay_w_norm = 0.25f;
    float overlay_h_norm = 0.25f;

    // Chroma key
    bool chroma_key_enabled = false;
    uint8_t chroma_r = 0;
    uint8_t chroma_g = 177;
    uint8_t chroma_b = 64;
    float chroma_tolerance = 0.30f;
    float chroma_softness = 0.05f;
};

} // namespace recorder_core

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

// ---------------------------------------------------------------------------
// CaptureRegion
// ---------------------------------------------------------------------------

// Axis-aligned rectangle in virtual screen coordinates (same as RECT / GetMonitorInfo.rcMonitor).
// When set in RecorderConfig.crop_region, the engine crops the monitor capture to this rectangle.
// Target must be CaptureTarget::Kind::Monitor.
struct CaptureRegion {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;

    // Minimum dimension accepted for region capture.
    // 64 px provides safe headroom above NVENC hard minimums.
    static constexpr int32_t kMinDimension = 64;

    [[nodiscard]] bool IsValid() const noexcept {
        return width >= kMinDimension && height >= kMinDimension;
    }
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

    // Format — WebM (AV1+Opus) and Matroska (AV1+AAC or AV1+Opus) are supported.
    // Validate() rejects unsupported combinations.
    Container container = Container::WebM;
    VideoCodec video_codec = VideoCodec::Av1Nvenc;
    AudioCodec audio_codec = AudioCodec::Opus;
    ChromaSubsampling chroma = ChromaSubsampling::Cs420;
    BitDepth bit_depth = BitDepth::Bit8;

    // NVENC quality tier — maps to CQP values in the encoder.
    NvencQualityPreset nvenc_quality_preset = NvencQualityPreset::Balanced;

    // Frame rate (numerator/denominator)
    uint32_t frame_rate_num = 60;
    uint32_t frame_rate_den = 1;

    // When true: CFR scheduler (duplicate/drop frames to hit constant rate).
    // When false: VFR passthrough (WGC timestamps used directly as PTS).
    bool cfr = true;

    // Resolved output audio tracks from the APP/MIC/SYS source model.
    // Phase 2 legacy compatibility: empty plan means single audio track.
    AudioTrackPlan audio_track_plan;

    // When false, no audio threads are started and audio_track_plan is ignored.
    // Default true preserves backward compatibility: empty plan -> WasapiLoopbackSrc.
    bool record_audio = true;

    // MIC input channel mapping policy (applies to explicit MIC capture plan).
    MicChannelMode mic_channel_mode = MicChannelMode::Auto;

    // Optional WASAPI capture endpoint ID for microphone capture.
    // nullopt preserves current behavior: use GetDefaultAudioEndpoint(eCapture, eConsole).
    std::optional<std::string> mic_device_id;

    // Optional target process id for process-loopback sources (App/Sys).
    // When empty, legacy loopback mode (empty audio_track_plan) remains valid.
    std::optional<uint32_t> audio_target_process_id;

    // Linear gain applied to microphone sources in mixed tracks.
    // For single-source MIC tracks, this gain is applied by wrapping the source in MixedAudioSrc when needed.
    // Default 1.0f (unity gain).
    float mic_gain_linear = 1.0f;

    // Whether the mouse cursor is composited into the captured frames.
    // Maps to GraphicsCaptureSession.IsCursorCaptureEnabled. Default true = WGC default.
    bool capture_cursor = true;

    // Optional region crop applied to Monitor captures.
    // When set, the engine crops the captured monitor frame to this rectangle
    // (coordinates in virtual screen space). Target.kind must be Kind::Monitor.
    std::optional<CaptureRegion> crop_region;

    // Optional webcam overlay composited into the recorded video.
    WebcamConfig webcam;
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

    // Thread-safe pause/resume.  Safe to call from any thread while Record()
    // is running.  Workers drain their source during pause so buffers do not stall.
    void Pause();
    void Resume();

    // Register a stats callback invoked approximately every 250 ms from an
    // internal worker thread.  Must be set before calling Record().
    void SetStatsCallback(StatsCallback cb);

  private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace recorder_core
