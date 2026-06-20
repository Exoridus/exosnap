#include "RecordingPreset.h"

#include <capability/container_compat_registry.h>
#include <recorder_core/audio_track_model.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

namespace exosnap {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Trim leading and trailing ASCII whitespace from a string_view.
[[nodiscard]] std::string TrimWhitespace(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\n\r\f\v");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\n\r\f\v");
    return std::string(s.substr(first, last - first + 1));
}

// Returns a lowercase hex string of `value`, zero-padded to `digits` characters.
[[nodiscard]] std::string ToHex16(uint64_t value) {
    constexpr int kDigits = 16;
    std::string result(kDigits, '0');
    for (int i = kDigits - 1; i >= 0; --i) {
        const int nibble = static_cast<int>(value & 0xFu);
        result[static_cast<std::size_t>(i)] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        value >>= 4u;
    }
    return result;
}

// Compares two AudioTrackPlan values for structural equality.
[[nodiscard]] bool AudioTrackPlansEqual(const recorder_core::AudioTrackPlan& a,
                                        const recorder_core::AudioTrackPlan& b) noexcept {
    if (a.tracks.size() != b.tracks.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.tracks.size(); ++i) {
        const auto& ta = a.tracks[i];
        const auto& tb = b.tracks[i];
        if (ta.sources != tb.sources) {
            return false;
        }
    }
    return true;
}

