#pragma once

#include "hdr_reference_white.h"
#include "hdr_tonemap.h"

#include <exosnap/engine/sdr_white_level.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

// Pure, GPU-independent evaluation of per-frame luminance measurements. The
// compute pass (gpu_frame_luminance.*) produces the raw numbers; everything that
// turns them into a tone-map knee or into HDR10 content-light metadata lives
// here, unit-tested and free of any D3D dependency.
//
// Two consumers with opposite requirements are fed from the same measurement,
// which is why the evaluation is split rather than shared:
//
//   * MaxCLL / MaxFALL want the exact extremes over the whole stream. They are
//     accumulated monotonically and never decay.
//   * The tone-map knee wants a short-term, smoothed value. An unsmoothed knee
//     that follows the content changes the picture's brightness at every scene
//     cut, which is more visible than the error it corrects.
//
// Luminance is carried in cd/m^2 throughout. That is the unit CTA-861.3 defines
// MaxCLL/MaxFALL in, and converting once at the measurement boundary keeps the
// two consumers from disagreeing about what a number means.

namespace exosnap::engine {

// Rec. 709 luminance weights. The capture surface is scRGB, whose primaries are
// BT.709, so this is the correct relative-luminance projection with no gamut
// conversion in between.
inline constexpr float kLuminanceWeightR = 0.2126f;
inline constexpr float kLuminanceWeightG = 0.7152f;
inline constexpr float kLuminanceWeightB = 0.0722f;

// Histogram resolution. Coarse on purpose: its only consumer is a percentile
// that feeds a smoothed knee, and 64 bins keep the per-threadgroup atomics and
// the readback buffer small enough to be free next to the reduction that shares
// the dispatch.
inline constexpr int kFrameLuminanceHistogramBins = 64;

// Histogram domain, in log2(cd/m^2). Luminance is perceived logarithmically and
// spans five orders of magnitude on an HDR desktop, so linear bins would collect
// every ordinary pixel in the first one and leave the percentile blind. The
// range covers the darkest level any panel resolves up to well beyond the
// brightest HDR10 mastering level (2^14 == 16384 cd/m^2).
inline constexpr float kFrameLuminanceHistogramMinLog2Nits = -7.0f;
inline constexpr float kFrameLuminanceHistogramMaxLog2Nits = 14.0f;

inline constexpr float kFrameLuminanceHistogramLog2Span =
    kFrameLuminanceHistogramMaxLog2Nits - kFrameLuminanceHistogramMinLog2Nits;
inline constexpr float kFrameLuminanceHistogramLog2Step =
    kFrameLuminanceHistogramLog2Span / static_cast<float>(kFrameLuminanceHistogramBins);

// Bin a luminance falls into. Matches the HLSL binning in gpu_frame_luminance.cpp
// exactly; the WARP test pins the two against each other. Values at or below the
// domain floor (including zero and clamped wide-gamut negatives) land in bin 0,
// values above the ceiling in the last bin.
[[nodiscard]] inline int FrameLuminanceHistogramBin(float nits) noexcept {
    if (!(nits > 0.0f)) {
        return 0;
    }
    const float l2 = std::log2(nits);
    const float t = (l2 - kFrameLuminanceHistogramMinLog2Nits) / kFrameLuminanceHistogramLog2Step;
    if (t <= 0.0f) {
        return 0;
    }
    const int bin = static_cast<int>(t);
    return bin < kFrameLuminanceHistogramBins ? bin : kFrameLuminanceHistogramBins - 1;
}

// Representative luminance of a bin: its geometric centre, which is the
// arithmetic centre in the log domain the bins are cut in. No interpolation
// between bins -- the percentile drives a value that is then smoothed over
// hundreds of milliseconds, so sub-bin precision would be discarded anyway.
[[nodiscard]] inline float FrameLuminanceHistogramBinCentreNits(int bin) noexcept {
    const float l2 =
        kFrameLuminanceHistogramMinLog2Nits + (static_cast<float>(bin) + 0.5f) * kFrameLuminanceHistogramLog2Step;
    return std::exp2(l2);
}

// One frame's measurement, as the compute pass delivers it.
struct FrameLuminanceStats {
    float min_nits = 0.0f;
    float max_nits = 0.0f;
    float mean_nits = 0.0f;
    std::array<uint32_t, kFrameLuminanceHistogramBins> histogram{};
};

// Luminance at the given rank of the distribution, in cd/m^2.
//
// `percentile` is a fraction in [0, 1]; 0.9999 is the conventional choice for a
// content peak because it rejects the handful of pixels a specular highlight or
// a stuck sub-pixel contributes without rejecting the highlight itself. Returns
// 0 for an empty histogram.
[[nodiscard]] inline float HistogramPercentileNits(std::span<const uint32_t> bins, float percentile) noexcept {
    uint64_t total = 0;
    for (const uint32_t count : bins) {
        total += count;
    }
    if (total == 0) {
        return 0.0f;
    }
    float p = percentile;
    if (!(p > 0.0f)) {
        p = 0.0f;
    }
    if (p > 1.0f) {
        p = 1.0f;
    }
    // Rank counted from the bright end so the requested fraction is the fraction
    // of pixels AT OR BELOW the answer, and rounding never selects a bin beyond
    // the brightest populated one. The cap at total - 1 is what keeps an empty
    // bin from ever being the answer: at the darkest populated bin every pixel
    // has been counted, so the walk terminates there whatever the rank was.
    uint64_t allowed_above = static_cast<uint64_t>(static_cast<double>(total) * static_cast<double>(1.0f - p));
    if (allowed_above > total - 1) {
        allowed_above = total - 1;
    }
    uint64_t above = 0;
    for (int bin = static_cast<int>(bins.size()) - 1; bin >= 0; --bin) {
        above += bins[static_cast<size_t>(bin)];
        if (above > allowed_above) {
            return FrameLuminanceHistogramBinCentreNits(bin);
        }
    }
    return FrameLuminanceHistogramBinCentreNits(0);
}

// Stream-wide content light levels (CTA-861.3), accumulated frame by frame.
//
// MaxCLL is the maximum over the per-frame maxima and MaxFALL the maximum over
// the per-frame frame-average levels, both over the entire encoded stream. They
// only ever rise, so a value is valid for the file the moment the last frame has
// been accumulated.
struct StreamLuminanceAccumulator {
    float max_cll_nits = 0.0f;
    float max_fall_nits = 0.0f;
    float min_cll_nits = 0.0f;
    uint64_t frames = 0;

