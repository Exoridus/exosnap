#include "RecordingPreset.h"

#include <capability/container_compat_registry.h>
#include <capability/resolver.h>
#include <exosnap/engine/audio_track_model.h>
#include <exosnap/engine/codec_types.h>

#include <algorithm>
#include <cctype>
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
[[nodiscard]] bool AudioTrackPlansEqual(const exosnap::engine::AudioTrackPlan& a,
                                        const exosnap::engine::AudioTrackPlan& b) noexcept {
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
[[nodiscard]] std::set<exosnap::engine::AudioSourceKind>
EnabledSourceKinds(const std::vector<exosnap::engine::AudioSourceRow>& rows) {
    std::set<exosnap::engine::AudioSourceKind> result;
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
    preset.config.capture.display_id = StableDisplayId{};
    preset.config.capture.window_key = "";
    preset.config.capture.has_region = false;
    preset.config.capture.region_display_id = StableDisplayId{};
    preset.config.capture.region_x_norm = 0.0f;
    preset.config.capture.region_y_norm = 0.0f;
    preset.config.capture.region_w_norm = 0.0f;
    preset.config.capture.region_h_norm = 0.0f;

    // Output — start from Defaults() then override codecs.
    // MKV + AV1 + Opus is a valid combination; do NOT call ReconcileContainerCodecs.
    preset.config.output = OutputSettingsModel::Defaults();
    preset.config.output.container = capability::Container::Matroska;
    preset.config.output.video_codec = capability::VideoCodec::Av1;
    preset.config.output.audio_codec = capability::AudioCodec::Opus;

    // Video
    preset.config.video.cq = exosnap::engine::CanonicalCq(exosnap::engine::QualityPreset::High);
    preset.config.video.cfr = true;
    preset.config.video.frame_pacing = exosnap::engine::FramePacingMode::Smooth;
    preset.config.video.capture_cursor = true;
    preset.config.video.frame_rate_num = 60;
    preset.config.video.frame_rate_den = 1;

    // Audio
    preset.config.audio.target_kind = capability::CaptureTargetKind::Display;
    preset.config.audio.source_rows = {
        {exosnap::engine::AudioSourceKind::SystemOutput, true, false},
        {exosnap::engine::AudioSourceKind::Mic, false, false},
    };
    preset.config.audio.mic_channel_mode = exosnap::engine::MicChannelMode::Auto;
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
// MakeBuiltInPresets
// ---------------------------------------------------------------------------

std::vector<RecordingPreset> MakeBuiltInPresets() {
    std::vector<RecordingPreset> result;
    result.push_back(MakeDefaultPreset());

    // Quality: the top of the ladder. Deliberately still P4 — measured on real
    // 1440p60 gameplay and a real browser scroll, P6 and P7 move VMAF by 0.01 at
    // the Ultra tier while spending 2-3% more bitrate and 1.7-2.3x the encode
    // time. Under constant QP the NVENC preset is an encode-time control, not a
    // quality one; the quantizer is what makes this preset better.
    RecordingPreset quality = MakeDefaultPreset();
    quality.id = std::string(kQualityPresetId);
    quality.name = "Quality";
    quality.config.video.cq = exosnap::engine::CanonicalCq(exosnap::engine::QualityPreset::Ultra);
    result.push_back(std::move(quality));

    // Compact: the smallest files that still read, for long screen recordings.
    // P6 is the one place a high NVENC preset earns its GPU time: on dense small
    // text it cuts 19% of the bitrate at slightly better quality, because the
    // sub-pixel motion search is what that content needs. On gameplay it costs
    // about 4% instead, which is the trade this preset exists to make. P7 adds a
    // further 3% at 38% more encode time and is not worth it.
    RecordingPreset compact = MakeDefaultPreset();
    compact.id = std::string(kCompactPresetId);
    compact.name = "Compact";
    compact.config.video.cq = exosnap::engine::CanonicalCq(exosnap::engine::QualityPreset::Low);
    compact.config.output.nvenc_preset = exosnap::engine::NvencPreset::P6;
    result.push_back(std::move(compact));

    // Performance: maximum encoder headroom at the default quality tier. P2
    // encodes a 1440p frame in under 0.6 ms against P4's 1.45 ms, for 0.24 VMAF
    // on gameplay and 0.10 on screen content, and produces a marginally SMALLER
    // file. For 1440p120 or a busy GPU that is the right end of the curve.
    RecordingPreset performance = MakeDefaultPreset();
    performance.id = std::string(kPerformancePresetId);
    performance.name = "Performance";
    performance.config.output.nvenc_preset = exosnap::engine::NvencPreset::P2;
    result.push_back(std::move(performance));

    // Compatibility: editing, upload, GPUs without AV1 encode (pre-RTX-40). It
    // keeps the same canonical CQ as Default, which is now a calibrated
    // statement rather than a coincidence: the canonical scale IS H.264's
    // quantizer scale, so CQ 19 here is the High tier for H.264 by definition.
    RecordingPreset compatibility = MakeDefaultPreset();
    compatibility.id = std::string(kCompatibilityPresetId);
    compatibility.name = "Compatibility";
    compatibility.config.output.container = capability::Container::Mp4;
    compatibility.config.output.video_codec = capability::VideoCodec::H264;
    compatibility.config.output.audio_codec = capability::AudioCodec::Aac;
    result.push_back(std::move(compatibility));

    return result;
}

bool IsBuiltInPresetId(std::string_view id) {
    return id == kDefaultPresetId || id == kQualityPresetId || id == kCompactPresetId || id == kPerformancePresetId ||
           id == kCompatibilityPresetId;
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

    // Output format: the resolver owns every static reconciliation rule
    // (container × codec per ADR 0010, the 10-bit demotion per ADR 0032, the
    // 4:4:4 chroma snap, and the MP4 CFR timing constraint). Copy its answer
    // instead of keeping a second rule set that could drift.
    {
        const capability::OutputFormatReconciliation reconciled = capability::ReconcileOutputFormat(
            {config.output.container, config.output.video_codec, config.output.audio_codec, config.output.bit_depth,
             config.output.chroma_subsampling, config.video.cfr});
        config.output.container = reconciled.resolved.container;
        config.output.video_codec = reconciled.resolved.video_codec;
        config.output.audio_codec = reconciled.resolved.audio_codec;
        config.output.bit_depth = reconciled.resolved.bit_depth;
        config.output.chroma_subsampling = reconciled.resolved.chroma;
        config.video.cfr = reconciled.resolved.cfr;
    }
    SanitizeOutputResolution(config.output.resolution);
    SanitizeSplitSettings(config.output.split);

    // Video: reset frame rate if degenerate (either numerator or denominator is zero).
    if (config.video.frame_rate_num == 0 || config.video.frame_rate_den == 0) {
        config.video.frame_rate_num = 60;
        config.video.frame_rate_den = 1;
    }
    {
        // Free frame rate (Expert entry): integer fps 1–240, den always 1. The
        // Default combo offers 15/30/60 and displays the nearest for any other
        // stored value; validity is intentionally wider than the list.
        const bool fps_valid =
            config.video.frame_rate_den == 1 && config.video.frame_rate_num >= 1 && config.video.frame_rate_num <= 240;
        if (!fps_valid) {
            config.video.frame_rate_num = 60;
            config.video.frame_rate_den = 1;
        }
    }
    // Video: frame_pacing (ADR 0035) — clamp unknown integer values to Smooth.
    {
        const int fp = static_cast<int>(config.video.frame_pacing);
        if (fp < 0 || fp > 1) {
            config.video.frame_pacing = exosnap::engine::FramePacingMode::Smooth;
        }
    }
    // Video: rate_control — default to ConstantQuality if an unknown value slips through.
    using RC = exosnap::engine::RateControlMode;
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

    // Audio: the App row's enabled/merge configuration is a persisted setting like any
    // other and survives every capture target, including Display/Region. Only its ACTIVE
    // state (receded vs. live) follows the target, and that derivation belongs to
    // PresentationStateBuilder — sanitize no longer strips or rewrites source rows here.
    // The actual recording-time audio plan still normalizes away the App row for a
    // non-Window target (exosnap::engine::NormalizeSourceRowsForTarget, via BuildAudioPlan),
    // since a display/region capture genuinely has no process to scope it to.

    // Audio: the first source row has no row above it, so a stored "merge with above"
    // there is meaningless state. Normalize it away rather than leave the resolver to
    // guess (pre-1.0: no compatibility duty for already-stored presets).
    if (!config.audio.source_rows.empty()) {
        config.audio.source_rows.front().merge_with_above = false;
    }

    // Audio: ensure mic_gain_linear is finite and strictly positive.
    if (!std::isfinite(config.audio.mic_gain_linear) || config.audio.mic_gain_linear <= 0.0f) {
        config.audio.mic_gain_linear = 1.0f;
    }

    // Audio v2 (0.6.0): clamp per-row gain_db to [kMinGainDb, kMaxGainDb]; reset NaN/Inf.
    for (auto& row : config.audio.source_rows) {
        if (!std::isfinite(row.gain_db)) {
            row.gain_db = 0.0f;
        } else if (row.gain_db < exosnap::engine::kMinGainDb) {
            row.gain_db = exosnap::engine::kMinGainDb;
        } else if (row.gain_db > exosnap::engine::kMaxGainDb) {
            row.gain_db = exosnap::engine::kMaxGainDb;
        }
        // muted is bool — no sanitization needed.
    }

    // Audio encoding params (ADR 0019):
    // audio_bitrate_kbps: 0 is valid (auto default); non-zero clamped to the
    // broadest safe range (Opus's, the wider of the two -- see codec_types.h).
    // Codec-specific clamping (kOpusBitrateKbpsMin/Max vs. kAacBitrateKbpsMin/Max)
    // happens in the engine/UI.
    if (config.audio.audio_bitrate_kbps > exosnap::engine::kOpusBitrateKbpsMax) {
        config.audio.audio_bitrate_kbps = exosnap::engine::kOpusBitrateKbpsMax;
    }
    // opus_complexity: clamp to [0, 10].
    if (config.audio.opus_complexity < 0) {
        config.audio.opus_complexity = 0;
    } else if (config.audio.opus_complexity > 10) {
        config.audio.opus_complexity = 10;
    }
    // opus_frame_duration: reset unknown values to the default (20 ms).
    {
        using D = exosnap::engine::OpusFrameDuration;
        const int d = static_cast<int>(config.audio.opus_frame_duration);
        if (d != static_cast<int>(D::Ms20) && d != static_cast<int>(D::Ms10) && d != static_cast<int>(D::Ms5) &&
            d != static_cast<int>(D::Ms2_5)) {
            config.audio.opus_frame_duration = D::Ms20;
        }
    }

    // Channel / sample-format model (ADR 0030 — 0.6.0):
    //   audio_channels: must be 1 or 2; else default to 2.
    //   audio_sample_rate: vetted set {44100, 48000, 96000}; else default to 48000.
    //     Opus locks to 48000 regardless of stored value.
    //   audio_bit_depth: gated by codec.
    //     Lossy (Opus/AAC): field is ignored; normalize to 16.
    //     PCM: {16, 24, 32}; else default to 16.
    //     FLAC: {16, 24}; else default to 16.
    //   flac_compression_level: clamp [0, 8].
    {
        if (config.audio.audio_channels != 1u && config.audio.audio_channels != 2u) {
            config.audio.audio_channels = 2u;
        }

        constexpr std::array<uint32_t, 3> kValidRates = {44100u, 48000u, 96000u};
        const bool rate_ok =
            std::find(kValidRates.begin(), kValidRates.end(), config.audio.audio_sample_rate) != kValidRates.end();
        if (!rate_ok) {
            config.audio.audio_sample_rate = 48000u;
        }
        // Opus is 48 kHz-only.
        if (config.output.audio_codec == capability::AudioCodec::Opus) {
            config.audio.audio_sample_rate = 48000u;
        }

        const auto codec = config.output.audio_codec;
        if (codec == capability::AudioCodec::Pcm) {
            constexpr std::array<uint32_t, 3> kPcmDepths = {16u, 24u, 32u};
            const bool depth_ok =
                std::find(kPcmDepths.begin(), kPcmDepths.end(), config.audio.audio_bit_depth) != kPcmDepths.end();
            if (!depth_ok) {
                config.audio.audio_bit_depth = 16u;
            }
        } else if (codec == capability::AudioCodec::Flac) {
            constexpr std::array<uint32_t, 2> kFlacDepths = {16u, 24u};
            const bool depth_ok =
                std::find(kFlacDepths.begin(), kFlacDepths.end(), config.audio.audio_bit_depth) != kFlacDepths.end();
            if (!depth_ok) {
                config.audio.audio_bit_depth = 16u;
            }
        } else {
            // Lossy (Opus / AAC): bit depth is internal to the codec; normalize to 16.
            config.audio.audio_bit_depth = 16u;
        }

        // audio_pcm_float: Pcm-only and requires audio_bit_depth == 32. An
        // inconsistent stored combination (hand-edited preset, or an older
        // schema regression) is silently narrowed back to false rather than
        // rejected -- same "narrow to a safe default" pattern as the bit-depth
        // clamp above.
        if (config.audio.audio_pcm_float &&
            (codec != capability::AudioCodec::Pcm || config.audio.audio_bit_depth != 32u)) {
            config.audio.audio_pcm_float = false;
        }

        if (config.audio.flac_compression_level < 0) {
            config.audio.flac_compression_level = 0;
        } else if (config.audio.flac_compression_level > 8) {
            config.audio.flac_compression_level = 8;
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

    // Microphone noise gate (Audio v2 — 0.6.0): threshold in dBFS. Reset NaN/Inf
    // to -45 dB; clamp to the usable [-80, 0] dB range.
    if (!std::isfinite(config.audio.mic_gate_threshold_db)) {
        config.audio.mic_gate_threshold_db = -45.0f;
    } else if (config.audio.mic_gate_threshold_db < -80.0f) {
        config.audio.mic_gate_threshold_db = -80.0f;
    } else if (config.audio.mic_gate_threshold_db > 0.0f) {
        config.audio.mic_gate_threshold_db = 0.0f;
    }

    // Microphone automatic gain control (Audio v2 — 0.6.0): target loudness in
    // dBFS. Reset NaN/Inf to -18 dB; clamp to the usable [-40, 0] dB range.
    if (!std::isfinite(config.audio.mic_agc_target_db)) {
        config.audio.mic_agc_target_db = -18.0f;
    } else if (config.audio.mic_agc_target_db < -40.0f) {
        config.audio.mic_agc_target_db = -40.0f;
    } else if (config.audio.mic_agc_target_db > 0.0f) {
        config.audio.mic_agc_target_db = 0.0f;
    }

    // Webcam: delegate to the provided sanitizer (handles NaN/Inf + clamping).
    config.webcam = SanitizeWebcamSettings(config.webcam);

    // Capture: if kind is Region but the normalized region has no area, clear it.
    if (config.capture.kind == PresetCaptureKind::Region &&
        (config.capture.region_w_norm <= 0.0f || config.capture.region_h_norm <= 0.0f)) {
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

std::string FoldPresetName(std::string_view name) {
    std::string folded = NormalizePresetName(name);
    std::transform(folded.begin(), folded.end(), folded.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return folded;
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
    if (!(a.capture.display_id == b.capture.display_id)) {
        return false;
    }
    if (a.capture.window_key != b.capture.window_key) {
        return false;
    }
    if (!(a.capture.region_display_id == b.capture.region_display_id)) {
        return false;
    }
    if (a.capture.has_region != b.capture.has_region) {
        return false;
    }
    if (a.capture.has_region) {
        constexpr float kNormTol = 1e-4f;
        if (std::abs(a.capture.region_x_norm - b.capture.region_x_norm) > kNormTol ||
            std::abs(a.capture.region_y_norm - b.capture.region_y_norm) > kNormTol ||
            std::abs(a.capture.region_w_norm - b.capture.region_w_norm) > kNormTol ||
            std::abs(a.capture.region_h_norm - b.capture.region_h_norm) > kNormTol) {
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
    if (a.output.bit_depth != b.output.bit_depth) {
        return false;
    }
    if (a.output.chroma_subsampling != b.output.chroma_subsampling) {
        return false;
    }
    if (a.output.color_range != b.output.color_range) {
        return false;
    }
    if (a.output.nvenc_preset != b.output.nvenc_preset) {
        return false;
    }
    if (a.output.hdr_mode != b.output.hdr_mode) {
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
    if (a.video.cq != b.video.cq) {
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
    if (a.video.frame_pacing != b.video.frame_pacing) {
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
    // A/V clock slaving (H-3): enabled (exact).
    if (a.audio.clock_slaving_enabled != b.audio.clock_slaving_enabled) {
        return false;
    }
    // Mic high-pass filter (Audio v2): enabled (exact) + cutoff (1e-2 Hz tolerance).
    if (a.audio.mic_hpf_enabled != b.audio.mic_hpf_enabled) {
        return false;
    }
    if (std::abs(a.audio.mic_hpf_cutoff_hz - b.audio.mic_hpf_cutoff_hz) > 1e-2f) {
        return false;
    }
    // Mic noise gate (Audio v2): enabled (exact) + threshold (1e-2 dB tolerance).
    if (a.audio.mic_gate_enabled != b.audio.mic_gate_enabled) {
        return false;
    }
    if (std::abs(a.audio.mic_gate_threshold_db - b.audio.mic_gate_threshold_db) > 1e-2f) {
        return false;
    }
    // Mic AGC (Audio v2): enabled (exact) + target (1e-2 dB tolerance).
    if (a.audio.mic_agc_enabled != b.audio.mic_agc_enabled) {
        return false;
    }
    if (std::abs(a.audio.mic_agc_target_db - b.audio.mic_agc_target_db) > 1e-2f) {
        return false;
    }
    // Mic RNNoise (Audio v2): enabled (exact); no numeric parameter.
    if (a.audio.mic_rnnoise_enabled != b.audio.mic_rnnoise_enabled) {
        return false;
    }
    // Channel / sample-format model (ADR 0030 — 0.6.0): exact integer comparisons.
    if (a.audio.audio_sample_rate != b.audio.audio_sample_rate) {
        return false;
    }
    if (a.audio.audio_channels != b.audio.audio_channels) {
        return false;
    }
    if (a.audio.audio_bit_depth != b.audio.audio_bit_depth) {
        return false;
    }
    if (a.audio.audio_pcm_float != b.audio.audio_pcm_float) {
        return false;
    }
    if (a.audio.flac_compression_level != b.audio.flac_compression_level) {
        return false;
    }

    // Semantic audio-row equality: same resolved plan AND same enabled-source set.
    {
        const exosnap::engine::AudioTrackPlan plan_a = exosnap::engine::ResolveAudioTracks(a.audio.source_rows);
        const exosnap::engine::AudioTrackPlan plan_b = exosnap::engine::ResolveAudioTracks(b.audio.source_rows);
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
    if (std::abs(a.webcam.opacity - b.webcam.opacity) > kPipTol) {
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
// ConfigDirtyDifference / ConfigDirtyEquivalent
// ---------------------------------------------------------------------------

bool ConfigDirtyEquivalent(const RecordingPresetConfig& a, const RecordingPresetConfig& b) {
    return ConfigDirtyDifference(a, b).empty();
}

std::string_view ConfigDirtyDifference(const RecordingPresetConfig& a, const RecordingPresetConfig& b) {
    // Capture identity (kind, display_id, window_key, has_region, region norms,
    // region_display_id), output.bit_depth, and output.hdr_mode are
    // intentionally NOT compared here: all three are environment fields
    // (see WithEnvironmentFields/StripEnvironmentFields) describing the
    // machine/display rather than the user's recording intent. Capture
    // depends on transient device availability and auto-resolution; bit depth
    // and HDR mode depend on the connected display and source. Comparing any
    // of them would cause spurious/unstable dirty state (e.g. default preset
    // appears dirty at startup because the live policy resolves an empty
    // display_id to a concrete monitor identity, or because a monitor is
    // replugged, or because the desktop's HDR toggle changes).
    // Per spec: temporary availability changes must not make the preset dirty.
    // NormalizedConfigEquals (full structural equality) is kept for persistence
    // round-trip verification and must NOT be changed.

    // Countdown
    if (a.countdown_seconds != b.countdown_seconds) {
        return "countdown_seconds";
    }

    // --- Output ---
    if (a.output.container != b.output.container) {
        return "output.container";
    }
    if (a.output.video_codec != b.output.video_codec) {
        return "output.video_codec";
    }
    if (a.output.chroma_subsampling != b.output.chroma_subsampling) {
        return "output.chroma_subsampling";
    }
    if (a.output.color_range != b.output.color_range) {
        return "output.color_range";
    }
    if (a.output.nvenc_preset != b.output.nvenc_preset) {
        return "output.nvenc_preset";
    }
    if (a.output.audio_codec != b.output.audio_codec) {
        return "output.audio_codec";
    }
    if (a.output.resolution.mode != b.output.resolution.mode) {
        return "output.resolution.mode";
    }
    if (a.output.resolution.custom_width != b.output.resolution.custom_width) {
        return "output.resolution.custom_width";
    }
    if (a.output.resolution.custom_height != b.output.resolution.custom_height) {
        return "output.resolution.custom_height";
    }
    if (a.output.resolution.fit != b.output.resolution.fit) {
        return "output.resolution.fit";
    }
    if (a.output.output_folder != b.output.output_folder) {
        return "output.output_folder";
    }
    if (a.output.naming_pattern != b.output.naming_pattern) {
        return "output.naming_pattern";
    }
    if (a.output.split != b.output.split) {
        return "output.split";
    }

    // --- Video ---
    if (a.video.cq != b.video.cq) {
        return "video.cq";
    }
    if (a.video.rate_control != b.video.rate_control) {
        return "video.rate_control";
    }
    if (a.video.bitrate_kbps != b.video.bitrate_kbps) {
        return "video.bitrate_kbps";
    }
    if (a.video.cfr != b.video.cfr) {
        return "video.cfr";
    }
    if (a.video.frame_pacing != b.video.frame_pacing) {
        return "video.frame_pacing";
    }
    if (a.video.capture_cursor != b.video.capture_cursor) {
        return "video.capture_cursor";
    }
    if (a.video.frame_rate_num != b.video.frame_rate_num) {
        return "video.frame_rate_num";
    }
    if (a.video.frame_rate_den != b.video.frame_rate_den) {
        return "video.frame_rate_den";
    }

    // --- Audio ---
    if (a.audio.target_kind != b.audio.target_kind) {
        return "audio.target_kind";
    }
    if (a.audio.mic_channel_mode != b.audio.mic_channel_mode) {
        return "audio.mic_channel_mode";
    }
    if (a.audio.selected_mic_device_id != b.audio.selected_mic_device_id) {
        return "audio.selected_mic_device_id";
    }
    if (a.audio.selected_window_pid != b.audio.selected_window_pid) {
        return "audio.selected_window_pid";
    }
    if (std::abs(a.audio.mic_gain_linear - b.audio.mic_gain_linear) > 1e-3f) {
        return "audio.mic_gain_linear";
    }

    // Audio encoding params (ADR 0019).
    if (a.audio.audio_bitrate_kbps != b.audio.audio_bitrate_kbps) {
        return "audio.audio_bitrate_kbps";
    }
    if (a.audio.opus_frame_duration != b.audio.opus_frame_duration) {
        return "audio.opus_frame_duration";
    }
    if (a.audio.opus_complexity != b.audio.opus_complexity) {
        return "audio.opus_complexity";
    }
    // Brickwall limiter (Audio v2): enabled (exact) + ceiling (1e-2 dB tolerance).
    if (a.audio.limiter_enabled != b.audio.limiter_enabled) {
        return "audio.limiter_enabled";
    }
    if (std::abs(a.audio.limiter_ceiling_db - b.audio.limiter_ceiling_db) > 1e-2f) {
        return "audio.limiter_ceiling_db";
    }
    // A/V clock slaving (H-3): enabled (exact).
    if (a.audio.clock_slaving_enabled != b.audio.clock_slaving_enabled) {
        return "audio.clock_slaving_enabled";
    }
    // Mic high-pass filter (Audio v2): enabled (exact) + cutoff (1e-2 Hz tolerance).
    if (a.audio.mic_hpf_enabled != b.audio.mic_hpf_enabled) {
        return "audio.mic_hpf_enabled";
    }
    if (std::abs(a.audio.mic_hpf_cutoff_hz - b.audio.mic_hpf_cutoff_hz) > 1e-2f) {
        return "audio.mic_hpf_cutoff_hz";
    }
    // Mic noise gate (Audio v2): enabled (exact) + threshold (1e-2 dB tolerance).
    if (a.audio.mic_gate_enabled != b.audio.mic_gate_enabled) {
        return "audio.mic_gate_enabled";
    }
    if (std::abs(a.audio.mic_gate_threshold_db - b.audio.mic_gate_threshold_db) > 1e-2f) {
        return "audio.mic_gate_threshold_db";
    }
    // Mic AGC (Audio v2): enabled (exact) + target (1e-2 dB tolerance).
    if (a.audio.mic_agc_enabled != b.audio.mic_agc_enabled) {
        return "audio.mic_agc_enabled";
    }
    if (std::abs(a.audio.mic_agc_target_db - b.audio.mic_agc_target_db) > 1e-2f) {
        return "audio.mic_agc_target_db";
    }
    // Mic RNNoise (Audio v2): enabled (exact); no numeric parameter.
    if (a.audio.mic_rnnoise_enabled != b.audio.mic_rnnoise_enabled) {
        return "audio.mic_rnnoise_enabled";
    }
    // Channel / sample-format model (ADR 0030 — 0.6.0): exact integer comparisons.
    if (a.audio.audio_sample_rate != b.audio.audio_sample_rate) {
        return "audio.audio_sample_rate";
    }
    if (a.audio.audio_channels != b.audio.audio_channels) {
        return "audio.audio_channels";
    }
    if (a.audio.audio_bit_depth != b.audio.audio_bit_depth) {
        return "audio.audio_bit_depth";
    }
    if (a.audio.audio_pcm_float != b.audio.audio_pcm_float) {
        return "audio.audio_pcm_float";
    }
    if (a.audio.flac_compression_level != b.audio.flac_compression_level) {
        return "audio.flac_compression_level";
    }

    // Semantic audio-row equality: same resolved plan AND same enabled-source set.
    {
        const exosnap::engine::AudioTrackPlan plan_a = exosnap::engine::ResolveAudioTracks(a.audio.source_rows);
        const exosnap::engine::AudioTrackPlan plan_b = exosnap::engine::ResolveAudioTracks(b.audio.source_rows);
        if (!AudioTrackPlansEqual(plan_a, plan_b)) {
            return "audio.source_rows (resolved track plan)";
        }
        if (EnabledSourceKinds(a.audio.source_rows) != EnabledSourceKinds(b.audio.source_rows)) {
            return "audio.source_rows (enabled kinds)";
        }
    }
    // Audio v2 (0.6.0): compare per-row gain_db and muted.
    {
        if (a.audio.source_rows.size() != b.audio.source_rows.size()) {
            return "audio.source_rows (row count)";
        }
        for (std::size_t i = 0; i < a.audio.source_rows.size(); ++i) {
            const auto& ra = a.audio.source_rows[i];
            const auto& rb = b.audio.source_rows[i];
            if (ra.muted != rb.muted) {
                return "audio.source_rows[].muted";
            }
            if (std::abs(ra.gain_db - rb.gain_db) > 1e-2f) {
                return "audio.source_rows[].gain_db";
            }
        }
    }

    // --- Webcam ---
    constexpr float kPipTol = 1e-3f;
    constexpr float kChromaTol = 1e-3f;

    if (a.webcam.enabled != b.webcam.enabled) {
        return "webcam.enabled";
    }
    if (a.webcam.device_id != b.webcam.device_id) {
        return "webcam.device_id";
    }
    if (a.webcam.width != b.webcam.width) {
        return "webcam.width";
    }
    if (a.webcam.height != b.webcam.height) {
        return "webcam.height";
    }
    if (a.webcam.fps != b.webcam.fps) {
        return "webcam.fps";
    }
    if (a.webcam.mirror != b.webcam.mirror) {
        return "webcam.mirror";
    }
    if (a.webcam.aspect_ratio_locked != b.webcam.aspect_ratio_locked) {
        return "webcam.aspect_ratio_locked";
    }
    if (a.webcam.overlay_user_placed != b.webcam.overlay_user_placed) {
        return "webcam.overlay_user_placed";
    }

    // PiP overlay — float tolerance
    if (std::abs(a.webcam.overlay.x_norm - b.webcam.overlay.x_norm) > kPipTol) {
        return "webcam.overlay.x_norm";
    }
    if (std::abs(a.webcam.overlay.y_norm - b.webcam.overlay.y_norm) > kPipTol) {
        return "webcam.overlay.y_norm";
    }
    if (std::abs(a.webcam.overlay.w_norm - b.webcam.overlay.w_norm) > kPipTol) {
        return "webcam.overlay.w_norm";
    }
    if (std::abs(a.webcam.overlay.h_norm - b.webcam.overlay.h_norm) > kPipTol) {
        return "webcam.overlay.h_norm";
    }
    if (std::abs(a.webcam.opacity - b.webcam.opacity) > kPipTol) {
        return "webcam.opacity";
    }

    // Chroma key
    if (a.webcam.chroma_key.enabled != b.webcam.chroma_key.enabled) {
        return "webcam.chroma_key.enabled";
    }
    if (a.webcam.chroma_key.color_mode != b.webcam.chroma_key.color_mode) {
        return "webcam.chroma_key.color_mode";
    }
    if (a.webcam.chroma_key.custom_r != b.webcam.chroma_key.custom_r) {
        return "webcam.chroma_key.custom_r";
    }
    if (a.webcam.chroma_key.custom_g != b.webcam.chroma_key.custom_g) {
        return "webcam.chroma_key.custom_g";
    }
    if (a.webcam.chroma_key.custom_b != b.webcam.chroma_key.custom_b) {
        return "webcam.chroma_key.custom_b";
    }
    if (std::abs(a.webcam.chroma_key.tolerance - b.webcam.chroma_key.tolerance) > kChromaTol) {
        return "webcam.chroma_key.tolerance";
    }
    if (std::abs(a.webcam.chroma_key.softness - b.webcam.chroma_key.softness) > kChromaTol) {
        return "webcam.chroma_key.softness";
    }
    if (std::abs(a.webcam.chroma_key.spill_reduction - b.webcam.chroma_key.spill_reduction) > kChromaTol) {
        return "webcam.chroma_key.spill_reduction";
    }

    return {};
}

// ---------------------------------------------------------------------------
// Environment fields
// ---------------------------------------------------------------------------

RecordingPresetConfig WithEnvironmentFields(RecordingPresetConfig config, const RecordingPresetConfig& env) {
    config.capture = env.capture;
    config.output.bit_depth = env.output.bit_depth;
    config.output.hdr_mode = env.output.hdr_mode;
    return config;
}

RecordingPresetConfig StripEnvironmentFields(RecordingPresetConfig config) {
    const OutputSettingsModel defaults = OutputSettingsModel::Defaults();
    config.capture = PresetCaptureTarget{};
    config.output.bit_depth = defaults.bit_depth;
    config.output.hdr_mode = defaults.hdr_mode;
    return config;
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
    case capability::VideoCodec::H264:
        return L"h264";
    case capability::VideoCodec::Hevc:
        return L"hevc";
    case capability::VideoCodec::Av1:
        return L"av1";
    }
    return L"h264";
}

std::wstring CodecToken(capability::AudioCodec codec) {
    switch (codec) {
    case capability::AudioCodec::Aac:
        return L"aac";
    case capability::AudioCodec::Opus:
        return L"opus";
    case capability::AudioCodec::Pcm:
        return L"pcm";
    case capability::AudioCodec::Flac:
        return L"flac";
    }
    return L"aac";
}

} // namespace exosnap
