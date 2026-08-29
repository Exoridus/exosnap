#pragma once

#include <exosnap/engine/audio_track_model.h>
#include <exosnap/engine/color_metadata.h>

#include "codec_types.h"
#include "error_types.h"
#include "frame_pacing.h"
#include "hdr_native.h"
#include "output_geometry.h"
#include "pipeline_diagnostics.h"
#include "preview_tap.h"
#include "session_stats.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace exosnap::engine {

// ---------------------------------------------------------------------------
// WebcamFrameProvider — injected into RecorderConfig for compositing
// ---------------------------------------------------------------------------

// Called by VideoThread to pull the latest webcam BGRA frame.
// Implementations must be thread-safe: TryGetFrame is called from VideoThread.
// The provider must remain alive for the duration of Record().
struct WebcamFrameProvider {
    // Returns true and fills out_width/out_height/out_bgra with the LATEST captured
    // frame. out_bgra is BGRA (B8G8R8A8 byte order), row-major.
    //
    // Freeze-on-loss contract: once a frame has been captured, a subsequent device
    // loss (unplug / driver error) does NOT make this return false — the last
    // captured frame keeps being served (frozen) so the composite holds the last
    // webcam image instead of the PiP vanishing, matching the DXGI monitor recovery
    // (ADR 0013) and industry practice. The provider recovers live if the device
    // returns. Returns false only before the first frame is captured (nothing to
    // show yet) or after the provider is stopped.
    // out_generation is bumped once per newly captured sample (StoreFrame call),
    // regardless of whether the pixels changed — callers use it to skip redundant
    // recomposition when the webcam has not produced a new sample since last tick.
    virtual bool TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra,
                             uint64_t& out_generation) = 0;
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

    // Uniform overlay opacity [0,1]; 1.0 = fully opaque. Applied to the sprite's
    // alpha after chroma keying, so keyed edges and overall fade compose correctly.
    float opacity = 1.0f;

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
    // Uniform overlay opacity [0,1]; 1.0 = fully opaque. Applied to the sprite's
    // alpha after chroma keying, so keyed edges and overall fade compose correctly.
    float opacity = 1.0f;
    // chroma_r/g/b carry the resolved active key color (not the raw mode enum).
    bool chroma_key_enabled = false;
    uint8_t chroma_r = 0;
    uint8_t chroma_g = 255;
    uint8_t chroma_b = 0;
    float chroma_tolerance = 0.40f;
    float chroma_softness = 0.15f;
    float chroma_spill_reduction = 0.30f;
};

} // namespace exosnap::engine

namespace exosnap::engine {

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
// PreviewSharedHandleCallback — WYSIWYG preview via a shared GPU texture
// ---------------------------------------------------------------------------

// Invoked ONCE from VideoThread when the engine has created the shared
// preview texture for a session — an NT-handle + keyed-mutex D3D11 texture
// holding the composited, pre-encode source frame (cursor + webcam PiP baked
// in, exactly as recorded). The engine copies each newly composed frame into
// that texture without ever stalling the encode path; the consumer opens the
// handle on its own device and samples it. Zero CPU copies.
//
// `nt_handle` is a Windows HANDLE passed as uintptr_t to keep <windows.h> out
// of this public header. Ownership passes to the callback: open it with
// ID3D11Device1::OpenSharedResource1, then CloseHandle. The callback MUST
// return quickly and MUST NOT make D3D11 calls on the calling (video) thread —
// stash the handle and hand off to the consumer's render thread.
//
// `tap` describes the display transform the consumer must apply before drawing
// (preview_tap.h): SDR / HDR-tone-map / 4:4:4 sessions share a samplable SDR or
// 10-bit surface (PreviewTapTransform::None); a native HDR10 session shares its
// linear scRGB FP16 pre-encode surface (ScrgbHdr — tone-map before display).
// The only session that never fires is the already-PQ R10G10B10A2 native
// sub-path, which has no linear surface to share. Must be set before Record();
// the callback is captured at Record() start and does not survive
// Stop()/Record() cycles.
using PreviewSharedHandleCallback =
    std::function<void(uintptr_t nt_handle, uint32_t width, uint32_t height, PreviewTapDesc tap)>;

// Fired from the video thread after every frame that was actually published into
// the shared preview texture (never for a frame the non-blocking keyed-mutex
// acquire dropped). It carries no payload: the shared texture always holds the
// newest published frame, so the edge itself is the whole message.
//
// It exists because the transport has no other way to say "there is something
// new to take". A consumer without this edge can only poll the keyed mutex, and
// a consumer whose redraw is driven by its own poll re-renders whether or not a
// frame arrived — measured on the Qt Quick frontend as 10 061 window renders
// against 3 consumed frames on an idle desktop.
//
// Same contract as PreviewSharedHandleCallback: must return quickly, must not
// make D3D11 calls on the calling (video) thread, and must be set before
// Record(). Optional; unset costs one null check per published frame.
using PreviewFramePublishedCallback = std::function<void()>;

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
    VideoCodec video_codec = VideoCodec::Av1;
    AudioCodec audio_codec = AudioCodec::Opus;
    ChromaSubsampling chroma = ChromaSubsampling::Cs420;
    BitDepth bit_depth = BitDepth::Bit8;