// Returns the set of enabled AudioSourceKind values from a row vector.
[[nodiscard]] std::set<recorder_core::AudioSourceKind>
EnabledSourceKinds(const std::vector<recorder_core::AudioSourceRow>& rows) {
    std::set<recorder_core::AudioSourceKind> result;
    for (const auto& row : rows) {
        if (row.enabled) {
            result.insert(row.kind);
        }
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// GeneratePresetId
// ---------------------------------------------------------------------------

std::string GeneratePresetId() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<uint64_t> dist;
    const uint64_t value = dist(rng);
    return std::string("preset.") + ToHex16(value);
}

// ---------------------------------------------------------------------------
// MakeDefaultPreset
// ---------------------------------------------------------------------------

RecordingPreset MakeDefaultPreset() {
    RecordingPreset preset;
    preset.id = std::string(kDefaultPresetId);
    preset.name = "Default";

    // Capture
    preset.config.capture.kind = PresetCaptureKind::Display;
    preset.config.capture.display_key = "";
    preset.config.capture.window_key = "";
    preset.config.capture.has_region = false;
    preset.config.capture.region = recorder_core::CaptureRegion{};
    preset.config.capture.region_display_key = "";

    // Output — start from Defaults() then override codecs.
    // MKV + AV1 + Opus is a valid combination; do NOT call ReconcileContainerCodecs.
    preset.config.output = OutputSettingsModel::Defaults();
    preset.config.output.container = capability::Container::Matroska;
    preset.config.output.video_codec = capability::VideoCodec::Av1Nvenc;
    preset.config.output.audio_codec = capability::AudioCodec::Opus;

    // Video
    preset.config.video.quality = recorder_core::NvencQualityPreset::High;
    preset.config.video.cfr = true;
    preset.config.video.capture_cursor = true;
    preset.config.video.frame_rate_num = 60;
    preset.config.video.frame_rate_den = 1;

    // Audio
    preset.config.audio.target_kind = capability::CaptureTargetKind::Display;
    preset.config.audio.source_rows = {
        {recorder_core::AudioSourceKind::SystemOutput, true, false},
        {recorder_core::AudioSourceKind::Mic, false, false},
    };
    preset.config.audio.mic_channel_mode = recorder_core::MicChannelMode::Auto;
    preset.config.audio.selected_mic_device_id = std::nullopt;
    preset.config.audio.mic_gain_linear = 1.0f;
    preset.config.audio.selected_window_pid = std::nullopt;

    // Webcam — bottom-right PiP at 25 % of frame width/height, with inset.
    preset.config.webcam.enabled = false;
    preset.config.webcam.device_id = "";
    preset.config.webcam.width = 1280;
    preset.config.webcam.height = 720;
    preset.config.webcam.fps = 30;
    preset.config.webcam.mirror = false;
    preset.config.webcam.aspect_ratio_locked = true;
    preset.config.webcam.overlay_user_placed = false;

    // Bottom-right placement: x = 1.0 - 0.25 - inset, y = 1.0 - 0.25 - inset
    constexpr float kPipSize = 0.25f;
    preset.config.webcam.overlay.w_norm = kPipSize;
    preset.config.webcam.overlay.h_norm = kPipSize;
    preset.config.webcam.overlay.x_norm = 1.0f - kPipSize - kDefaultPipInsetNorm;
    preset.config.webcam.overlay.y_norm = 1.0f - kPipSize - kDefaultPipInsetNorm;

    // Chroma key — default (disabled; conservative green-screen values)
    preset.config.webcam.chroma_key.enabled = false;
    preset.config.webcam.chroma_key.color_mode = WebcamChromaKeyColorMode::Green;
    preset.config.webcam.chroma_key.custom_r = 0;
    preset.config.webcam.chroma_key.custom_g = 255;
    preset.config.webcam.chroma_key.custom_b = 0;
    preset.config.webcam.chroma_key.tolerance = 0.40f;
    preset.config.webcam.chroma_key.softness = 0.15f;
    preset.config.webcam.chroma_key.spill_reduction = 0.30f;

    // Countdown
    preset.config.countdown_seconds = 0;

    return preset;
}

// ---------------------------------------------------------------------------
// ReconcileContainerCodecs
// ---------------------------------------------------------------------------

void ReconcileContainerCodecs(OutputSettingsModel& output) {
    // Delegate to the single-source-of-truth registry (ADR 0010).
    // ContainerCompatRegistry::ReconcileCodecs replaces the previous ad-hoc
    // switch/if chain and uses the same compatibility table that gates
    // recording start via CapabilitySet::QueryCombo().
    capability::ContainerCompatRegistry::ReconcileCodecs(output.container, output.video_codec, output.audio_codec);
}

// ---------------------------------------------------------------------------
// SanitizePresetConfig
// ---------------------------------------------------------------------------

RecordingPresetConfig SanitizePresetConfig(RecordingPresetConfig config) {
    // Countdown: must be one of {0, 3, 5, 10}
    constexpr std::array<int, 4> kValidCountdowns = {0, 3, 5, 10};
    const bool countdown_valid =
        std::find(kValidCountdowns.begin(), kValidCountdowns.end(), config.countdown_seconds) != kValidCountdowns.end();
    if (!countdown_valid) {
        config.countdown_seconds = 0;
    }

    // Output: reconcile codecs and normalize the canonical output-size intent.
    ReconcileContainerCodecs(config.output);
    SanitizeOutputResolution(config.output.resolution);
    SanitizeSplitSettings(config.output.split);

    // Video: reset frame rate if degenerate (either numerator or denominator is zero).
    if (config.video.frame_rate_num == 0 || config.video.frame_rate_den == 0) {
        config.video.frame_rate_num = 60;
        config.video.frame_rate_den = 1;
    }
    {
        constexpr std::array<uint32_t, 5> kValidFps = {24, 25, 30, 50, 60};
        const bool valid_rate =
            config.video.frame_rate_den == 1 &&
            std::find(kValidFps.begin(), kValidFps.end(), config.video.frame_rate_num) != kValidFps.end();
        if (!valid_rate) {
            config.video.frame_rate_num = 60;
            config.video.frame_rate_den = 1;
        }
    }
    if (config.output.container == capability::Container::Mp4 && !config.video.cfr) {
        config.video.cfr = true;
    }
    // Video: rate_control — default to ConstantQuality if an unknown value slips through.
    using RC = recorder_core::RateControlMode;
    if (config.video.rate_control != RC::ConstantQuality && config.video.rate_control != RC::VariableBitrate &&
        config.video.rate_control != RC::ConstantBitrate && config.video.rate_control != RC::Lossless) {
        config.video.rate_control = RC::ConstantQuality;
    }
    // Lossless is not implemented — silently revert to ConstantQuality.
    if (config.video.rate_control == RC::Lossless) {
        config.video.rate_control = RC::ConstantQuality;
    }
    // Video: clamp bitrate to [1000, 200000] kbps; only meaningful for VBR/CBR.
    constexpr uint32_t kMinBitrateKbps = 1000u;
    constexpr uint32_t kMaxBitrateKbps = 200000u;
    if (config.video.bitrate_kbps < kMinBitrateKbps) {
        config.video.bitrate_kbps = kMinBitrateKbps;
    } else if (config.video.bitrate_kbps > kMaxBitrateKbps) {
        config.video.bitrate_kbps = kMaxBitrateKbps;
    }

    // Audio: ensure mic_gain_linear is finite and strictly positive.
    if (!std::isfinite(config.audio.mic_gain_linear) || config.audio.mic_gain_linear <= 0.0f) {
        config.audio.mic_gain_linear = 1.0f;
    }

    // Audio v2 (0.6.0): clamp per-row gain_db to [kMinGainDb, kMaxGainDb]; reset NaN/Inf.
    for (auto& row : config.audio.source_rows) {
        if (!std::isfinite(row.gain_db)) {
            row.gain_db = 0.0f;
        } else if (row.gain_db < recorder_core::kMinGainDb) {
            row.gain_db = recorder_core::kMinGainDb;
        } else if (row.gain_db > recorder_core::kMaxGainDb) {
            row.gain_db = recorder_core::kMaxGainDb;
        }
        // muted is bool — no sanitization needed.
    }

    // Audio encoding params (ADR 0019):
    // audio_bitrate_kbps: 0 is valid (auto default); non-zero clamped to broadest safe range.
    // Codec-specific clamping ([32,510] Opus / [64,320] AAC) happens in the engine.
    if (config.audio.audio_bitrate_kbps > 510u) {
        config.audio.audio_bitrate_kbps = 510u;
    }
    // opus_complexity: clamp to [0, 10].
    if (config.audio.opus_complexity < 0) {
        config.audio.opus_complexity = 0;
    } else if (config.audio.opus_complexity > 10) {
        config.audio.opus_complexity = 10;
    }
    // opus_frame_duration: reset unknown values to the default (20 ms).
    {
        using D = recorder_core::OpusFrameDuration;
        const int d = static_cast<int>(config.audio.opus_frame_duration);
        if (d != static_cast<int>(D::Ms20) && d != static_cast<int>(D::Ms10) && d != static_cast<int>(D::Ms5) &&
            d != static_cast<int>(D::Ms2_5)) {
            config.audio.opus_frame_duration = D::Ms20;
        }
    }

    // Brickwall limiter (Audio v2 — 0.6.0): ceiling is a dBFS value <= 0. Reset
    // NaN/Inf to 0 dBFS; clamp positives to 0 dBFS and an absurd floor to -60.
    if (!std::isfinite(config.audio.limiter_ceiling_db) || config.audio.limiter_ceiling_db > 0.0f) {
        config.audio.limiter_ceiling_db = 0.0f;
    } else if (config.audio.limiter_ceiling_db < -60.0f) {
        config.audio.limiter_ceiling_db = -60.0f;
    }

    // Microphone high-pass filter (Audio v2 — 0.6.0): cutoff in Hz. Reset NaN/Inf
    // to 80 Hz; clamp to the usable [20, 1000] Hz range.
    if (!std::isfinite(config.audio.mic_hpf_cutoff_hz)) {
        config.audio.mic_hpf_cutoff_hz = 80.0f;
    } else if (config.audio.mic_hpf_cutoff_hz < 20.0f) {
        config.audio.mic_hpf_cutoff_hz = 20.0f;
    } else if (config.audio.mic_hpf_cutoff_hz > 1000.0f) {
        config.audio.mic_hpf_cutoff_hz = 1000.0f;
    }

    // Webcam: delegate to the provided sanitizer (handles NaN/Inf + clamping).
    config.webcam = SanitizeWebcamSettings(config.webcam);

    // Capture: if kind is Region but region is not valid, clear has_region.
    if (config.capture.kind == PresetCaptureKind::Region && !config.capture.region.IsValid()) {
        config.capture.has_region = false;
    }

    return config;
}

// ---------------------------------------------------------------------------
// SanitizePreset
// ---------------------------------------------------------------------------

RecordingPreset SanitizePreset(RecordingPreset preset) {
    // Name: trim; if empty after trim, use fallback.
    const std::string trimmed_name = TrimWhitespace(preset.name);
    preset.name = trimmed_name.empty() ? "Untitled preset" : trimmed_name;

    // Id: ensure non-empty.
    if (preset.id.empty()) {
        preset.id = GeneratePresetId();
    }

    // Config: sanitize.
    preset.config = SanitizePresetConfig(std::move(preset.config));

    return preset;
}

// ---------------------------------------------------------------------------
// IsValidPresetName / NormalizePresetName
// ---------------------------------------------------------------------------

bool IsValidPresetName(std::string_view name) {
    return !TrimWhitespace(name).empty();
}

std::string NormalizePresetName(std::string_view name) {
    return TrimWhitespace(name);
}

// ---------------------------------------------------------------------------
// NormalizedConfigEquals
// ---------------------------------------------------------------------------

bool NormalizedConfigEquals(const RecordingPresetConfig& a, const RecordingPresetConfig& b) {
    // Countdown
    if (a.countdown_seconds != b.countdown_seconds) {
        return false;
    }

    // --- Capture ---
    if (a.capture.kind != b.capture.kind) {
        return false;
    }
    if (a.capture.display_key != b.capture.display_key) {
        return false;
    }
    if (a.capture.window_key != b.capture.window_key) {
        return false;
    }
    if (a.capture.region_display_key != b.capture.region_display_key) {
        return false;
    }
    if (a.capture.has_region != b.capture.has_region) {
        return false;
    }
    if (a.capture.has_region) {
        if (a.capture.region.x != b.capture.region.x || a.capture.region.y != b.capture.region.y ||
            a.capture.region.width != b.capture.region.width || a.capture.region.height != b.capture.region.height) {
            return false;
        }
    }

    // --- Output ---
    if (a.output.container != b.output.container) {
        return false;
    }
    if (a.output.video_codec != b.output.video_codec) {
        return false;
    }
    if (a.output.audio_codec != b.output.audio_codec) {
        return false;
    }
    if (a.output.resolution.mode != b.output.resolution.mode) {
        return false;
    }
    if (a.output.resolution.custom_width != b.output.resolution.custom_width) {
        return false;
    }
    if (a.output.resolution.custom_height != b.output.resolution.custom_height) {
        return false;
    }
    if (a.output.resolution.fit != b.output.resolution.fit) {
        return false;
    }
    if (a.output.output_folder != b.output.output_folder) {
        return false;
    }
    if (a.output.naming_pattern != b.output.naming_pattern) {
        return false;
    }
    if (a.output.split != b.output.split) {
        return false;
    }

    // --- Video ---
    if (a.video.quality != b.video.quality) {
        return false;
    }
    if (a.video.rate_control != b.video.rate_control) {
        return false;
    }
    if (a.video.bitrate_kbps != b.video.bitrate_kbps) {
        return false;
    }
    if (a.video.cfr != b.video.cfr) {
        return false;
    }
    if (a.video.capture_cursor != b.video.capture_cursor) {
        return false;
    }
    if (a.video.frame_rate_num != b.video.frame_rate_num) {
        return false;
    }
    if (a.video.frame_rate_den != b.video.frame_rate_den) {
        return false;
    }

    // --- Audio ---
    if (a.audio.target_kind != b.audio.target_kind) {
        return false;
    }
    if (a.audio.mic_channel_mode != b.audio.mic_channel_mode) {
        return false;
    }
    if (a.audio.selected_mic_device_id != b.audio.selected_mic_device_id) {
        return false;
    }
    if (a.audio.selected_window_pid != b.audio.selected_window_pid) {
        return false;
    }
    if (std::abs(a.audio.mic_gain_linear - b.audio.mic_gain_linear) > 1e-3f) {
        return false;
    }

    // Audio encoding params (ADR 0019).
    if (a.audio.audio_bitrate_kbps != b.audio.audio_bitrate_kbps) {
        return false;
    }
    if (a.audio.opus_frame_duration != b.audio.opus_frame_duration) {
        return false;
    }
    if (a.audio.opus_complexity != b.audio.opus_complexity) {
        return false;
    }
    // Brickwall limiter (Audio v2): enabled (exact) + ceiling (1e-2 dB tolerance).
    if (a.audio.limiter_enabled != b.audio.limiter_enabled) {
        return false;
    }
    if (std::abs(a.audio.limiter_ceiling_db - b.audio.limiter_ceiling_db) > 1e-2f) {
        return false;
    }
    // Mic high-pass filter (Audio v2): enabled (exact) + cutoff (1e-2 Hz tolerance).
    if (a.audio.mic_hpf_enabled != b.audio.mic_hpf_enabled) {
        return false;
    }
    if (std::abs(a.audio.mic_hpf_cutoff_hz - b.audio.mic_hpf_cutoff_hz) > 1e-2f) {
        return false;
    }

    // Semantic audio-row equality: same resolved plan AND same enabled-source set.
    {
        const recorder_core::AudioTrackPlan plan_a = recorder_core::ResolveAudioTracks(a.audio.source_rows);
        const recorder_core::AudioTrackPlan plan_b = recorder_core::ResolveAudioTracks(b.audio.source_rows);
        if (!AudioTrackPlansEqual(plan_a, plan_b)) {
            return false;
        }
        if (EnabledSourceKinds(a.audio.source_rows) != EnabledSourceKinds(b.audio.source_rows)) {
            return false;
        }
    }
    // Audio v2 (0.6.0): compare per-row gain_db and muted.
    {
        if (a.audio.source_rows.size() != b.audio.source_rows.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.audio.source_rows.size(); ++i) {
            const auto& ra = a.audio.source_rows[i];
            const auto& rb = b.audio.source_rows[i];
            if (ra.muted != rb.muted) {
                return false;
            }
            if (std::abs(ra.gain_db - rb.gain_db) > 1e-2f) {
                return false;
            }
        }
    }

    // --- Webcam ---
    constexpr float kPipTol = 1e-3f;
    constexpr float kChromaTol = 1e-3f;

    if (a.webcam.enabled != b.webcam.enabled) {
        return false;
    }
    if (a.webcam.device_id != b.webcam.device_id) {
        return false;
    }
    if (a.webcam.width != b.webcam.width) {
        return false;
    }
    if (a.webcam.height != b.webcam.height) {
        return false;
    }
    if (a.webcam.fps != b.webcam.fps) {
        return false;
    }
    if (a.webcam.mirror != b.webcam.mirror) {
        return false;
    }
    if (a.webcam.aspect_ratio_locked != b.webcam.aspect_ratio_locked) {
        return false;
    }
    if (a.webcam.overlay_user_placed != b.webcam.overlay_user_placed) {
        return false;
    }

    // PiP overlay — float tolerance
    if (std::abs(a.webcam.overlay.x_norm - b.webcam.overlay.x_norm) > kPipTol) {
        return false;
    }
    if (std::abs(a.webcam.overlay.y_norm - b.webcam.overlay.y_norm) > kPipTol) {
        return false;
    }
    if (std::abs(a.webcam.overlay.w_norm - b.webcam.overlay.w_norm) > kPipTol) {
        return false;
    }
    if (std::abs(a.webcam.overlay.h_norm - b.webcam.overlay.h_norm) > kPipTol) {
        return false;
    }

    // Chroma key
    if (a.webcam.chroma_key.enabled != b.webcam.chroma_key.enabled) {
        return false;
    }
    if (a.webcam.chroma_key.color_mode != b.webcam.chroma_key.color_mode) {
        return false;
    }
    if (a.webcam.chroma_key.custom_r != b.webcam.chroma_key.custom_r) {
        return false;
    }
    if (a.webcam.chroma_key.custom_g != b.webcam.chroma_key.custom_g) {
        return false;
    }
    if (a.webcam.chroma_key.custom_b != b.webcam.chroma_key.custom_b) {
        return false;
    }
    if (std::abs(a.webcam.chroma_key.tolerance - b.webcam.chroma_key.tolerance) > kChromaTol) {
        return false;
    }
    if (std::abs(a.webcam.chroma_key.softness - b.webcam.chroma_key.softness) > kChromaTol) {
        return false;
    }
    if (std::abs(a.webcam.chroma_key.spill_reduction - b.webcam.chroma_key.spill_reduction) > kChromaTol) {
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// ConfigDirtyEquivalent
// ---------------------------------------------------------------------------

bool ConfigDirtyEquivalent(const RecordingPresetConfig& a, const RecordingPresetConfig& b) {
    // Capture identity (kind, display_key, window_key, has_region, region,
    // region_display_key) is intentionally NOT compared here.  Capture depends
    // on transient device availability and auto-resolution, so comparing it
    // would cause spurious/unstable dirty state (e.g. default preset appears
    // dirty at startup because the live policy resolves an empty display_key to
    // a concrete monitor key, or because a monitor is replugged).
    // Per spec: temporary availability changes must not make the preset dirty.
    // NormalizedConfigEquals (full structural equality) is kept for persistence
    // round-trip verification and must NOT be changed.

    // Countdown
    if (a.countdown_seconds != b.countdown_seconds) {
        return false;
    }

    // --- Output ---
    if (a.output.container != b.output.container) {
        return false;
    }
    if (a.output.video_codec != b.output.video_codec) {
        return false;
    }
    if (a.output.audio_codec != b.output.audio_codec) {
        return false;
    }
    if (a.output.resolution.mode != b.output.resolution.mode) {
        return false;
    }
    if (a.output.resolution.custom_width != b.output.resolution.custom_width) {
        return false;
    }
    if (a.output.resolution.custom_height != b.output.resolution.custom_height) {
        return false;
    }
    if (a.output.resolution.fit != b.output.resolution.fit) {
        return false;
    }
    if (a.output.output_folder != b.output.output_folder) {
        return false;
    }
    if (a.output.naming_pattern != b.output.naming_pattern) {
        return false;
    }
    if (a.output.split != b.output.split) {
        return false;
    }

    // --- Video ---
    if (a.video.quality != b.video.quality) {
        return false;
    }
    if (a.video.rate_control != b.video.rate_control) {
        return false;
    }
    if (a.video.bitrate_kbps != b.video.bitrate_kbps) {
        return false;
    }
    if (a.video.cfr != b.video.cfr) {
        return false;
    }
    if (a.video.capture_cursor != b.video.capture_cursor) {
        return false;
    }
    if (a.video.frame_rate_num != b.video.frame_rate_num) {
        return false;
    }
    if (a.video.frame_rate_den != b.video.frame_rate_den) {
        return false;
    }

    // --- Audio ---
    if (a.audio.target_kind != b.audio.target_kind) {
        return false;
    }
    if (a.audio.mic_channel_mode != b.audio.mic_channel_mode) {
        return false;
    }
    if (a.audio.selected_mic_device_id != b.audio.selected_mic_device_id) {
        return false;
    }
    if (a.audio.selected_window_pid != b.audio.selected_window_pid) {
        return false;
    }
    if (std::abs(a.audio.mic_gain_linear - b.audio.mic_gain_linear) > 1e-3f) {
        return false;
    }

    // Audio encoding params (ADR 0019).
    if (a.audio.audio_bitrate_kbps != b.audio.audio_bitrate_kbps) {
        return false;
    }
    if (a.audio.opus_frame_duration != b.audio.opus_frame_duration) {
        return false;
    }
    if (a.audio.opus_complexity != b.audio.opus_complexity) {
        return false;
    }
    // Brickwall limiter (Audio v2): enabled (exact) + ceiling (1e-2 dB tolerance).
    if (a.audio.limiter_enabled != b.audio.limiter_enabled) {
        return false;
    }
    if (std::abs(a.audio.limiter_ceiling_db - b.audio.limiter_ceiling_db) > 1e-2f) {
        return false;
    }
    // Mic high-pass filter (Audio v2): enabled (exact) + cutoff (1e-2 Hz tolerance).
    if (a.audio.mic_hpf_enabled != b.audio.mic_hpf_enabled) {
        return false;
    }
    if (std::abs(a.audio.mic_hpf_cutoff_hz - b.audio.mic_hpf_cutoff_hz) > 1e-2f) {
        return false;
    }

    // Semantic audio-row equality: same resolved plan AND same enabled-source set.
    {
        const recorder_core::AudioTrackPlan plan_a = recorder_core::ResolveAudioTracks(a.audio.source_rows);
        const recorder_core::AudioTrackPlan plan_b = recorder_core::ResolveAudioTracks(b.audio.source_rows);
        if (!AudioTrackPlansEqual(plan_a, plan_b)) {
            return false;
        }
        if (EnabledSourceKinds(a.audio.source_rows) != EnabledSourceKinds(b.audio.source_rows)) {
            return false;
        }
    }
    // Audio v2 (0.6.0): compare per-row gain_db and muted.
    {
        if (a.audio.source_rows.size() != b.audio.source_rows.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.audio.source_rows.size(); ++i) {
            const auto& ra = a.audio.source_rows[i];
            const auto& rb = b.audio.source_rows[i];
            if (ra.muted != rb.muted) {
                return false;
            }
            if (std::abs(ra.gain_db - rb.gain_db) > 1e-2f) {
                return false;
            }
        }
    }

    // --- Webcam ---
    constexpr float kPipTol = 1e-3f;
    constexpr float kChromaTol = 1e-3f;

    if (a.webcam.enabled != b.webcam.enabled) {
        return false;
    }
    if (a.webcam.device_id != b.webcam.device_id) {
        return false;
    }
    if (a.webcam.width != b.webcam.width) {
        return false;
    }
    if (a.webcam.height != b.webcam.height) {
        return false;
    }
    if (a.webcam.fps != b.webcam.fps) {
        return false;
    }
    if (a.webcam.mirror != b.webcam.mirror) {
        return false;
    }
    if (a.webcam.aspect_ratio_locked != b.webcam.aspect_ratio_locked) {
        return false;
    }
    if (a.webcam.overlay_user_placed != b.webcam.overlay_user_placed) {
        return false;
    }

    // PiP overlay — float tolerance
    if (std::abs(a.webcam.overlay.x_norm - b.webcam.overlay.x_norm) > kPipTol) {
        return false;
    }
    if (std::abs(a.webcam.overlay.y_norm - b.webcam.overlay.y_norm) > kPipTol) {
        return false;
    }
    if (std::abs(a.webcam.overlay.w_norm - b.webcam.overlay.w_norm) > kPipTol) {
        return false;
    }
    if (std::abs(a.webcam.overlay.h_norm - b.webcam.overlay.h_norm) > kPipTol) {
        return false;
    }

    // Chroma key
    if (a.webcam.chroma_key.enabled != b.webcam.chroma_key.enabled) {
        return false;
    }
    if (a.webcam.chroma_key.color_mode != b.webcam.chroma_key.color_mode) {
        return false;
    }
    if (a.webcam.chroma_key.custom_r != b.webcam.chroma_key.custom_r) {
        return false;
    }
    if (a.webcam.chroma_key.custom_g != b.webcam.chroma_key.custom_g) {
        return false;
    }
    if (a.webcam.chroma_key.custom_b != b.webcam.chroma_key.custom_b) {
        return false;
    }
    if (std::abs(a.webcam.chroma_key.tolerance - b.webcam.chroma_key.tolerance) > kChromaTol) {
        return false;
    }
    if (std::abs(a.webcam.chroma_key.softness - b.webcam.chroma_key.softness) > kChromaTol) {
        return false;
    }
    if (std::abs(a.webcam.chroma_key.spill_reduction - b.webcam.chroma_key.spill_reduction) > kChromaTol) {
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Filename token helpers (moved from RecordingProfile.cpp)
// ---------------------------------------------------------------------------

std::wstring ContainerToken(capability::Container container) {
    switch (container) {
    case capability::Container::Matroska:
        return L"mkv";
    case capability::Container::Mp4:
        return L"mp4";
    case capability::Container::WebM:
        return L"webm";
    }
    return L"mkv";
}

std::wstring CodecToken(capability::VideoCodec codec) {
    switch (codec) {
    case capability::VideoCodec::H264Nvenc:
        return L"h264";
    case capability::VideoCodec::HevcNvenc:
        return L"hevc";
    case capability::VideoCodec::Av1Nvenc:
        return L"av1";
    }
    return L"h264";
}

std::wstring CodecToken(capability::AudioCodec codec) {
    switch (codec) {
    case capability::AudioCodec::AacMf:
        return L"aac";
    case capability::AudioCodec::Opus:
        return L"opus";
    case capability::AudioCodec::Pcm:
        return L"pcm";
    }
    return L"aac";
}

} // namespace exosnap
