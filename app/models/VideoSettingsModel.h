#pragma once

#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/frame_pacing.h>

namespace exosnap {

// How frequently the encoder inserts an IDR (keyframe).  Shorter intervals
// allow finer trim points in Quick Trim but slightly increase file size.
enum class KeyframeIntervalMode {
    Seconds2,   // 2 s — default; smaller GOP, slightly smaller files
    Seconds1,   // 1 s — 1-second trim grid
    Seconds0_5, // 0.5 s — finest trim accuracy, slightly larger files
};

struct VideoSettingsModel {
    // Constant-quality target (1 = best, 51 = worst). The three named presets are
    // shorthands for canonical values, not a separate setting: the selected
    // segment is derived via exosnap::engine::NearestQualityPreset(cq).
    uint32_t cq = exosnap::engine::CanonicalCq(exosnap::engine::QualityPreset::Balanced);
    exosnap::engine::RateControlMode rate_control = exosnap::engine::RateControlMode::ConstantQuality;
    uint32_t bitrate_kbps = 20000; // Used for VariableBitrate and ConstantBitrate modes
    bool cfr = true;
    exosnap::engine::FramePacingMode frame_pacing = exosnap::engine::FramePacingMode::Smooth;
    bool capture_cursor = true;
    uint32_t frame_rate_num = 60;
    uint32_t frame_rate_den = 1;
    KeyframeIntervalMode keyframe_interval = KeyframeIntervalMode::Seconds2; // default 2 s

    static VideoSettingsModel Defaults() {
        return {};
    }
};

} // namespace exosnap
