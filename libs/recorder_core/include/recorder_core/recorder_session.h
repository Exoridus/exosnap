#pragma once

#include <recorder_core/audio_track_model.h>

#include "codec_types.h"
#include "error_types.h"
#include "output_geometry.h"
#include "pipeline_diagnostics.h"
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

    // Horizontal mirror (left/right flip) of the webcam image before compositing.
    // No vertical flip is performed.  Must match the Record-preview mirror state.
    bool mirror = false;

    // Chroma key. chroma_r/g/b hold the resolved active key color (caller
    // computes this from WebcamChromaKeySettings::active_color() before handing
    // config to the engine; the engine never needs to know the color mode).
    bool chroma_key_enabled = false;
    uint8_t chroma_r = 0;
    uint8_t chroma_g = 255;
    uint8_t chroma_b = 0;
    float chroma_tolerance = 0.40f;
    float chroma_softness = 0.15f;
    float chroma_spill_reduction = 0.30f;
};

// Live-mutable subset of WebcamConfig, updatable while Record() runs.
// Device/resolution/fps are not here: changing those requires a capture restart.
struct WebcamOverlayLive {
    bool enabled = false; // allows mid-recording show/hide of the PiP
    float overlay_x_norm = 0.0f;
    float overlay_y_norm = 0.0f;
    float overlay_w_norm = 0.25f;
    float overlay_h_norm = 0.25f;
    bool mirror = false;
    // chroma_r/g/b carry the resolved active key color (not the raw mode enum).
    bool chroma_key_enabled = false;
    uint8_t chroma_r = 0;
    uint8_t chroma_g = 255;
    uint8_t chroma_b = 0;
    float chroma_tolerance = 0.40f;
    float chroma_softness = 0.15f;
    float chroma_spill_reduction = 0.30f;
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
// Split recording (SPLIT-RECORDING-R1 / SPLIT-BY-SIZE-R1)
// ---------------------------------------------------------------------------

// Engine-level split configuration carried in RecorderConfig.
//
// Two independent thresholds (ADR 0021: dual time+size, whichever first):
//   duration_ms == 0  → time-based splitting disabled
//   size_bytes   == 0 → size-based splitting disabled
// Both may be active simultaneously. Manual splits are always available
// regardless of these settings.
//
// Note: RecordingSplitMode is kept for backward source-level compatibility but
// is no longer used internally. The coordinator resolves UI settings to numeric
// thresholds (duration_ms / size_bytes) before handing them to the engine.
enum class RecordingSplitMode {
    Off,      // legacy: equivalent to duration_ms = 0
    Duration, // legacy: equivalent to duration_ms > 0
};

// Engine-level split configuration carried in RecorderConfig.
struct RecordingSplitSettings {
    // Media-time interval per automatic segment (0 = disabled).
    std::uint64_t duration_ms = 0;
    // Committed-bytes threshold per segment (0 = disabled).
    std::uint64_t size_bytes = 0;

    bool operator==(const RecordingSplitSettings&) const = default;
};

// What triggered a split. Shared by the manual button and the global hotkey so
// they route through the exact same typed command path.
enum class SplitTriggerSource {
    AutomaticDuration,
    AutomaticSize,
    ManualButton,
    Hotkey,
};

// Metadata for one finalized media segment, emitted via SegmentCallback as each
// segment's container is closed (including the final segment at session end).
struct CompletedSegment {
    std::filesystem::path path;
    std::uint64_t session_start_ms = 0; // segment start on the continuous session timeline
    std::uint64_t duration_ms = 0;      // segment-local media duration
    std::uint64_t file_size_bytes = 0;
    std::uint32_t index = 0; // 0-based segment index
    bool succeeded = false;  // false => finalize failed / file quarantined
};

// Invoked from the mux worker thread as each segment is finalized. Must be
// thread-safe. Used by the app layer to build a multi-segment CompletedRecording.
using SegmentCallback = std::function<void(const CompletedSegment&)>;

// ---------------------------------------------------------------------------
// OpusFrameDuration — configurable Opus frame size (ADR 0019)
// ---------------------------------------------------------------------------

// Supported Opus frame durations. Maps to frame-size-in-samples at 48 kHz.
// 20 ms is the default; shorter durations reduce latency at the cost of
// higher CPU usage and slightly lower coding efficiency.
enum class OpusFrameDuration {
    Ms20 = 960,  // 20 ms — default, best coding efficiency
    Ms10 = 480,  // 10 ms — lower latency
    Ms5 = 240,   // 5 ms  — low latency / higher CPU
    Ms2_5 = 120, // 2.5 ms — very low latency / highest CPU (expert)
};

// Returns the frame size in samples for a given OpusFrameDuration.
// Equivalent to static_cast<int>(duration) but named for clarity.
inline constexpr int OpusFrameSizeSamples(OpusFrameDuration d) noexcept {
    return static_cast<int>(d);
}

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

