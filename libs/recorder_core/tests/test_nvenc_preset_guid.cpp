#include "nvenc_encoder.h"

#include <gtest/gtest.h>

// Tests for NvencPresetToGuid — the pure, GPU-free mapping from the canonical
// NvencPreset (P1..P7) to the NVENC SDK preset GUID. Applies uniformly across
// codecs (H.264, HEVC, AV1); the mapping itself is codec-independent.

namespace recorder_core {

TEST(NvencPresetToGuid, P1MapsToNvEncPresetP1Guid) {
    EXPECT_TRUE(IsEqualGUID(NvencPresetToGuid(NvencPreset::P1), NV_ENC_PRESET_P1_GUID));
}

TEST(NvencPresetToGuid, P2MapsToNvEncPresetP2Guid) {
    EXPECT_TRUE(IsEqualGUID(NvencPresetToGuid(NvencPreset::P2), NV_ENC_PRESET_P2_GUID));
}

TEST(NvencPresetToGuid, P3MapsToNvEncPresetP3Guid) {
    EXPECT_TRUE(IsEqualGUID(NvencPresetToGuid(NvencPreset::P3), NV_ENC_PRESET_P3_GUID));
}

TEST(NvencPresetToGuid, P4MapsToNvEncPresetP4Guid) {
    EXPECT_TRUE(IsEqualGUID(NvencPresetToGuid(NvencPreset::P4), NV_ENC_PRESET_P4_GUID));
}

TEST(NvencPresetToGuid, P5MapsToNvEncPresetP5Guid) {
    EXPECT_TRUE(IsEqualGUID(NvencPresetToGuid(NvencPreset::P5), NV_ENC_PRESET_P5_GUID));
}

TEST(NvencPresetToGuid, P6MapsToNvEncPresetP6Guid) {
    EXPECT_TRUE(IsEqualGUID(NvencPresetToGuid(NvencPreset::P6), NV_ENC_PRESET_P6_GUID));
}

TEST(NvencPresetToGuid, P7MapsToNvEncPresetP7Guid) {
    EXPECT_TRUE(IsEqualGUID(NvencPresetToGuid(NvencPreset::P7), NV_ENC_PRESET_P7_GUID));
}

// Sanity: the seven mapped GUIDs must be pairwise distinct (a copy/paste bug in
// the switch could otherwise silently alias two presets to the same GUID).
TEST(NvencPresetToGuid, AllSevenGuidsAreDistinct) {
    const GUID guids[7] = {
        NvencPresetToGuid(NvencPreset::P1), NvencPresetToGuid(NvencPreset::P2), NvencPresetToGuid(NvencPreset::P3),
        NvencPresetToGuid(NvencPreset::P4), NvencPresetToGuid(NvencPreset::P5), NvencPresetToGuid(NvencPreset::P6),
        NvencPresetToGuid(NvencPreset::P7),
    };
    for (size_t i = 0; i < 7; ++i) {
        for (size_t j = i + 1; j < 7; ++j) {
            EXPECT_FALSE(IsEqualGUID(guids[i], guids[j]))
                << "preset " << i + 1 << " and " << j + 1 << " map to the same GUID";
        }
    }
}

} // namespace recorder_core
