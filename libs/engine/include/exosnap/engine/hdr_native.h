#pragma once

#include "codec_types.h"
#include "color_metadata.h"

// Pure decision + metadata-assembly logic for the native HDR10 (PQ / BT.2020)
// output path. No D3D / no threads — unit-tested directly. The impure step
// (turning the selected capture target into its display's HDR facts) belongs to
// the capture backend / session-start layer, which then feeds these functions.

namespace exosnap::engine {

// HDR facts of the display being captured, read from DXGI_OUTPUT_DESC1. Mirrors
// the subset needed to signal HDR10 output; kept as a plain engine struct
// so this stays free of any capability/DXGI dependency.
struct HdrDisplayFacts {
    bool hdr_active = false; // display currently in an HDR (PQ/BT.2020) colour space
    // CIE 1931 xy chromaticity primaries + white point.
    float red_primary_x = 0.0f;
    float red_primary_y = 0.0f;
    float green_primary_x = 0.0f;
    float green_primary_y = 0.0f;
    float blue_primary_x = 0.0f;
    float blue_primary_y = 0.0f;
    float white_point_x = 0.0f;
    float white_point_y = 0.0f;
    // Display luminance range, cd/m^2.
    float max_luminance_nits = 0.0f;
    float min_luminance_nits = 0.0f;
    // The display's DISPLAYCONFIG_SDR_WHITE_LEVEL in nits at session start
    // (0 = unknown / query failed). Independent of hdr_active gating: it is the
    // OS SDR-content-brightness reference, not an EDID capability claim.
    float sdr_white_level_nits = 0.0f;
};

// Only HEVC and AV1 can carry an HDR10 (10-bit PQ/BT.2020) bitstream; H.264 is
// excluded (matches the pre-flight rec.hdr.h264 blocker and the 8-bit-only
// H.264 encode path).
[[nodiscard]] inline bool CodecSupportsHdr10Native(VideoCodec codec) noexcept {
    return codec == VideoCodec::Hevc || codec == VideoCodec::Av1;
}

// The native HDR10 output path engages only when the user asked for it
// (HdrMode::Hdr10), the captured display is actually in an HDR colour space, and
// the selected codec can encode HDR10. Any other combination keeps the existing
// behaviour (tone-map to SDR, or plain SDR) — so an SDR desktop, a display that
// is not HDR-active, or an H.264 selection never produces a PQ/BT.2020 stream.
[[nodiscard]] inline bool IsHdr10NativeEffective(HdrMode mode, bool display_hdr_active, VideoCodec codec) noexcept {
    return mode == HdrMode::Hdr10 && display_hdr_active && CodecSupportsHdr10Native(codec);
}

// Guard for a faulty caller: native HDR10 output (PQ/BT.2020) is a 10-bit-only
// format packed into P010. Engaging the native path with an 8-bit bit depth is a
// caller inconsistency — the encode ring would be NV12 and the PQ converter's
// render target creation would fail cryptically. True == that inconsistency.
[[nodiscard]] inline bool NativeHdr10BitDepthViolation(bool native_hdr_expected, BitDepth bit_depth) noexcept {
    return native_hdr_expected && bit_depth != BitDepth::Bit10;
}

// Assemble the HDR10 colour description for a native session from the captured
// display's HDR facts: BT.2020 primaries, PQ (SMPTE ST 2084) transfer, BT.2020
// non-constant-luminance matrix, limited range, 10-bit. Mastering-display
// metadata (SMPTE ST 2086) is filled from the display's reported primaries and
// luminance range — the display's capabilities are the usual approximation for
// the content's mastering values. MaxCLL/MaxFALL are deliberately left absent
// (no per-frame content-light analysis is performed; 0 = absent is legal).
//
// The facts must come from an HDR-active display (an SDR-mode display reports
// inflated EDID luminance caps that must not be trusted); callers gate this on
// facts.hdr_active via IsHdr10NativeEffective.
[[nodiscard]] inline ColorMetadata MakeHdr10ColorMetadata(const HdrDisplayFacts& facts) noexcept {
    ColorMetadata color;
    color.primaries = ColorPrimaries::Bt2020;
    color.transfer = TransferCharacteristics::SmpteSt2084;
    color.matrix = MatrixCoefficients::Bt2020Ncl;
    color.range = ColorRange::Limited; // HDR10 is a narrow-range format
    color.bits_per_channel = 10;       // HDR10 is 10-bit by definition
    color.hdr = true;
    // No MaxCLL/MaxFALL: absence is legal and no content light-level analysis
    // is done. They stay 0 (the writer omits them).

    // Mastering-display metadata is only meaningful with real chromaticity /
    // luminance readings; a degenerate (all-zero) report omits it rather than
    // writing a zeroed element.
    if (facts.white_point_x > 0.0f && facts.max_luminance_nits > 0.0f) {
        color.has_mastering_display = true;
        color.mastering_display_primary_r_x = facts.red_primary_x;
        color.mastering_display_primary_r_y = facts.red_primary_y;
        color.mastering_display_primary_g_x = facts.green_primary_x;
        color.mastering_display_primary_g_y = facts.green_primary_y;
        color.mastering_display_primary_b_x = facts.blue_primary_x;
        color.mastering_display_primary_b_y = facts.blue_primary_y;
        color.mastering_display_white_point_x = facts.white_point_x;
        color.mastering_display_white_point_y = facts.white_point_y;
        color.mastering_display_max_luminance = facts.max_luminance_nits;
        color.mastering_display_min_luminance = facts.min_luminance_nits;
    }
    return color;
}

} // namespace exosnap::engine
