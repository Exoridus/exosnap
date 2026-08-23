// Pure-math tests for the native HDR10 (PQ / BT.2020 / P010) on-screen
// monitoring decode. The recording-view live preview and the frame snapshot read
// the encoded P010 back and must tone-map it to SDR for display; decoding raw PQ
// with the SDR BT.709 matrix (the pre-existing behaviour) leaves reference white
// near mid-grey. These tests pin the reference chain (hdr_preview.h), which the
// live/snapshot paths use, and check the frame converter tracks it.

#include "hdr_preview.h"

#include <recorder_core/hdr_native.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

using namespace recorder_core;

namespace {

// scRGB linear (1.0 = 80-nit reference white) -> HDR10 P010 codes, the exact
// encode chain (hdr_pq.h) that fills the P010 the monitoring paths read back.
P010Codes EncodeScrgbNeutral(float scrgb) {
    return ScrgbToP010(LinearRgb{scrgb, scrgb, scrgb});
}

// Session display peak used by native HDR sessions: an HDR-active 1000-nit panel
// -> knee at 1000/80 = 12.5 reference-white multiples.
constexpr float kPeak1000 = 12.5f;

} // namespace

// ---- PQ EOTF is the exact inverse of the encode OETF -----------------------

TEST(HdrPreview, PqEotfInvertsPqOetf) {
    for (float l : {0.0f, 0.001f, 0.008f, 0.1f, 0.5f, 1.0f}) {
        EXPECT_NEAR(PqEotf(PqOetf(l)), l, 1e-4f) << "l=" << l;
    }
}

// ---- BT.2020 <-> BT.709 gamut round-trips to identity ----------------------

TEST(HdrPreview, GamutRoundTripsToIdentity) {
    for (LinearRgb c : {LinearRgb{0.2f, 0.5f, 0.8f}, LinearRgb{1.0f, 0.0f, 0.0f}, LinearRgb{0.05f, 0.05f, 0.05f}}) {
        const LinearRgb back = Bt2020ToBt709(Bt709ToBt2020(c));
        EXPECT_NEAR(back.r, c.r, 1e-4f);
        EXPECT_NEAR(back.g, c.g, 1e-4f);
        EXPECT_NEAR(back.b, c.b, 1e-4f);
    }
}

// ---- The core fix: reference white decodes bright, not flat mid-grey --------

TEST(HdrPreview, ReferenceWhiteDecodesBrightNotFlat) {
    const P010Codes rw = EncodeScrgbNeutral(1.0f); // 80 nits, PQ-encoded
    const MonitorBgr px = P010PqPixelToMonitorBgr(rw.y, rw.cb, rw.cr, kPeak1000);
    // Golden (CPU reference): 80-nit neutral tone-maps to ~235/255 on a 1000-nit
    // display, near-white and clearly above the ~124 mid-grey the raw-SDR
    // misinterpretation produced (the "washed out / flat" bug).
    EXPECT_NEAR(px.r, 235, 1);
    EXPECT_NEAR(px.g, 235, 1);
    EXPECT_NEAR(px.b, 235, 1);
    // Neutral input stays neutral.
    EXPECT_EQ(px.r, px.g);
    EXPECT_EQ(px.g, px.b);
    // Strictly brighter than the raw-PQ-as-SDR luma of the same Y code (~124),
    // i.e. the decode actually lifts reference white instead of leaving it dim.
    const int naive_sdr = static_cast<int>((static_cast<float>(rw.y) - 64.0f) / 876.0f * 255.0f);
    EXPECT_GT(px.r, naive_sdr + 40);
}

TEST(HdrPreview, BlackDecodesToBlack) {
    const MonitorBgr px = P010PqPixelToMonitorBgr(64, 512, 512, kPeak1000); // limited-range black, neutral chroma
    EXPECT_EQ(px.r, 0);
    EXPECT_EQ(px.g, 0);
    EXPECT_EQ(px.b, 0);
}

