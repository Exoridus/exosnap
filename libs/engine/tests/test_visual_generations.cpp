#include "exosnap/engine/visual_generations.h"
#include <gtest/gtest.h>

using namespace exosnap::engine;

TEST(VisualFrameKey, DefaultKeysAreEqual) {
    EXPECT_EQ(VisualFrameKey{}, VisualFrameKey{});
}

TEST(VisualFrameKey, DiffersWhenAnySingleGenerationDiffers) {
    VisualFrameKey base{};

    VisualFrameKey screenChanged{};
    screenChanged.screen_generation = 1;
    EXPECT_NE(base, screenChanged);

    VisualFrameKey webcamChanged{};
    webcamChanged.webcam_generation = 1;
    EXPECT_NE(base, webcamChanged);

    VisualFrameKey cursorChanged{};
    cursorChanged.cursor_generation = 1;
    EXPECT_NE(base, cursorChanged);

    VisualFrameKey overlayChanged{};
    overlayChanged.overlay_generation = 1;
    EXPECT_NE(base, overlayChanged);

    VisualFrameKey colorChanged{};
    colorChanged.color_pipeline_generation = 1;
    EXPECT_NE(base, colorChanged);
}

TEST(VisualFrameKey, EqualWhenAllFieldsMatch) {
    VisualFrameKey a{1, 2, 3, 4, 5};
    VisualFrameKey b{1, 2, 3, 4, 5};
    EXPECT_EQ(a, b);
}

TEST(MakeVisualFrameKeyTest, CopiesEachGenerationFieldByName) {
    VisualGenerations gens{};
    gens.screen = 10;
    gens.webcam = 20;
    gens.cursor = 30;
    gens.overlay = 40;
    gens.color_pipeline = 50;

    const VisualFrameKey key = MakeVisualFrameKey(gens);
    EXPECT_EQ(key.screen_generation, 10u);
    EXPECT_EQ(key.webcam_generation, 20u);
    EXPECT_EQ(key.cursor_generation, 30u);
    EXPECT_EQ(key.overlay_generation, 40u);
    EXPECT_EQ(key.color_pipeline_generation, 50u);
}

TEST(MakeVisualFrameKeyTest, IsConstexprEvaluable) {
    constexpr VisualGenerations gens{1, 2, 3, 4, 5};
    constexpr VisualFrameKey key = MakeVisualFrameKey(gens);
    static_assert(key.screen_generation == 1);
}