    void Accumulate(const FrameLuminanceStats& frame) noexcept {
        if (std::isfinite(frame.max_nits) && frame.max_nits > max_cll_nits) {
            max_cll_nits = frame.max_nits;
        }
        if (std::isfinite(frame.mean_nits) && frame.mean_nits > max_fall_nits) {
            max_fall_nits = frame.mean_nits;
        }
        if (std::isfinite(frame.min_nits) && (frames == 0 || frame.min_nits < min_cll_nits)) {
            min_cll_nits = frame.min_nits;
        }
        ++frames;
    }

    // A device loss invalidates every measurement taken so far: the surfaces the
    // numbers came from belonged to a device that no longer exists, and letting
    // them stand would describe the recovered recording with a dead device's
    // content.
    void Reset() noexcept {
        *this = StreamLuminanceAccumulator{};
    }

    [[nodiscard]] bool HasData() const noexcept {
        return frames > 0;
    }
};

// CTA-861.3 codes MaxCLL and MaxFALL as unsigned 16-bit integers in cd/m^2, so
// the serialised value saturates rather than wrapping.
inline constexpr uint32_t kMaxContentLightLevelCeiling = 65535;

[[nodiscard]] inline uint32_t ContentLightLevelCode(float nits) noexcept {
    if (!std::isfinite(nits) || nits <= 0.0f) {
        return 0;
    }
    const float rounded = std::round(nits);
    if (rounded >= static_cast<float>(kMaxContentLightLevelCeiling)) {
        return kMaxContentLightLevelCeiling;
    }
    return static_cast<uint32_t>(rounded);
}

// Rank the tone-map knee is taken at. Not the exact maximum: a single specular
// pixel, a stuck sub-pixel or an overshooting wide-gamut sample would otherwise
// define the roll-off for the whole picture and darken everything below it. The
// same rank Microsoft's Direct2D HDR tone-mapping sample takes, and it discards
// at most 830 pixels of a 4K frame.
inline constexpr float kContentPeakPercentile = 0.9999f;

// Time constants of the content-peak smoother, as one-pole time constants.
//
// The asymmetry is the whole point. A highlight that appears must reach the knee
// before it has burned out, so the rise is fast enough to be over within a
// couple of frames at any supported frame rate. A highlight that leaves must not
// drag the knee down with it, because that is what makes the picture pump: the
// fall is slow enough that a cut back and forth between a bright and a dark
// scene is ridden out at the brighter setting.
inline constexpr float kContentPeakRiseTimeConstantSeconds = 0.06f;
inline constexpr float kContentPeakFallTimeConstantSeconds = 1.50f;

// Asymmetric one-pole smoother for the measured content peak.
//
// Holds cd/m^2. The first measurement is adopted outright rather than ramped to
// from zero: there is no previous state to preserve, and starting from zero
// would spend the first second of every recording at a knee below paper white.
class ContentPeakSmoother {
  public:
    void Reset() noexcept {
        smoothed_nits_ = 0.0f;
        has_value_ = false;
    }