TEST(HdrPreview, BrighterInputsAreMonotonicUpToWhite) {
    const P010Codes rw = EncodeScrgbNeutral(1.0f);
    const P010Codes x4 = EncodeScrgbNeutral(4.0f);   // 320 nits
    const P010Codes atp = EncodeScrgbNeutral(12.5f); // at display peak -> white
    const MonitorBgr a = P010PqPixelToMonitorBgr(rw.y, rw.cb, rw.cr, kPeak1000);
    const MonitorBgr b = P010PqPixelToMonitorBgr(x4.y, x4.cb, x4.cr, kPeak1000);
    const MonitorBgr c = P010PqPixelToMonitorBgr(atp.y, atp.cb, atp.cr, kPeak1000);
    EXPECT_LT(a.r, b.r);
    EXPECT_LT(b.r, c.r);
    EXPECT_EQ(c.r, 255); // content at the display peak clamps to white
    EXPECT_NEAR(b.r, 250, 1);
}

// ---- Frame converter (P010 planar -> BGRA) tracks the reference -------------

TEST(HdrPreview, FrameConverterMatchesReferenceForNeutralPatch) {
    const P010Codes rw = EncodeScrgbNeutral(1.0f);
    constexpr uint32_t w = 4, h = 2;
    // P010: 10 bits left-justified in 16-bit words (value = code << 6). Y plane
    // full-res; UV plane interleaved Cb,Cr at half height.
    std::vector<uint16_t> y_plane(static_cast<size_t>(w) * h, static_cast<uint16_t>(rw.y << 6));
    std::vector<uint16_t> uv_plane(static_cast<size_t>(w) * (h / 2));
    for (size_t i = 0; i < uv_plane.size(); i += 2) {
        uv_plane[i] = static_cast<uint16_t>(rw.cb << 6);
        uv_plane[i + 1] = static_cast<uint16_t>(rw.cr << 6);
    }
    PlanarYuv420Frame src;
    src.y_plane = reinterpret_cast<const uint8_t*>(y_plane.data());
    src.y_stride_bytes = w * sizeof(uint16_t);
    src.uv_plane = reinterpret_cast<const uint8_t*>(uv_plane.data());
    src.uv_stride_bytes = w * sizeof(uint16_t);
    src.width = w;
    src.height = h;
    src.bits_per_sample = 10;

    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4u, 0);
    const P010PqMonitorConverter converter(kPeak1000);
    converter.Convert(src, out.data(), w * 4u);

    const MonitorBgr ref = P010PqPixelToMonitorBgr(rw.y, rw.cb, rw.cr, kPeak1000);
    for (uint32_t p = 0; p < w * h; ++p) {
        const uint8_t* px = out.data() + static_cast<size_t>(p) * 4u;
        EXPECT_NEAR(px[0], ref.b, 2) << "pixel " << p; // table quantisation
        EXPECT_NEAR(px[1], ref.g, 2) << "pixel " << p;
        EXPECT_NEAR(px[2], ref.r, 2) << "pixel " << p;
        EXPECT_EQ(px[3], 255) << "pixel " << p;
    }
}

TEST(HdrPreview, FrameConverterIgnoresDegenerateInput) {
    std::vector<uint8_t> out(16, 7);
    PlanarYuv420Frame src; // all-zero: null planes / zero size
    const P010PqMonitorConverter converter(kPeak1000);
    converter.Convert(src, out.data(), 8);
    for (uint8_t b : out)
        EXPECT_EQ(b, 7); // untouched
}

// ---- The fully-planar overload is the same picture, different memory --------

