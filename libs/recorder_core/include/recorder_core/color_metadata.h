#pragma once

#include <cstdint>

// Color-management foundation (v0.7.0, ADR 0032).
//
// Until this model existed the engine left the RGB->NV12 conversion to the
// D3D11 VideoProcessor's driver default (implementation-defined matrix/range)
// and wrote NO color description into the container, so players guessed. This
// model is the single source of truth for BOTH the encoder-input conversion
// (VideoProcessor color space) AND the container color tags, so they always
// agree. Values follow ISO/IEC 23001-8 (CICP / "Coding-independent code
// points"), the same code points Matroska and MP4 store.
//
// v0.7.0 ships the SDR BT.709 foundation. The HDR fields (PQ/HLG transfer,
// BT.2020 primaries, mastering display + content light level) are modeled here
// so the HDR slice only has to populate them — no further type churn.

namespace recorder_core {

// CICP color primaries (ISO/IEC 23001-8 Table 2).
enum class ColorPrimaries : uint8_t {
    Bt709 = 1,       // Rec. ITU-R BT.709-6 (HD/sRGB) — SDR default
    Unspecified = 2, //
    Bt2020 = 9,      // Rec. ITU-R BT.2020-2 — HDR/wide gamut (later slice)
};

// CICP transfer characteristics (ISO/IEC 23001-8 Table 3).
enum class TransferCharacteristics : uint8_t {
    Bt709 = 1,        // Rec. ITU-R BT.709-6 — SDR default
    Unspecified = 2,  //
    SmpteSt2084 = 16, // PQ — HDR10 (later slice)
    AribStdB67 = 18,  // HLG — (later slice)
};

// CICP matrix coefficients (ISO/IEC 23001-8 Table 4).
enum class MatrixCoefficients : uint8_t {
    Bt709 = 1,       // Y'CbCr from BT.709 primaries — SDR default
    Unspecified = 2, //
    Bt2020Ncl = 9,   // BT.2020 non-constant luminance (later slice)
};

// Y'CbCr signal range, in Matroska "Range" element semantics (not CICP):
// 0 = unspecified, 1 = broadcast/studio (16-235), 2 = full (0-255).
enum class ColorRange : uint8_t {
    Unspecified = 0,
    Limited = 1, // studio range — the SDR HD default and what we convert to
    Full = 2,
};

// Complete color description attached to the video track. The defaults describe
// SDR Rec.709 limited-range 8-bit — the implicit assumption ExoSnap produced
// before this model, now made explicit and deterministic.
struct ColorMetadata {
    ColorPrimaries primaries = ColorPrimaries::Bt709;
    TransferCharacteristics transfer = TransferCharacteristics::Bt709;
    MatrixCoefficients matrix = MatrixCoefficients::Bt709;
    ColorRange range = ColorRange::Limited;
    uint32_t bits_per_channel = 8;

    // True once any HDR field below is meaningful. SDR recordings leave this
    // false and the container omits the HDR sub-elements entirely.
    bool hdr = false;
    // HDR10 static metadata (only written when hdr == true). 0 means "absent".
    uint32_t max_content_light_level = 0;       // MaxCLL, cd/m^2
    uint32_t max_frame_average_light_level = 0; // MaxFALL, cd/m^2

    [[nodiscard]] static ColorMetadata Sdr709() noexcept {
        return ColorMetadata{};
    }
};

} // namespace recorder_core