    [[nodiscard]] bool HasValue() const noexcept {
        return has_value_;
    }

    [[nodiscard]] float ValueNits() const noexcept {
        return smoothed_nits_;
    }

    // Folds one measurement in and returns the new smoothed peak.
    // `dt_seconds` is the interval since the previous measurement; a
    // non-positive or non-finite interval adopts the measurement unsmoothed,
    // which is the only honest answer when no time is known to have passed.
    float Update(float measured_nits, float dt_seconds) noexcept {
        if (!std::isfinite(measured_nits) || measured_nits < 0.0f) {
            return smoothed_nits_;
        }
        if (!has_value_ || !std::isfinite(dt_seconds) || dt_seconds <= 0.0f) {
            smoothed_nits_ = measured_nits;
            has_value_ = true;
            return smoothed_nits_;
        }
        const float tau =
            measured_nits > smoothed_nits_ ? kContentPeakRiseTimeConstantSeconds : kContentPeakFallTimeConstantSeconds;
        const float alpha = 1.0f - std::exp(-dt_seconds / tau);
        smoothed_nits_ += alpha * (measured_nits - smoothed_nits_);
        return smoothed_nits_;
    }

  private:
    float smoothed_nits_ = 0.0f;
    bool has_value_ = false;
};

// The measured content peak expressed as a tone-map peak scale, in reference-
// white multiples, ready to replace HdrPeakScale's display-derived value.
//
// The lower bound is the rule HdrPeakScale applies to the display peak, applied
// to the measured one: the shader divides both the signal and this
// peak by the paper-white scale, so a peak at or below the white the OS composes
// SDR content at leaves the quotient under the knee and HdrToneMapChannel
// degenerates into a hard clamp -- every highlight collapses onto white. A dark
// scene legitimately measures a peak below paper white, so this is the ordinary
// case rather than an error case, and clamping there reproduces exactly the
// identity-like roll-off a scene with no highlights should get.
[[nodiscard]] inline float ContentPeakScale(float content_peak_nits, float sdr_white_level_nits) noexcept {
    const float paper_white_nits = EffectiveOverlayReferenceWhiteNits(sdr_white_level_nits);
    float peak_nits = content_peak_nits;
    if (!std::isfinite(peak_nits) || peak_nits < paper_white_nits) {
        peak_nits = paper_white_nits;
    }
    return peak_nits / kHdrReferenceWhiteNits; // >= 1.0
}

// Relative change in the peak scale below which the tone-map pass keeps the knee
// it already holds.
//
// The smoother produces a slightly different number on every frame it is fed, so
// without a band the knee would be rewritten at frame rate for changes far under
// what the roll-off can express: half a percent of the knee moves an 8-bit
// highlight code by well under one level. The band is on the knee rather than on
// the measurement so the threshold is stated in the quantity that is acted on.
inline constexpr float kContentPeakScaleUpdateFraction = 0.005f;

// Whether a newly resolved tone-map peak scale is worth handing to the tone-map
// pass.
//
// `applied` is the knee the pass currently holds, carried as a non-positive
// scale while it holds no measured one. The band is relative to it, so it is
// zero in that state and the first measurement always passes.
[[nodiscard]] inline bool ContentPeakScaleChangedMaterially(float applied, float candidate) noexcept {
    if (!std::isfinite(candidate)) {
        return false;
    }
    return std::fabs(candidate - applied) > applied * kContentPeakScaleUpdateFraction;
}

} // namespace exosnap::engine
