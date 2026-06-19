#include <gtest/gtest.h>

#include <capability/capability_set.h>
#include <recorder_core/codec_types.h>

#include "models/RecordingPreset.h"
#include "models/VideoSettingsModel.h"

// Tests for the 0.5.0 video rate-control slice (ADR 0009):
//  - VideoSettingsModel defaults
//  - Preset round-trip / dirty-tracking for rate_control + bitrate_kbps
//  - SanitizePresetConfig: bitrate clamping, Lossless → CQ fallback
//  - CapabilitySet::QueryRateControlMode: CQ/VBR/CBR available; Lossless NotImplemented

namespace exosnap {

// ===========================================================================
// VideoSettingsModel — defaults must be unchanged
// ===========================================================================

TEST(VideoSettingsModel, Defaults_QualityIsBalanced) {
    const VideoSettingsModel m = VideoSettingsModel::Defaults();
    EXPECT_EQ(m.quality, recorder_core::NvencQualityPreset::Balanced);
}

TEST(VideoSettingsModel, Defaults_RateControlIsConstantQuality) {
    const VideoSettingsModel m = VideoSettingsModel::Defaults();
    EXPECT_EQ(m.rate_control, recorder_core::RateControlMode::ConstantQuality);
}

TEST(VideoSettingsModel, Defaults_BitrateIs20000) {
    const VideoSettingsModel m = VideoSettingsModel::Defaults();
    EXPECT_EQ(m.bitrate_kbps, 20000u);
}

TEST(VideoSettingsModel, Defaults_OtherFieldsUnchanged) {
    const VideoSettingsModel m = VideoSettingsModel::Defaults();
    EXPECT_TRUE(m.cfr);
    EXPECT_TRUE(m.capture_cursor);
    EXPECT_EQ(m.frame_rate_num, 60u);
    EXPECT_EQ(m.frame_rate_den, 1u);
}

// ===========================================================================
// SanitizePresetConfig — bitrate clamping
// ===========================================================================

TEST(SanitizePresetConfig, BitrateBelowMinIsClamped) {
    auto cfg = MakeDefaultPreset().config;
    cfg.video.rate_control = recorder_core::RateControlMode::VariableBitrate;
    cfg.video.bitrate_kbps = 0u;
    const auto result = SanitizePresetConfig(cfg);
    EXPECT_GE(result.video.bitrate_kbps, 1000u);
}

TEST(SanitizePresetConfig, BitrateAboveMaxIsClamped) {
    auto cfg = MakeDefaultPreset().config;
    cfg.video.rate_control = recorder_core::RateControlMode::ConstantBitrate;
    cfg.video.bitrate_kbps = 999999u;
    const auto result = SanitizePresetConfig(cfg);
    EXPECT_LE(result.video.bitrate_kbps, 200000u);
}

TEST(SanitizePresetConfig, BitrateInRangeIsPreserved) {
    auto cfg = MakeDefaultPreset().config;
    cfg.video.bitrate_kbps = 50000u;
    const auto result = SanitizePresetConfig(cfg);
    EXPECT_EQ(result.video.bitrate_kbps, 50000u);
}

TEST(SanitizePresetConfig, LosslessModeFallsBackToConstantQuality) {
    auto cfg = MakeDefaultPreset().config;
    cfg.video.rate_control = recorder_core::RateControlMode::Lossless;
    const auto result = SanitizePresetConfig(cfg);
    EXPECT_EQ(result.video.rate_control, recorder_core::RateControlMode::ConstantQuality);
}

TEST(SanitizePresetConfig, DefaultPresetRateControlIsPreserved) {
    // Default preset must remain ConstantQuality (no behavior change for existing users).
    const auto cfg = SanitizePresetConfig(MakeDefaultPreset().config);
    EXPECT_EQ(cfg.video.rate_control, recorder_core::RateControlMode::ConstantQuality);
}

// ===========================================================================
// NormalizedConfigEquals — rate_control + bitrate_kbps included
// ===========================================================================

TEST(NormalizedConfigEquals, DifferentRateControlNotEqual) {
    auto a = MakeDefaultPreset().config;
    auto b = a;
    b.video.rate_control = recorder_core::RateControlMode::VariableBitrate;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(NormalizedConfigEquals, DifferentBitrateNotEqual) {
    auto a = MakeDefaultPreset().config;
    auto b = a;
    b.video.bitrate_kbps = 50000u;
    EXPECT_FALSE(NormalizedConfigEquals(a, b));
}

TEST(NormalizedConfigEquals, SameRateControlAndBitrateEqual) {
    auto a = MakeDefaultPreset().config;
    auto b = a;
    EXPECT_TRUE(NormalizedConfigEquals(a, b));
}

// ===========================================================================
// ConfigDirtyEquivalent — rate_control + bitrate_kbps included in dirty check
// ===========================================================================

TEST(ConfigDirtyEquivalent, DifferentRateControlIsDirty) {
    auto a = MakeDefaultPreset().config;
    auto b = a;
    b.video.rate_control = recorder_core::RateControlMode::ConstantBitrate;
    EXPECT_FALSE(ConfigDirtyEquivalent(a, b));
}

TEST(ConfigDirtyEquivalent, DifferentBitrateIsDirty) {
    auto a = MakeDefaultPreset().config;
    auto b = a;
    b.video.bitrate_kbps = 80000u;
    EXPECT_FALSE(ConfigDirtyEquivalent(a, b));
}

// ===========================================================================
// CapabilitySet::QueryRateControlMode
// ===========================================================================

namespace {
// Minimal CapabilitySet sufficient for QueryRateControlMode (no map entries needed;
// the method uses a switch on the enum, not the maps).
capability::CapabilitySet MakeMinimalCapSet() {
    return {};
}
} // namespace

TEST(CapabilitySetRateControl, ConstantQuality_IsAvailable) {
    const auto caps = MakeMinimalCapSet();
    const auto ann = caps.QueryRateControlMode(recorder_core::RateControlMode::ConstantQuality);
    EXPECT_EQ(ann.level, capability::SupportLevel::Available);
}

TEST(CapabilitySetRateControl, VariableBitrate_IsAvailable) {
    const auto caps = MakeMinimalCapSet();
    const auto ann = caps.QueryRateControlMode(recorder_core::RateControlMode::VariableBitrate);
    EXPECT_EQ(ann.level, capability::SupportLevel::Available);
}

TEST(CapabilitySetRateControl, ConstantBitrate_IsAvailable) {
    const auto caps = MakeMinimalCapSet();
    const auto ann = caps.QueryRateControlMode(recorder_core::RateControlMode::ConstantBitrate);
    EXPECT_EQ(ann.level, capability::SupportLevel::Available);
}

TEST(CapabilitySetRateControl, Lossless_IsNotImplemented) {
    const auto caps = MakeMinimalCapSet();
    const auto ann = caps.QueryRateControlMode(recorder_core::RateControlMode::Lossless);
    EXPECT_EQ(ann.level, capability::SupportLevel::NotImplemented);
}

TEST(CapabilitySetRateControl, Lossless_IsNotSelectable) {
    const auto caps = MakeMinimalCapSet();
    const auto ann = caps.QueryRateControlMode(recorder_core::RateControlMode::Lossless);
    EXPECT_FALSE(capability::IsSelectable(ann.level));
}

} // namespace exosnap