    // Color description for the encoded video (ADR 0032). Default SDR BT.709
    // full-range; written into the container and matched by the encoder-input
    // color conversion (range is user-selectable — Full default / Limited). HDR
    // fields stay unset until the HDR slice.
    ColorMetadata color = ColorMetadata::Sdr709();

    // HDR handling mode (config plumbing only for now). An HDR-capable
    // desktop is auto-detected elsewhere; Default TonemapSdr tone-maps it
    // down to SDR, Hdr10 is an expert opt-in that keeps the native
    // PQ/BT.2020 signal, Off disables HDR handling entirely (legacy SDR-only
    // behavior). This only threads the mode through the config pipeline for
    // now — no BT.2020/PQ values are derived into `color` above yet (needs
    // runtime display facts, still to be wired up).
    HdrMode hdr_mode = HdrMode::TonemapSdr;

    // Constant-quality target. 1 = best, 51 = worst. Only used when the rate
    // control mode is ConstantQuality; the encoder backend maps it onto its own
    // quality parameter (NVENC: CQP).
    uint32_t cq = CanonicalCq(QualityPreset::Balanced);

    // Canonical rate-control mode (ADR 0009). Defaults to ConstantQuality (existing behavior).
    RateControlMode nvenc_rate_control = RateControlMode::ConstantQuality;

    // NVENC speed/quality preset (P1 fastest/lowest quality .. P7 slowest/best
    // quality). Applies uniformly to all three NVENC codecs; never capability-
    // gated. Default P4 (balanced) — matches the prior hardcoded AV1/HEVC
    // default; H.264 previously used P6 (visible default change, expert-
    // overridable — see ADR 0039).
    NvencPreset nvenc_preset = NvencPreset::P4;

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

    // ---------------------------------------------------------------------------
    // Audio format model (ADR 0030 — 0.6.0)
    // ---------------------------------------------------------------------------

    // Target sample rate in Hz. Vetted set: 44100, 48000, 96000.
    // Opus requires 48000 (enforced by Validate). Default 48000.
    uint32_t audio_sample_rate = 48000;

    // Target channel count. Vetted set: 1 (mono), 2 (stereo). Default 2.
    uint32_t audio_channels = 2;

    // Bit depth for lossless codecs (PCM, FLAC). Lossy codecs (Opus, AAC) ignore
    // this field. PCM vetted: 16, 24, 32. FLAC vetted: 16, 24. Default 16.
    uint32_t audio_bit_depth = 16;

    // 32-bit float PCM (A_PCM/FLOAT/IEEE) instead of signed-int PCM
    // (A_PCM/INT/LIT). Only valid when audio_codec == AudioCodec::Pcm and
    // audio_bit_depth == 32; Validate() rejects any other combination.
    // Default false (existing int-PCM behavior, unchanged).
    bool audio_pcm_float = false;

    // FLAC compression level [0, 8]. 0 = fastest / largest; 8 = slowest / smallest.
    // Lossless at every level; only trades encode CPU vs. file size. Default 5.
    // Ignored when audio_codec != Flac.
    int flac_compression_level = 5;

    // Frame rate (numerator/denominator)
    uint32_t frame_rate_num = 60;
    uint32_t frame_rate_den = 1;

    // When true: CFR scheduler (duplicate/drop frames to hit constant rate).
    // When false: VFR passthrough (WGC timestamps used directly as PTS).
    bool cfr = true;