    // NVENC quality tier — maps to CQP values in the encoder (used for ConstantQuality mode).
    NvencQualityPreset nvenc_quality_preset = NvencQualityPreset::Balanced;

    // Canonical rate-control mode (ADR 0009). Defaults to ConstantQuality (existing behavior).
    RateControlMode nvenc_rate_control = RateControlMode::ConstantQuality;

    // Target bitrate in kbps — used for VariableBitrate and ConstantBitrate modes.
    // Ignored (and zero-ed by the encoder) when mode is ConstantQuality.
    uint32_t nvenc_bitrate_kbps = 20000;

    // ---------------------------------------------------------------------------
    // Audio encoding parameters (ADR 0019)
    // ---------------------------------------------------------------------------

    // Target audio bitrate in kbps. 0 = use encoder default.
    // Opus: applied via OPUS_SET_BITRATE (VBR); range [32, 510] kbps; default 160 kbps.
    // AAC:  applied via MF_MT_AVG_BITRATE / AACENC_BITRATE; range [64, 320] kbps; default 192 kbps.
    uint32_t audio_bitrate_kbps = 0;

    // Opus frame duration. Controls the latency ↔ CPU tradeoff.
    // 20 ms is the default (best coding efficiency). AAC frame size is fixed at 1024
    // samples and is not configurable — this field is ignored when audio_codec != Opus.
    OpusFrameDuration opus_frame_duration = OpusFrameDuration::Ms20;

    // Opus encoder complexity 0–10 (10 = best quality / highest CPU load).
    // Default 10 per the roadmap. Ignored when audio_codec != Opus.
    int opus_complexity = 10;

    // Frame rate (numerator/denominator)
    uint32_t frame_rate_num = 60;
    uint32_t frame_rate_den = 1;

    // When true: CFR scheduler (duplicate/drop frames to hit constant rate).
    // When false: VFR passthrough (WGC timestamps used directly as PTS).
    bool cfr = true;

    // Requested encoded output size. 0x0 means Native: the selected source
    // dimensions are frozen at session start (after Region crop, when present).
    // Non-zero values are exact encoder dimensions and must be even.
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    OutputFitMode output_fit = OutputFitMode::Contain;

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

    // ---------------------------------------------------------------------------
    // Brickwall limiter (Audio v2 — 0.6.0)
    // ---------------------------------------------------------------------------

    // When true, audio that passes through MixedAudioSrc (merged tracks, or any
    // source with non-unity per-row gain) is peak-limited to
    // audio_limiter_ceiling_db instead of hard-clipped. These are exactly the
    // paths where per-track gain or summing can push the signal past full scale.
    // Default true: strictly better than the previous hard clip at the ceiling.
    bool audio_limiter_enabled = true;

    // Limiter ceiling in dBFS (<= 0). No output sample exceeds this level.
    // Default 0.0 dBFS keeps the previous clamp ceiling, so levels are unchanged
    // except that overs are smoothed (attack/release) instead of hard-clipped.
    float audio_limiter_ceiling_db = 0.0f;

    // ---------------------------------------------------------------------------
    // Microphone high-pass filter (Audio v2 — 0.6.0)
    // ---------------------------------------------------------------------------

    // When true, the microphone input is run through a 2nd-order Butterworth
    // high-pass filter (the first stage of the MicDspAudioSrc chain) to remove
    // low-frequency rumble (desk thumps, HVAC hum, plosives). Default false: mic
    // DSP alters captured audio, so it is opt-in.
    bool mic_hpf_enabled = false;

    // High-pass cutoff (−3 dB) frequency in Hz. Default 80 Hz.
    float mic_hpf_cutoff_hz = 80.0f;

