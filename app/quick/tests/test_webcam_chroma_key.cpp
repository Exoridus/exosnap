// QCR-609. The webcam chroma key called std::hypot once per pixel, at 30 Hz over
// a 320x180 preview — roughly 1.7 million correctly-rounded square roots per
// second for a decision that is, for almost every pixel, "clearly inside the key"
// or "clearly outside it".
//
// The square root now runs only inside the alpha ramp, between `tolerance` and
// `tolerance + softness`; the two saturated bands are decided on the squared
// distance against the squared bounds. These cases assert that the resulting
// alpha is unchanged — exactly, not approximately — across the whole colour cube
// and specifically at the two bounds.
//
// Driven through RecordWebcamFrameProvider::requestImage() rather than a copy of
// the kernel: the kernel is file-local by design, and what matters is the pixel
// that reaches the QML image, not an extracted formula that could drift from it.

#include "RecordWebcamFrameProvider.h"

#include <QColor>
#include <QImage>
#include <QSize>
#include <QString>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using exosnap::quick::RecordWebcamFrameProvider;

namespace {

struct KeyConfig {
    uint8_t red = 0;
    uint8_t green = 255;
    uint8_t blue = 0;
    float tolerance = 0.15f;
    float softness = 0.10f;
    float spill_reduction = 0.0f;
};

// The pre-QCR-609 alpha, transcribed from the old implementation: one hypot, one
// clamp, no branches.
float LegacyKeyedAlpha(uint8_t r8, uint8_t g8, uint8_t b8, const KeyConfig& key) {
    const float key_red = static_cast<float>(key.red) / 255.0f;
    const float key_green = static_cast<float>(key.green) / 255.0f;
    const float key_blue = static_cast<float>(key.blue) / 255.0f;
    const float key_cb = -0.169f * key_red - 0.331f * key_green + 0.5f * key_blue + 0.5f;
    const float key_cr = 0.5f * key_red - 0.419f * key_green - 0.081f * key_blue + 0.5f;
    const float softness = std::max(key.softness, 0.001f);

    const float red = static_cast<float>(r8) / 255.0f;
    const float green = static_cast<float>(g8) / 255.0f;
    const float blue = static_cast<float>(b8) / 255.0f;
    const float cb = -0.169f * red - 0.331f * green + 0.5f * blue + 0.5f;
    const float cr = 0.5f * red - 0.419f * green - 0.081f * blue + 0.5f;
    const float distance = std::hypot(cb - key_cb, cr - key_cr);
    return std::clamp((distance - key.tolerance) / softness, 0.0f, 1.0f);
}

int LegacyAlphaByte(uint8_t r8, uint8_t g8, uint8_t b8, const KeyConfig& key) {
    // Source alpha is 255 in every fixture below, so the old line
    // `pixels[3] * keyed_alpha` reduces to this.
    return static_cast<int>(std::lround(255.0f * LegacyKeyedAlpha(r8, g8, b8, key)));
}

// One row per colour, opaque, RGBA8888 — the format the kernel converts to, so
// the conversion branch is exercised in its skip form as well.
QImage MakeStrip(const std::vector<std::array<uint8_t, 3>>& colours) {
    QImage image(static_cast<int>(colours.size()), 1, QImage::Format_RGBA8888);
    uchar* pixels = image.scanLine(0);
    for (size_t i = 0; i < colours.size(); ++i) {
        pixels[i * 4 + 0] = colours[i][0];
        pixels[i * 4 + 1] = colours[i][1];
        pixels[i * 4 + 2] = colours[i][2];
        pixels[i * 4 + 3] = 255;
    }
    return image;
}

QImage RunKey(const QImage& source, const KeyConfig& key) {
    RecordWebcamFrameProvider provider;
    provider.submitFrame(source);
    provider.setChromaKey(true, key.red, key.green, key.blue, key.tolerance, key.softness, key.spill_reduction);
    QSize size;
    // An invalid requested size is the "no scaling" path, so the kernel sees the
    // submitted pixels untouched.
    return provider.requestImage(QString{}, &size, QSize{});
}

} // namespace

TEST(WebcamChromaKeyTest, FullyKeyedAndFullyOpaqueColoursAreUnchanged) {
    const KeyConfig key;
    const std::vector<std::array<uint8_t, 3>> colours{
        {0, 255, 0},     // the key colour itself — distance 0
        {10, 240, 20},   // near the key
        {255, 255, 255}, // white
        {0, 0, 0},       // black
        {255, 0, 0},     // red
        {0, 0, 255},     // blue
    };
    const QImage keyed = RunKey(MakeStrip(colours), key);
    ASSERT_FALSE(keyed.isNull());
    const uchar* pixels = keyed.constScanLine(0);
    for (size_t i = 0; i < colours.size(); ++i) {
        EXPECT_EQ(static_cast<int>(pixels[i * 4 + 3]),
                  LegacyAlphaByte(colours[i][0], colours[i][1], colours[i][2], key))
            << "colour index " << i;
    }
}