    // CFR frame pacing (ADR 0035). Smooth = phase-correct selection; Newest = newest-at-tick.
    // Ignored for VFR and for WGC capture (no LastPresentTime → newest-at-tick).
    FramePacingMode cfr_pacing_mode = FramePacingMode::Smooth;

    // Keyframe interval in seconds. Controls NVENC gopLength/idrPeriod.
    // gopLength = round(keyframe_interval_secs * frame_rate_num / frame_rate_den).
    // Valid values: 0.5, 1.0, 2.0. Default 2.0 matches the pre-0.9.0 hardcoded value.
    // Shorter intervals improve Quick Trim accuracy at a minor file-size cost (~1–2 %).
    // Info-i: shown in Advanced → Video as the "Keyframe interval" setting.
    float keyframe_interval_secs = 2.0f;

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
    // A/V clock slaving (H-3)
    // ---------------------------------------------------------------------------

    // When true (default), audio gently tracks the video (QPC) clock: once the
    // measured device-clock drift crosses ~15 ms the audio output timeline is
    // resampled by a sub-audible ppm amount (<= 0.05 %) so long recordings do not
    // drift out of sync. Default on, codec-independent (sync before bit-exactness);
    // the expert opt-out restores byte-identical capture for archival PCM/FLAC.
    // Below the engage threshold — the majority of sessions — it is a no-op and
    // the default 48 kHz/stereo path stays a byte-identical passthrough.
    bool audio_clock_slaving_enabled = true;

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

    // ---------------------------------------------------------------------------
    // Microphone automatic gain control (Audio v2 — 0.6.0)
    // ---------------------------------------------------------------------------

    // When true, the microphone input is run through an automatic gain control
    // (the third stage of the MicDspAudioSrc chain, after the high-pass filter
    // and noise gate): a slowly-varying makeup gain tracks the mic level and
    // drives it toward a target loudness (quiet talker boosted, loud attenuated).
    // Default false: mic DSP alters captured audio, so it is opt-in.
    bool mic_agc_enabled = false;

    // AGC target loudness in dBFS. Default -18 dB.
    float mic_agc_target_db = -18.0f;

    // ---------------------------------------------------------------------------
    // Microphone RNNoise neural noise suppression (Audio v2 — 0.6.0)
    // ---------------------------------------------------------------------------

    // When true, the microphone input is run through RNNoise (the fourth and
    // final stage of the MicDspAudioSrc chain, after the high-pass filter, the
    // noise gate, and the AGC): a trained recurrent network attenuates background
    // noise (fans, keyboards, hum, hiss) while preserving speech. Default false:
    // mic DSP alters captured audio, so it is opt-in. RNNoise has no numeric
    // parameter (it is a fixed trained model). It runs only at 48 kHz.
    bool mic_rnnoise_enabled = false;

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

// Apply the native HDR10 (PQ/BT.2020) encode overrides to a base config once the
// caller has established that the native path is effective (see
// IsHdr10NativeEffective) and gathered the captured display's HdrDisplayFacts:
//   * colour metadata derived from the display facts (BT.2020/PQ, mastering data),
//   * bit depth pinned to 10-bit (HDR10 is 10-bit by definition), and
//   * chroma snapped to 4:2:0 — 4:4:4 (AYUV) is an 8-bit-only path, so a leftover
//     Cs444 selection would otherwise reach Validate() as Cs444 + Bit10 and fail
//     the recording start. Returns true iff the chroma was snapped (the caller
//     may log a reconcile line). Pure: no logging, no D3D.
[[nodiscard]] inline bool ApplyHdr10NativeEncode(RecorderConfig& config, const HdrDisplayFacts& facts) noexcept {
    config.color = MakeHdr10ColorMetadata(facts);
    config.bit_depth = BitDepth::Bit10;
    const bool chroma_snapped = config.chroma != ChromaSubsampling::Cs420;
    config.chroma = ChromaSubsampling::Cs420;
    return chroma_snapped;
}

// Result of DeriveSegmentPath: the derived candidate path on success, or an
// error describing why no candidate could be CONFIRMED free.
//
// Mirrors this codebase's existing domain-specific result-struct convention
// (see RemuxResult in mp4_remuxer.h) rather than std::expected: this project
// targets C++20 (CMAKE_CXX_STANDARD 20), and <expected> is a C++23-only
// library feature even on the exact MSVC toolset this repo builds with today
// (verified: MSVC STL emits "STL4038: The contents of <expected> are
// available only with C++23 or later" under /std:c++20) — so introducing
// std::expected here would require bumping the whole project's language
// standard, which is out of scope for this fix.
struct SegmentPathResult {
    bool success = false;
    std::filesystem::path path;
    // Set only on failure. On this platform, filesystem probe errors surface
    // through std::system_category (Win32 error values), so callers that
    // report failures as HRESULT can convert via
    // HRESULT_FROM_WIN32(static_cast<DWORD>(error.value())).
    std::error_code error;
    std::string message;

