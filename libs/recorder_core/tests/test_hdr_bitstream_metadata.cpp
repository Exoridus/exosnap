// Byte-exact unit tests for the in-band HDR10 metadata payload builders
// (recorder_core/hdr_bitstream_metadata.h). Pure functions — no GPU/NVENC.
//
// Reference values: P3-D65 mastering display, 1000 cd/m^2 max / 0.0001 cd/m^2
// min mastering luminance, MaxCLL 1000, MaxFALL 400. The expected byte vectors
// are hand-computed below with the arithmetic shown so a future reader can
// re-derive every byte.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <recorder_core/color_metadata.h>
#include <recorder_core/hdr_bitstream_metadata.h>

using namespace recorder_core;

namespace {

// Uppercase space-separated hex, so a byte mismatch prints a readable diff.
std::string ToHex(const std::vector<uint8_t>& bytes) {
    static const char* kDigits = "0123456789ABCDEF";
    std::string s;
    s.reserve(bytes.size() * 3);
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            s.push_back(' ');
        }
        s.push_back(kDigits[(bytes[i] >> 4) & 0xF]);
        s.push_back(kDigits[bytes[i] & 0xF]);
    }
    return s;
}

template <size_t N> std::string ToHex(const std::array<uint8_t, N>& bytes) {
    return ToHex(std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

// A complete HDR10 colour description with P3-D65 mastering primaries.
ColorMetadata MakeP3D65Hdr10() {
    ColorMetadata c;
    c.primaries = ColorPrimaries::Bt2020;
    c.transfer = TransferCharacteristics::SmpteSt2084;
    c.matrix = MatrixCoefficients::Bt2020Ncl;
    c.range = ColorRange::Limited;
    c.bits_per_channel = 10;
    c.hdr = true;

    c.has_mastering_display = true;
    // Display P3 primaries (CIE 1931 xy).
    c.mastering_display_primary_r_x = 0.680f;
    c.mastering_display_primary_r_y = 0.320f;
    c.mastering_display_primary_g_x = 0.265f;
    c.mastering_display_primary_g_y = 0.690f;
    c.mastering_display_primary_b_x = 0.150f;
    c.mastering_display_primary_b_y = 0.060f;
    // D65 white point.
    c.mastering_display_white_point_x = 0.3127f;
    c.mastering_display_white_point_y = 0.3290f;
    c.mastering_display_max_luminance = 1000.0f;
    c.mastering_display_min_luminance = 0.0001f;

    c.max_content_light_level = 1000;      // MaxCLL
    c.max_frame_average_light_level = 400; // MaxFALL
    return c;
}

} // namespace

// ---------------------------------------------------------------------------
// HEVC Mastering Display Colour Volume SEI (payload type 137, H.265 D.2.28)
// ---------------------------------------------------------------------------
//
// Layout (all big-endian): display_primaries_x/y for c = 0,1,2 in G,B,R order,
// each 0.00002 units (round(coord * 50000)); white_point_x/y likewise; then
// max/min_display_mastering_luminance u(32) in 0.0001 cd/m^2 (round(nits*10000)).
//
//   G_x = round(0.265 * 50000) = 13250 = 0x33C2
//   G_y = round(0.690 * 50000) = 34500 = 0x86C4
//   B_x = round(0.150 * 50000) =  7500 = 0x1D4C
//   B_y = round(0.060 * 50000) =  3000 = 0x0BB8
//   R_x = round(0.680 * 50000) = 34000 = 0x84D0
//   R_y = round(0.320 * 50000) = 16000 = 0x3E80
//   WP_x = round(0.3127 * 50000) = 15635 = 0x3D13
//   WP_y = round(0.3290 * 50000) = 16450 = 0x4042
//   maxLum = round(1000   * 10000) = 10000000 = 0x00989680
//   minLum = round(0.0001 * 10000) =        1 = 0x00000001
TEST(HdrBitstreamMetadata, HevcMasteringDisplaySeiPayloadIsByteExact) {
    const ColorMetadata c = MakeP3D65Hdr10();
    const std::vector<uint8_t> got = hdr_meta::BuildHevcMasteringDisplaySeiPayload(c);

    static constexpr std::array<uint8_t, 24> kExpected = {
        0x33, 0xC2, 0x86, 0xC4, 0x1D, 0x4C, 0x0B, 0xB8, // G_x G_y B_x B_y
        0x84, 0xD0, 0x3E, 0x80,                         // R_x R_y
        0x3D, 0x13, 0x40, 0x42,                         // WP_x WP_y
        0x00, 0x98, 0x96, 0x80,                         // maxLum
        0x00, 0x00, 0x00, 0x01,                         // minLum
    };
    EXPECT_EQ(ToHex(got), ToHex(kExpected));
}

// ---------------------------------------------------------------------------
// HEVC Content Light Level Info SEI (payload type 144, H.265 D.2.35)
// ---------------------------------------------------------------------------
//   max_content_light_level     = 1000 = 0x03E8   u(16)
//   max_pic_average_light_level =  400 = 0x0190   u(16)
TEST(HdrBitstreamMetadata, HevcContentLightLevelSeiPayloadIsByteExact) {
    const ColorMetadata c = MakeP3D65Hdr10();
    const std::vector<uint8_t> got = hdr_meta::BuildHevcContentLightLevelSeiPayload(c);

    static constexpr std::array<uint8_t, 4> kExpected = {0x03, 0xE8, 0x01, 0x90};
    EXPECT_EQ(ToHex(got), ToHex(kExpected));
}

// ---------------------------------------------------------------------------
// AV1 HDR MDCV metadata payload (metadata_type 2, AV1 spec 6.7.4)
// ---------------------------------------------------------------------------
//
// Layout (all big-endian): primary_chromaticity_x/y for i = 0,1,2 in R,G,B
// order, each 0.16 fixed-point (round(coord * 65536)); white_point_x/y likewise;
// luminance_max 24.8 fixed-point (round(nits * 256)); luminance_min 18.14
// fixed-point (round(nits * 16384)).
//
//   R_x = round(0.680 * 65536) = round(44564.48) = 44564 = 0xAE14
//   R_y = round(0.320 * 65536) = round(20971.52) = 20972 = 0x51EC
//   G_x = round(0.265 * 65536) = round(17367.04) = 17367 = 0x43D7
//   G_y = round(0.690 * 65536) = round(45219.84) = 45220 = 0xB0A4
//   B_x = round(0.150 * 65536) = round( 9830.40) =  9830 = 0x2666
//   B_y = round(0.060 * 65536) = round( 3932.16) =  3932 = 0x0F5C
//   WP_x = round(0.3127 * 65536) = round(20493.11) = 20493 = 0x500D
//   WP_y = round(0.3290 * 65536) = round(21561.34) = 21561 = 0x5439
//   luminance_max = round(1000   * 256)   = 256000 = 0x0003E800
//   luminance_min = round(0.0001 * 16384) = round(1.6384) = 2 = 0x00000002
TEST(HdrBitstreamMetadata, Av1MasteringDisplayObuPayloadIsByteExact) {
    const ColorMetadata c = MakeP3D65Hdr10();
    const std::vector<uint8_t> got = hdr_meta::BuildAv1MasteringDisplayObuPayload(c);

    static constexpr std::array<uint8_t, 24> kExpected = {
        0xAE, 0x14, 0x51, 0xEC, 0x43, 0xD7, 0xB0, 0xA4, // R_x R_y G_x G_y
        0x26, 0x66, 0x0F, 0x5C,                         // B_x B_y
        0x50, 0x0D, 0x54, 0x39,                         // WP_x WP_y
        0x00, 0x03, 0xE8, 0x00,                         // luminance_max
        0x00, 0x00, 0x00, 0x02,                         // luminance_min
    };
    EXPECT_EQ(ToHex(got), ToHex(kExpected));
}

// ---------------------------------------------------------------------------
// AV1 HDR CLL metadata payload (metadata_type 1, AV1 spec 6.7.3)
// ---------------------------------------------------------------------------
//   max_cll  = 1000 = 0x03E8   f(16)
//   max_fall =  400 = 0x0190   f(16)
TEST(HdrBitstreamMetadata, Av1ContentLightLevelObuPayloadIsByteExact) {
    const ColorMetadata c = MakeP3D65Hdr10();
    const std::vector<uint8_t> got = hdr_meta::BuildAv1ContentLightLevelObuPayload(c);

    static constexpr std::array<uint8_t, 4> kExpected = {0x03, 0xE8, 0x01, 0x90};
    EXPECT_EQ(ToHex(got), ToHex(kExpected));
}

// ---------------------------------------------------------------------------
// Payload sizes are fixed by spec.
// ---------------------------------------------------------------------------
TEST(HdrBitstreamMetadata, PayloadSizesMatchSpec) {
    const ColorMetadata c = MakeP3D65Hdr10();
    EXPECT_EQ(hdr_meta::BuildHevcMasteringDisplaySeiPayload(c).size(), 24u);
    EXPECT_EQ(hdr_meta::BuildHevcContentLightLevelSeiPayload(c).size(), 4u);
    EXPECT_EQ(hdr_meta::BuildAv1MasteringDisplayObuPayload(c).size(), 24u);
    EXPECT_EQ(hdr_meta::BuildAv1ContentLightLevelObuPayload(c).size(), 4u);
}

// ---------------------------------------------------------------------------
// Scope gate: emit only for HDR10-native (PQ / BT.2020) sessions carrying data.
// ---------------------------------------------------------------------------
TEST(HdrBitstreamMetadata, ShouldEmitOnlyForHdr10NativeWithData) {
    // SDR default → never emit (bitstream must stay byte-identical to before).
    EXPECT_FALSE(hdr_meta::ShouldEmitHdrBitstreamMetadata(ColorMetadata::Sdr709()));

    // Full HDR10 with mastering data → emit.
    EXPECT_TRUE(hdr_meta::ShouldEmitHdrBitstreamMetadata(MakeP3D65Hdr10()));

    // HDR10 signalled but with no mastering data and no CLL → nothing to emit.
    ColorMetadata bare;
    bare.primaries = ColorPrimaries::Bt2020;
    bare.transfer = TransferCharacteristics::SmpteSt2084;
    bare.matrix = MatrixCoefficients::Bt2020Ncl;
    bare.hdr = true;
    EXPECT_FALSE(hdr_meta::ShouldEmitHdrBitstreamMetadata(bare));

    // CLL only (no mastering display) still qualifies.
    ColorMetadata cllOnly = bare;
    cllOnly.max_content_light_level = 500;
    EXPECT_TRUE(hdr_meta::ShouldEmitHdrBitstreamMetadata(cllOnly));

    // hdr flag set but transfer/primaries not PQ/BT.2020 → not HDR10-native.
    ColorMetadata mislabelled = MakeP3D65Hdr10();
    mislabelled.transfer = TransferCharacteristics::Bt709;
    mislabelled.primaries = ColorPrimaries::Bt709;
    EXPECT_FALSE(hdr_meta::ShouldEmitHdrBitstreamMetadata(mislabelled));
}

// ---------------------------------------------------------------------------
// Per-message data-presence predicates.
// ---------------------------------------------------------------------------
TEST(HdrBitstreamMetadata, DataPresencePredicates) {
    const ColorMetadata full = MakeP3D65Hdr10();
    EXPECT_TRUE(hdr_meta::HasMasteringDisplayData(full));
    EXPECT_TRUE(hdr_meta::HasContentLightLevelData(full));

    ColorMetadata noMdcv = full;
    noMdcv.has_mastering_display = false;
    EXPECT_FALSE(hdr_meta::HasMasteringDisplayData(noMdcv));

    ColorMetadata noCll = full;
    noCll.max_content_light_level = 0;
    noCll.max_frame_average_light_level = 0;
    EXPECT_FALSE(hdr_meta::HasContentLightLevelData(noCll));

    // MaxFALL alone (MaxCLL 0) still counts as content-light data present.
    ColorMetadata fallOnly = full;
    fallOnly.max_content_light_level = 0;
    fallOnly.max_frame_average_light_level = 200;
    EXPECT_TRUE(hdr_meta::HasContentLightLevelData(fallOnly));
}
