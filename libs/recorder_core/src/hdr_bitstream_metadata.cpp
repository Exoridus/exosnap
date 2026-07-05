#include "recorder_core/hdr_bitstream_metadata.h"

#include <algorithm>
#include <cmath>

namespace recorder_core::hdr_meta {

namespace {

// Round-half-away-from-zero to an integer, clamped into [0, hi]. Inputs are
// non-negative chromaticity / luminance values, so this is a plain round+clamp.
// Arithmetic is done in double so the result does not depend on float rounding
// of the intermediate product.
[[nodiscard]] uint64_t ScaleRoundClamp(double value, double scale, uint64_t hi) {
    if (!(value > 0.0)) {
        return 0u;
    }
    const double scaled = std::round(value * scale);
    if (scaled <= 0.0) {
        return 0u;
    }
    const double hi_d = static_cast<double>(hi);
    return (scaled >= hi_d) ? hi : static_cast<uint64_t>(scaled);
}

void AppendBe16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
}

constexpr uint64_t kU16Max = 0xFFFFu;
constexpr uint64_t kU32Max = 0xFFFFFFFFu;

// HEVC display_primaries / white point: 0.00002 units → coord * 50000, u(16).
uint16_t HevcChroma(float coord) {
    return static_cast<uint16_t>(ScaleRoundClamp(static_cast<double>(coord), 50000.0, kU16Max));
}

// HEVC display mastering luminance: 0.0001 cd/m^2 units → nits * 10000, u(32).
uint32_t HevcLuminance(float nits) {
    return static_cast<uint32_t>(ScaleRoundClamp(static_cast<double>(nits), 10000.0, kU32Max));
}

// AV1 chromaticity: 0.16 fixed-point → coord * 65536, f(16).
uint16_t Av1Chroma(float coord) {
    return static_cast<uint16_t>(ScaleRoundClamp(static_cast<double>(coord), 65536.0, kU16Max));
}

// AV1 luminance_max: 24.8 fixed-point → nits * 256, f(32).
uint32_t Av1LuminanceMax(float nits) {
    return static_cast<uint32_t>(ScaleRoundClamp(static_cast<double>(nits), 256.0, kU32Max));
}

// AV1 luminance_min: 18.14 fixed-point → nits * 16384, f(32).
uint32_t Av1LuminanceMin(float nits) {
    return static_cast<uint32_t>(ScaleRoundClamp(static_cast<double>(nits), 16384.0, kU32Max));
}

// Content light level fields are raw cd/m^2 integers, clamped to u(16). Shared
// by HEVC (D.2.35) and AV1 (6.7.3).
uint16_t Cll16(uint32_t v) {
    return static_cast<uint16_t>(std::min<uint32_t>(v, 0xFFFFu));
}

} // namespace

bool HasMasteringDisplayData(const ColorMetadata& color) noexcept {
    return color.has_mastering_display;
}

bool HasContentLightLevelData(const ColorMetadata& color) noexcept {
    return color.max_content_light_level > 0 || color.max_frame_average_light_level > 0;
}

bool ShouldEmitHdrBitstreamMetadata(const ColorMetadata& color) noexcept {
    // Gate strictly on HDR10-native signalling (PQ transfer + BT.2020 primaries)
    // with the hdr flag set. SDR and tone-map-SDR sessions leave hdr == false and
    // so emit nothing, keeping their bitstream byte-identical. Emit only if there
    // is at least one message worth of data present.
    const bool hdr10_native = color.hdr && color.transfer == TransferCharacteristics::SmpteSt2084 &&
                              color.primaries == ColorPrimaries::Bt2020;
    return hdr10_native && (HasMasteringDisplayData(color) || HasContentLightLevelData(color));
}

std::vector<uint8_t> BuildHevcMasteringDisplaySeiPayload(const ColorMetadata& color) {
    // H.265 D.2.28 mastering_display_colour_volume(): display_primaries in G,B,R
    // order (c = 0,1,2), then white point, then max/min luminance u(32).
    std::vector<uint8_t> out;
    out.reserve(24);
    AppendBe16(out, HevcChroma(color.mastering_display_primary_g_x));
    AppendBe16(out, HevcChroma(color.mastering_display_primary_g_y));
    AppendBe16(out, HevcChroma(color.mastering_display_primary_b_x));
    AppendBe16(out, HevcChroma(color.mastering_display_primary_b_y));
    AppendBe16(out, HevcChroma(color.mastering_display_primary_r_x));
    AppendBe16(out, HevcChroma(color.mastering_display_primary_r_y));
    AppendBe16(out, HevcChroma(color.mastering_display_white_point_x));
    AppendBe16(out, HevcChroma(color.mastering_display_white_point_y));
    AppendBe32(out, HevcLuminance(color.mastering_display_max_luminance));
    AppendBe32(out, HevcLuminance(color.mastering_display_min_luminance));
    return out;
}

std::vector<uint8_t> BuildHevcContentLightLevelSeiPayload(const ColorMetadata& color) {
    // H.265 D.2.35 content_light_level_info(): two u(16) values.
    std::vector<uint8_t> out;
    out.reserve(4);
    AppendBe16(out, Cll16(color.max_content_light_level));
    AppendBe16(out, Cll16(color.max_frame_average_light_level));
    return out;
}

std::vector<uint8_t> BuildAv1MasteringDisplayObuPayload(const ColorMetadata& color) {
    // AV1 6.7.4 metadata_hdr_mdcv(): primary_chromaticity in R,G,B order
    // (i = 0,1,2), then white point, then luminance_max (24.8) / min (18.14).
    std::vector<uint8_t> out;
    out.reserve(24);
    AppendBe16(out, Av1Chroma(color.mastering_display_primary_r_x));
    AppendBe16(out, Av1Chroma(color.mastering_display_primary_r_y));
    AppendBe16(out, Av1Chroma(color.mastering_display_primary_g_x));
    AppendBe16(out, Av1Chroma(color.mastering_display_primary_g_y));
    AppendBe16(out, Av1Chroma(color.mastering_display_primary_b_x));
    AppendBe16(out, Av1Chroma(color.mastering_display_primary_b_y));
    AppendBe16(out, Av1Chroma(color.mastering_display_white_point_x));
    AppendBe16(out, Av1Chroma(color.mastering_display_white_point_y));
    AppendBe32(out, Av1LuminanceMax(color.mastering_display_max_luminance));
    AppendBe32(out, Av1LuminanceMin(color.mastering_display_min_luminance));
    return out;
}

std::vector<uint8_t> BuildAv1ContentLightLevelObuPayload(const ColorMetadata& color) {
    // AV1 6.7.3 metadata_hdr_cll(): max_cll, max_fall, both f(16). Identical byte
    // layout to the HEVC CLL SEI payload.
    std::vector<uint8_t> out;
    out.reserve(4);
    AppendBe16(out, Cll16(color.max_content_light_level));
    AppendBe16(out, Cll16(color.max_frame_average_light_level));
    return out;
}

} // namespace recorder_core::hdr_meta