    // ---------------------------------------------------------------------------
    // Microphone noise gate (Audio v2 — 0.6.0)
    // ---------------------------------------------------------------------------

    // When true, the microphone input is run through a downward noise gate (the
    // second stage of the MicDspAudioSrc chain, after the high-pass filter): below
    // the threshold the mic is attenuated toward silence (keyboard/fan/room noise
    // between speech), above it the mic passes through. Default false: mic DSP
    // alters captured audio, so it is opt-in.
    bool mic_gate_enabled = false;

    // Gate threshold in dBFS. Levels below this close the gate. Default -45 dB.
    float mic_gate_threshold_db = -45.0f;

    // Whether the mouse cursor is composited into the captured frames.
    // Maps to GraphicsCaptureSession.IsCursorCaptureEnabled. Default true = WGC default.
    bool capture_cursor = true;

    // Optional region crop applied to Monitor captures.
    // When set, the engine crops the captured monitor frame to this rectangle
    // (coordinates in virtual screen space). Target.kind must be Kind::Monitor.
    std::optional<CaptureRegion> crop_region;

    // Optional webcam overlay composited into the recorded video.
    WebcamConfig webcam;

    // Automatic/manual segment splitting. Default Off == single-file recording.
    RecordingSplitSettings split;
};

// Derive the on-disk path for segment `index` (0-based) from a base output path.
// Segment 0 keeps the base name; later segments insert a "_part-NNN" suffix
// before the extension (recording.mkv -> recording_part-002.mkv). If the derived
// path already exists, a "_N" disambiguator is appended before the extension
// (recording_part-002_2.mkv). Locale-independent, Windows-safe, deterministic.
std::filesystem::path DeriveSegmentPath(const std::filesystem::path& base, std::uint32_t index);

// Derive the transient MKV path used when recording with Container::Mp4
// (ADR-0014: remux-on-stop architecture). The engine records to this MKV file;
// on successful stop the app layer remuxes it to the final MP4 and deletes it.
// The transient path is the requested MP4 path with extension replaced by ".mkv.tmp".
// Example: "recording.mp4" -> "recording.mkv.tmp"
std::filesystem::path DeriveTransientMkvPath(const std::filesystem::path& mp4_output_path);

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

    // Thread-safe request to split the recording at the next safe boundary.
    // Valid only while Record() is running (in Recording or Paused state). The
    // current segment is finalized and a new one begins with a forced keyframe;
    // capture/encode/audio continue uninterrupted. Coalesced: repeated requests
    // before the previous boundary is reached count as one. The trigger source
    // is recorded for logging only. No-op if not recording or if the session was
    // started without splitting wired (it is always wired; mode only gates auto).
    void RequestSplit(SplitTriggerSource source);

    // Register a callback invoked from the mux worker thread as each media
    // segment is finalized (including the final one). Must be set before
    // Record(). For single-file recordings, fires exactly once.
    void SetSegmentCallback(SegmentCallback cb);

    // Thread-safe live webcam overlay update. Safe to call from any thread while
    // Record() is running. No-op if not recording or if the session was started
    // without a webcam frame provider.
    void UpdateWebcamOverlay(const WebcamOverlayLive& overlay);

    // Register a stats callback invoked approximately every 264 ms from an
    // internal worker thread.  Must be set before calling Record().
    void SetStatsCallback(StatsCallback cb);

    // Register a meter callback invoked approximately every 33 ms from an
    // internal worker thread.  Must be set before calling Record().
    void SetMeterCallback(MeterCallback cb);

    // Register a live pipeline-diagnostics callback invoked approximately every
    // 200 ms (5 Hz) from an internal worker thread while recording, plus one final
    // frozen snapshot (Completed/Failed) when Record() returns. Must be set before
    // calling Record(). Optional: leaving it unset disables diagnostics with no cost.
    void SetDiagnosticsCallback(DiagnosticsCallback cb);

    // Request a one-shot BGRA frame snapshot from the next composed video frame.
    // The callback fires from VideoThread with (success, width, height, bgra_bytes, error).
    // No-op if not recording or a snapshot is already pending.
    // If the session stops while the request is pending the callback fires with success=false.
    using FrameSnapshotCallback = std::function<void(bool, uint32_t, uint32_t, std::vector<uint8_t>, std::string)>;
    void RequestFrameSnapshot(FrameSnapshotCallback callback);

  private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace recorder_core