TEST(WebcamChromaKeyTest, ColoursOnEitherSideOfBothBoundsClassifyTheSame) {
    // Greens walked across the whole ramp: the pure key, the band below
    // tolerance, the ramp itself, and everything past tolerance + softness.
    const KeyConfig key;
    std::vector<std::array<uint8_t, 3>> colours;
    for (int green = 255; green >= 0; green -= 3)
        colours.push_back({0, static_cast<uint8_t>(green), 0});

    const QImage keyed = RunKey(MakeStrip(colours), key);
    ASSERT_FALSE(keyed.isNull());
    const uchar* pixels = keyed.constScanLine(0);
    for (size_t i = 0; i < colours.size(); ++i) {
        EXPECT_EQ(static_cast<int>(pixels[i * 4 + 3]),
                  LegacyAlphaByte(colours[i][0], colours[i][1], colours[i][2], key))
            << "green=" << static_cast<int>(colours[i][1]);
    }
}

TEST(WebcamChromaKeyTest, MatchesTheOldKernelOverTheColourCube) {
    const std::vector<KeyConfig> keys{
        KeyConfig{},                              // default green key
        KeyConfig{0, 255, 0, 0.0f, 0.0f, 0.0f},   // zero tolerance, softness floored to 0.001
        KeyConfig{0, 255, 0, 1.0f, 1.0f, 0.0f},   // everything inside the ramp
        KeyConfig{0, 0, 255, 0.30f, 0.05f, 0.0f}, // blue key, hard edge
    };

    // A 6x6x6 lattice over the cube: 216 colours, every one of them classified by
    // both kernels.
    std::vector<std::array<uint8_t, 3>> colours;
    for (int r = 0; r <= 255; r += 51)
        for (int g = 0; g <= 255; g += 51)
            for (int b = 0; b <= 255; b += 51)
                colours.push_back({static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)});

    for (const KeyConfig& key : keys) {
        const QImage keyed = RunKey(MakeStrip(colours), key);
        ASSERT_FALSE(keyed.isNull());
        const uchar* pixels = keyed.constScanLine(0);
        for (size_t i = 0; i < colours.size(); ++i) {
            EXPECT_EQ(static_cast<int>(pixels[i * 4 + 3]),
                      LegacyAlphaByte(colours[i][0], colours[i][1], colours[i][2], key))
                << "tolerance=" << key.tolerance << " softness=" << key.softness << " colour index " << i;
        }
    }
}

TEST(WebcamChromaKeyTest, ADisabledKeyLeavesEveryPixelOpaque) {
    const std::vector<std::array<uint8_t, 3>> colours{{0, 255, 0}, {128, 128, 128}};
    RecordWebcamFrameProvider provider;
    provider.submitFrame(MakeStrip(colours));
    provider.setChromaKey(false, 0, 255, 0, 0.15f, 0.10f, 0.0f);
    QSize size;
    const QImage untouched = provider.requestImage(QString{}, &size, QSize{});
    ASSERT_FALSE(untouched.isNull());
    const uchar* pixels = untouched.constScanLine(0);
    EXPECT_EQ(static_cast<int>(pixels[3]), 255);
    EXPECT_EQ(static_cast<int>(pixels[7]), 255);
}

TEST(WebcamChromaKeyTest, ANonRgbaSourceIsStillConverted) {
    // The conversion is now skipped when the format already matches; a source
    // that does NOT match must still come back as RGBA8888 with the same key
    // decisions.
    QImage rgb32(2, 1, QImage::Format_RGB32);
    rgb32.setPixelColor(0, 0, QColor(0, 255, 0));
    rgb32.setPixelColor(1, 0, QColor(255, 0, 0));

    const KeyConfig key;
    RecordWebcamFrameProvider provider;
    provider.submitFrame(rgb32);
    provider.setChromaKey(true, key.red, key.green, key.blue, key.tolerance, key.softness, key.spill_reduction);
    QSize size;
    const QImage keyed = provider.requestImage(QString{}, &size, QSize{});

    ASSERT_FALSE(keyed.isNull());
    EXPECT_EQ(keyed.format(), QImage::Format_RGBA8888);
    const uchar* pixels = keyed.constScanLine(0);
    EXPECT_EQ(static_cast<int>(pixels[3]), LegacyAlphaByte(0, 255, 0, key));
    EXPECT_EQ(static_cast<int>(pixels[7]), LegacyAlphaByte(255, 0, 0, key));
}
