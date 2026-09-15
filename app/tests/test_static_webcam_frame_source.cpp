// What "static" has to mean for the source that makes 091-55 measurable, on both
// axes: the pixels and the generation. Each is pinned against a deliberate
// counter-example, because a provider failing either one does not look broken --
// it silently removes the defect the overlay test is looking for.

#include "services/StaticWebcamFrameSource.h"

#include <gtest/gtest.h>

using exosnap::MakeStaticWebcamPattern;
using exosnap::StaticWebcamFrameSource;
using exosnap::engine::WebcamFrameProvider;

namespace {

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;
    uint64_t generation = 0;
};

Frame Pull(WebcamFrameProvider& provider) {
    Frame f;
    EXPECT_TRUE(provider.TryGetFrame(f.width, f.height, f.bgra, f.generation));
    return f;
}

// What the real camera does, and what this source must not: a new generation for
// every delivered sample, whether or not anything changed.
class GenerationBumpingSource final : public WebcamFrameProvider {
  public:
    bool TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra,
                     uint64_t& out_generation) override {
        out_width = 8;
        out_height = 8;
        out_bgra.assign(8u * 8u * 4u, 128u);
        out_generation = ++generation_;
        return true;
    }

  private:
    uint64_t generation_ = 0;
};

// The other way to break the contract: hold the generation still while the pixels
// move. A consumer that skips recomposition on an unchanged generation then
// encodes a frame that no longer matches the source.
class SilentlyChangingSource final : public WebcamFrameProvider {
  public:
    bool TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra,
                     uint64_t& out_generation) override {
        out_width = 8;
        out_height = 8;
        out_bgra.assign(8u * 8u * 4u, static_cast<uint8_t>(fill_++));
        out_generation = 7;
        return true;
    }

  private:
    int fill_ = 0;
};

} // namespace

TEST(StaticWebcamFrameSource, ServesTheSamePixelsEveryTime) {
    StaticWebcamFrameSource source(64, 48);
    const Frame first = Pull(source);
    ASSERT_EQ(first.width, 64);
    ASSERT_EQ(first.height, 48);
    ASSERT_EQ(first.bgra.size(), 64u * 48u * 4u);

    for (int i = 0; i < 5; ++i) {
        const Frame again = Pull(source);
        EXPECT_EQ(again.bgra, first.bgra);
    }
}

TEST(StaticWebcamFrameSource, TheGenerationStandsStill) {
    // The half of the contract that is easy to get wrong and impossible to notice
    // from the picture: identical pixels published under a rising generation still
    // recomposite every tick, and a missing overlay generation is then invisible.
    StaticWebcamFrameSource source(64, 48);
    const Frame first = Pull(source);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(Pull(source).generation, first.generation);
    }
}

TEST(StaticWebcamFrameSource, ACameraLikeSourceWouldBreakTheMeasurement) {
    // The counter-example, so the assertion above is a decision and not a
    // restatement of whatever the implementation happens to do.
    GenerationBumpingSource bumping;
    const Frame first = Pull(bumping);
    const Frame second = Pull(bumping);
    EXPECT_EQ(first.bgra, second.bgra) << "identical pixels";
    EXPECT_NE(first.generation, second.generation) << "and yet a new generation every call";
}

TEST(StaticWebcamFrameSource, ASourceThatChangesUnderOneGenerationIsAlsoWrong) {
    SilentlyChangingSource changing;
    const Frame first = Pull(changing);
    const Frame second = Pull(changing);
    EXPECT_EQ(first.generation, second.generation);
    EXPECT_NE(first.bgra, second.bgra) << "pixels moved without saying so";

    // The source under test satisfies neither counter-example.
    StaticWebcamFrameSource source(64, 48);
    const Frame a = Pull(source);
    const Frame b = Pull(source);
    EXPECT_EQ(a.generation, b.generation);
    EXPECT_EQ(a.bgra, b.bgra);
}

TEST(StaticWebcamPattern, CarriesFourDistinctQuadrantsABorderAndOneMark) {
    // Not decoration. A flat colour cannot distinguish an overlay that moved from
    // one that was scaled, and a symmetric pattern cannot distinguish either from
    // a mirrored one.
    const std::vector<uint8_t> pattern = MakeStaticWebcamPattern(64, 48);
    ASSERT_EQ(pattern.size(), 64u * 48u * 4u);

    const auto at = [&](int x, int y) { return pattern[(static_cast<size_t>(y) * 64 + x) * 4u]; };

    // Well inside each quadrant, clear of the border and of the mark.
    const uint8_t top_left = at(24, 18);
    const uint8_t top_right = at(52, 18);
    const uint8_t bottom_left = at(12, 40);
    const uint8_t bottom_right = at(52, 40);
    EXPECT_NE(top_left, top_right);
    EXPECT_NE(top_left, bottom_left);
    EXPECT_NE(bottom_left, bottom_right);
    EXPECT_NE(top_right, bottom_right);

    // The border is darker than every quadrant, so the sprite's extent is findable.
    const uint8_t border = at(0, 0);
    EXPECT_LT(border, top_left);
    EXPECT_LT(border, bottom_right);

    // The mark sits in exactly one quadrant, which is what breaks the symmetry.
    // Counted rather than probed at a fixed pixel, so the assertion is about the
    // property and not about where this implementation happens to put it.
    int marked[4] = {0, 0, 0, 0};
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 64; ++x) {
            if (at(x, y) == 255) {
                ++marked[(y < 24 ? 0 : 2) + (x < 32 ? 0 : 1)];
            }
        }
    }
    const int quadrants_with_mark =
        (marked[0] > 0 ? 1 : 0) + (marked[1] > 0 ? 1 : 0) + (marked[2] > 0 ? 1 : 0) + (marked[3] > 0 ? 1 : 0);
    EXPECT_EQ(quadrants_with_mark, 1) << "a mark in two quadrants is symmetric again";
    EXPECT_GT(marked[0] + marked[1] + marked[2] + marked[3], 0);
}

TEST(StaticWebcamPattern, ADegenerateSizeYieldsNoFrameRatherThanAnEmptyOne) {
    StaticWebcamFrameSource source(0, 0);
    int w = -1;
    int h = -1;
    std::vector<uint8_t> bgra;
    uint64_t generation = 0;
    EXPECT_FALSE(source.TryGetFrame(w, h, bgra, generation))
        << "a provider that answers true with nothing in it composites a hole";
}