    static SegmentPathResult Ok(std::filesystem::path p) {
        SegmentPathResult r;
        r.success = true;
        r.path = std::move(p);
        return r;
    }
    static SegmentPathResult Fail(std::error_code ec, std::string msg) {
        SegmentPathResult r;
        r.success = false;
        r.error = ec;
        r.message = std::move(msg);
        return r;
    }
};

// Probe used to test whether a candidate segment path already exists.
// Production callers must never pass this argument — the default (nullptr)
// resolves to std::filesystem::exists. It exists solely so tests can simulate
// a genuine filesystem probe error (permission denied, unreadable path, ...)
// deterministically: std::filesystem::exists on this toolchain silently
// downgrades most real OS errors (access-denied, invalid path, ...) to "not
// found" (empty error_code), so such an error cannot be reproduced organically
// in a test (verified empirically: ACL-denied directories, invalid
// characters, and very long paths all still yield an empty error_code here).
using SegmentPathExistsProbe = std::function<bool(const std::filesystem::path&, std::error_code&)>;

// Derive the on-disk path for segment `index` (0-based) from a base output path.
// Segment 0 keeps the base name verbatim (no existence probe, no rename of the
// first file) and always succeeds. Later segments insert a "_part-NNN" suffix
// before the extension (recording.mkv -> recording_part-002.mkv); if that
// candidate already exists, a "_N" disambiguator is appended before the
// extension (recording_part-002_2.mkv) and probed again.
//
// Collision-safety contract:
//   - Only a candidate CONFIRMED free (exists() returned false with an empty
//     error_code) is ever returned as success.
//   - A real filesystem error while probing a candidate (nonzero error_code)
//     is propagated immediately as a failure -- it is never treated the same
//     as "path is free".
//   - Exhausting the bounded collision scan (10000 candidates) without
//     finding a free one is a defined failure, never a silent return of the
//     last (still-colliding) candidate.
//
// Locale-independent, Windows-safe, deterministic (given a fixed filesystem
// state).
SegmentPathResult DeriveSegmentPath(const std::filesystem::path& base, std::uint32_t index,
                                    const SegmentPathExistsProbe& exists_probe = nullptr);

// Returns the valuable live artifact for a final output path. The complete
// final filename is preserved and ".partial" is appended.
std::filesystem::path DeriveValuablePartialPath(const std::filesystem::path& final_output_path);

// Returns the valuable MKV-backed live artifact for an MP4 output path.
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
    // Mute or unmute one audio source kind while a recording runs. The track it
    // belongs to keeps running at full length and carries silence for as long as
    // the mute stands; nothing is re-opened, so unmuting resumes on the next
    // packet. Ignored between sessions -- the source rows are what a new session
    // starts from.
    void SetAudioSourceMuted(AudioSourceKind kind, bool muted) noexcept;

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

    // Register the WYSIWYG preview shared-texture callback (see
    // PreviewSharedHandleCallback). Fired once from VideoThread when the shared
    // preview texture is ready. Must be set before Record(): the callback is
    // captured at Record() start, so setting or clearing it while a recording is
    // running has NO effect until the next Record(). Optional: leaving it unset
    // disables the preview tap at zero cost (the shared texture is never created).
    void SetPreviewSharedHandleCallback(PreviewSharedHandleCallback cb);

    // Register the per-frame publish edge (see PreviewFramePublishedCallback).
    // Same capture-at-Record() lifetime as SetPreviewSharedHandleCallback, and
    // only ever fires while that one is set — the tap is not created without a
    // handle consumer.
    void SetPreviewFramePublishedCallback(PreviewFramePublishedCallback cb);

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

} // namespace exosnap::engine
