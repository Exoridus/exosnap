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

namespace exosnap::engine {

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
    Bt601 = 6,       // Y'CbCr from BT.601/SMPTE-170M primaries (legacy SD; not
                     // produced by the encoder, but a recognized CICP value —
                     // the shared YUV->BGRA conversion helper supports it so
                     // callers reading foreign/legacy color metadata convert
                     // correctly too).
    Bt2020Ncl = 9,   // BT.2020 non-constant luminance (later slice)
};

// Y'CbCr signal range, in Matroska "Range" element semantics (not CICP):
// 0 = unspecified, 1 = broadcast/studio (16-235), 2 = full (0-255).
enum class ColorRange : uint8_t {
    Unspecified = 0,
    Limited = 1, // studio range (16-235) — broadcast standard; the SDR default
                 // (fix/color-range-signaling: player-compatibility, see ADR 0032)
    Full = 2,    // full range (0-255) — native screen precision; opt-in
};

// Complete color description attached to the video track. The defaults describe
// SDR Rec.709 limited-range 8-bit. Limited (16-235, broadcast/studio) is the
// default (fix/color-range-signaling): a controlled comparison showed common
// consumer players (VLC) ignore the range flag entirely and always apply
// limited->full expansion, so a Full-range recording is permanently
// crushed/dark there regardless of correct tagging — the same reason OBS and
// the rest of the consumer-video ecosystem encode limited by default. The
// range is user-selectable; Full (0-255, native screen precision) remains
// available as an opt-in for pipelines known to honour the range flag.
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

    // Mastering display metadata (SMPTE ST 2086), written into the Matroska
    // KaxVideoColourMasterMeta element. Independent of `hdr`/MaxCLL/MaxFALL
    // above — the container treats it as a separate optional child of
    // Colour — so presence is gated by its own explicit flag rather than by
    // 0-as-absent (a chromaticity coordinate of 0.0 is not a meaningful
    // "unset" sentinel the way MaxCLL/MaxFALL's 0 is).
    bool has_mastering_display = false;
    // Display primaries chromaticity, CIE 1931 xy, normalized to [0, 1].
    float mastering_display_primary_r_x = 0.0f;
    float mastering_display_primary_r_y = 0.0f;
    float mastering_display_primary_g_x = 0.0f;
    float mastering_display_primary_g_y = 0.0f;
    float mastering_display_primary_b_x = 0.0f;
    float mastering_display_primary_b_y = 0.0f;
    // White point chromaticity, CIE 1931 xy, normalized to [0, 1].
    float mastering_display_white_point_x = 0.0f;
    float mastering_display_white_point_y = 0.0f;
    // Mastering display luminance range, cd/m^2 (nits).
    float mastering_display_max_luminance = 0.0f;
    float mastering_display_min_luminance = 0.0f;

    // SDR Rec.709, limited-range 8-bit (the default). Callers that want full
    // range override range to ColorRange::Full after constructing this.
    [[nodiscard]] static ColorMetadata Sdr709() noexcept {
        return ColorMetadata{};
    }
};

} // namespace exosnap::engine
