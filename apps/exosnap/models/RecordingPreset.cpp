#include "RecordingPreset.h"

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

    // Chroma key — default (disabled, green screen defaults)
    preset.config.webcam.chroma_key.enabled = false;
    preset.config.webcam.chroma_key.r = 0;
    preset.config.webcam.chroma_key.g = 177;
    preset.config.webcam.chroma_key.b = 64;
    preset.config.webcam.chroma_key.tolerance = 0.30f;
    preset.config.webcam.chroma_key.softness = 0.05f;

    // Countdown
    preset.config.countdown_seconds = 0;

    return preset;
}

// ---------------------------------------------------------------------------
// ReconcileContainerCodecs
// ---------------------------------------------------------------------------

void ReconcileContainerCodecs(OutputSettingsModel& output) {
    if (output.container == capability::Container::Mp4) {
        output.video_codec = capability::VideoCodec::H264Nvenc;
        output.audio_codec = capability::AudioCodec::AacMf;
    } else if (output.container == capability::Container::WebM) {
        output.video_codec = capability::VideoCodec::Av1Nvenc;
        output.audio_codec = capability::AudioCodec::Opus;
    } else if (output.container == capability::Container::Matroska) {
        if (output.video_codec == capability::VideoCodec::HevcNvenc) {
            output.video_codec = capability::VideoCodec::H264Nvenc;
        }
        if (output.video_codec == capability::VideoCodec::H264Nvenc &&
            output.audio_codec == capability::AudioCodec::Opus) {
            output.audio_codec = capability::AudioCodec::AacMf;
        }
        if (output.audio_codec == capability::AudioCodec::Pcm) {
            output.audio_codec = capability::AudioCodec::AacMf;
        }
    }
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

    // Output: reconcile codecs; leave output_folder as-is if empty.
    ReconcileContainerCodecs(config.output);

    // Video: reset frame rate if degenerate (either numerator or denominator is zero).
    if (config.video.frame_rate_num == 0 || config.video.frame_rate_den == 0) {
        config.video.frame_rate_num = 60;
        config.video.frame_rate_den = 1;
    }

    // Audio: ensure mic_gain_linear is finite and strictly positive.
    if (!std::isfinite(config.audio.mic_gain_linear) || config.audio.mic_gain_linear <= 0.0f) {
        config.audio.mic_gain_linear = 1.0f;
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
    if (a.output.output_folder != b.output.output_folder) {
        return false;
    }
    if (a.output.naming_pattern != b.output.naming_pattern) {
        return false;
    }

    // --- Video ---
    if (a.video.quality != b.video.quality) {
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
    if (a.webcam.chroma_key.r != b.webcam.chroma_key.r) {
        return false;
    }
    if (a.webcam.chroma_key.g != b.webcam.chroma_key.g) {
        return false;
    }
    if (a.webcam.chroma_key.b != b.webcam.chroma_key.b) {
        return false;
    }
    if (std::abs(a.webcam.chroma_key.tolerance - b.webcam.chroma_key.tolerance) > kChromaTol) {
        return false;
    }
    if (std::abs(a.webcam.chroma_key.softness - b.webcam.chroma_key.softness) > kChromaTol) {
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
    if (a.output.output_folder != b.output.output_folder) {
        return false;
    }
    if (a.output.naming_pattern != b.output.naming_pattern) {
        return false;
    }

    // --- Video ---
    if (a.video.quality != b.video.quality) {
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
    if (a.webcam.chroma_key.r != b.webcam.chroma_key.r) {
        return false;
    }
    if (a.webcam.chroma_key.g != b.webcam.chroma_key.g) {
        return false;
    }
    if (a.webcam.chroma_key.b != b.webcam.chroma_key.b) {
        return false;
    }
    if (std::abs(a.webcam.chroma_key.tolerance - b.webcam.chroma_key.tolerance) > kChromaTol) {
        return false;
    }
    if (std::abs(a.webcam.chroma_key.softness - b.webcam.chroma_key.softness) > kChromaTol) {
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