// The editor's decoder hands over YUV420P10LE (separate U and V planes, plain
// [0, 1023] samples), not the P010 layout the capture path reads back. Both
// must resolve to the same picture -- otherwise the editor preview and the
// recording preview disagree about colour for the very same frame.
//
// Deliberately a non-uniform patch: a flat one would pass even with the chroma
// or luma indexing wrong.
TEST(HdrPreview, FullPlanarConverterMatchesTheP010Converter) {
    const P010Codes left = ScrgbToP010(LinearRgb{1.0f, 0.2f, 0.05f});  // warm
    const P010Codes right = ScrgbToP010(LinearRgb{0.05f, 0.3f, 1.2f}); // cool, above reference white
    constexpr uint32_t w = 4, h = 2;
    // One chroma sample per 2x2 block: columns 0-1 take `left`, columns 2-3 `right`.
    const uint16_t y_codes[w * h] = {64, 200, 400, 550, 700, 850, 940, 1023};

    std::vector<uint16_t> p010_y(w * h);
    std::vector<uint16_t> p010_uv(static_cast<size_t>(w) * (h / 2));
    std::vector<uint16_t> planar_y(w * h);
    std::vector<uint16_t> planar_u(static_cast<size_t>(w / 2) * (h / 2));
    std::vector<uint16_t> planar_v(static_cast<size_t>(w / 2) * (h / 2));
    for (size_t i = 0; i < p010_y.size(); ++i) {
        planar_y[i] = y_codes[i];
        p010_y[i] = static_cast<uint16_t>(y_codes[i] << 6); // P010 left-justifies
    }
    p010_uv[0] = static_cast<uint16_t>(left.cb << 6);
    p010_uv[1] = static_cast<uint16_t>(left.cr << 6);
    p010_uv[2] = static_cast<uint16_t>(right.cb << 6);
    p010_uv[3] = static_cast<uint16_t>(right.cr << 6);
    planar_u[0] = left.cb;
    planar_u[1] = right.cb;
    planar_v[0] = left.cr;
    planar_v[1] = right.cr;

    PlanarYuv420Frame p010;
    p010.y_plane = reinterpret_cast<const uint8_t*>(p010_y.data());
    p010.y_stride_bytes = w * sizeof(uint16_t);
    p010.uv_plane = reinterpret_cast<const uint8_t*>(p010_uv.data());
    p010.uv_stride_bytes = w * sizeof(uint16_t);
    p010.width = w;
    p010.height = h;
    p010.bits_per_sample = 10;

    FullPlanarYuv420Frame planar;
    planar.y_plane = reinterpret_cast<const uint8_t*>(planar_y.data());
    planar.y_stride_bytes = w * sizeof(uint16_t);
    planar.u_plane = reinterpret_cast<const uint8_t*>(planar_u.data());
    planar.u_stride_bytes = (w / 2) * sizeof(uint16_t);
    planar.v_plane = reinterpret_cast<const uint8_t*>(planar_v.data());
    planar.v_stride_bytes = (w / 2) * sizeof(uint16_t);
    planar.width = w;
    planar.height = h;
    planar.bits_per_sample = 10;

    const P010PqMonitorConverter converter(kPeak1000);
    std::vector<uint8_t> from_p010(static_cast<size_t>(w) * h * 4u, 0);
    std::vector<uint8_t> from_planar(static_cast<size_t>(w) * h * 4u, 0);
    converter.Convert(p010, from_p010.data(), w * 4u);
    converter.Convert(planar, from_planar.data(), w * 4u);

    EXPECT_EQ(from_planar, from_p010) << "the two input layouts must produce identical pixels";
    // Guard against both paths quietly producing nothing at all.
    EXPECT_NE(from_planar, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4u, 0));
}

TEST(HdrPreview, FullPlanarConverterIgnoresDegenerateInput) {
    std::vector<uint8_t> out(16, 7);
    FullPlanarYuv420Frame src; // all-zero: null planes / zero size
    const P010PqMonitorConverter converter(kPeak1000);
    converter.Convert(src, out.data(), 8);
    for (uint8_t b : out)
        EXPECT_EQ(b, 7); // untouched
}

// ---- Native HDR10 requires a 10-bit encode target — guard fails fast --------

TEST(HdrPreview, NativeHdr10BitDepthGuard) {
    // Native expected + 8-bit => inconsistency (guard fires).
    EXPECT_TRUE(NativeHdr10BitDepthViolation(true, BitDepth::Bit8));
    // Native expected + 10-bit => fine.
    EXPECT_FALSE(NativeHdr10BitDepthViolation(true, BitDepth::Bit10));
    // Not a native HDR session => never a violation regardless of bit depth.
    EXPECT_FALSE(NativeHdr10BitDepthViolation(false, BitDepth::Bit8));
    EXPECT_FALSE(NativeHdr10BitDepthViolation(false, BitDepth::Bit10));
}
