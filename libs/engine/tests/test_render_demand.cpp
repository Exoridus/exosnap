#include <exosnap/engine/render_demand.h>

#include <gtest/gtest.h>

#include <array>

using namespace exosnap::engine;

namespace {
constexpr CompositionSize source{3840, 2160};
constexpr RenderDemand recording{1, 1, 7, {1280, 720}, 60};
constexpr RenderDemand preview{2, 1, 7, {2560, 1440}, 60};
} // namespace

TEST(RenderDemand, DisabledRetainsSourceOracle) {
    EXPECT_EQ(ResolveCompositionDemand(source, std::array{recording}, false).pixels, source);
}

TEST(RenderDemand, SingleSdrOutputUsesRequestedPixels) {
    const auto plan = ResolveCompositionDemand(source, std::array{recording}, true);
    EXPECT_EQ(plan.pixels, recording.pixels);
    EXPECT_TRUE(plan.reduced);
}

TEST(RenderDemand, VisiblePreviewPreservesItsHigherDetail) {
    EXPECT_EQ(ResolveCompositionDemand(source, std::array{recording, preview}, true).pixels, preview.pixels);
    auto hidden = preview;
    hidden.visible = false;
    EXPECT_EQ(ResolveCompositionDemand(source, std::array{recording, hidden}, true).pixels, recording.pixels);
}

TEST(RenderDemand, IncompatibleConsumersRetainSource) {
    auto different = preview;
    different.pixels = {1440, 2560};
    EXPECT_FALSE(ResolveCompositionDemand(source, std::array{recording, different}, true).reduced);
    different = preview;
    ++different.revision;
    EXPECT_FALSE(ResolveCompositionDemand(source, std::array{recording, different}, true).reduced);
    different = preview;
    ++different.scene;
    EXPECT_FALSE(ResolveCompositionDemand(source, std::array{recording, different}, true).reduced);
    different = preview;
    different.cadence = 30;
    EXPECT_FALSE(ResolveCompositionDemand(source, std::array{recording, different}, true).reduced);
}

TEST(RenderDemand, UnsafeRepresentationsAndUnknownDemandRetainSource) {
    for (const auto color : {CompositionColor::Hdr, CompositionColor::Mixed}) {
        auto demand = recording;
        demand.color = color;
        EXPECT_FALSE(ResolveCompositionDemand(source, std::array{demand}, true).reduced);
    }
    auto demand = recording;
    demand.alpha = true;
    EXPECT_FALSE(ResolveCompositionDemand(source, std::array{demand}, true).reduced);
    demand = recording;
    demand.full_scene = false;
    EXPECT_FALSE(ResolveCompositionDemand(source, std::array{demand}, true).reduced);
    demand = recording;
    demand.pixels = {};
    EXPECT_FALSE(ResolveCompositionDemand(source, std::array{demand}, true).reduced);
    EXPECT_FALSE(ResolveCompositionDemand(source, {}, true).reduced);
}

TEST(RenderDemand, LargerDemandNeverInventsSourceDetail) {
    auto demand = preview;
    demand.pixels = {7680, 4320};
    EXPECT_EQ(ResolveCompositionDemand(source, std::array{demand}, true).pixels, source);
}

TEST(RenderDemand, PreviewAccountsForDpiZoomAndVisibility) {
    EXPECT_EQ(ResolvePreviewPixelDemand(1280, 720, 2, 1, 1, true), (CompositionSize{2560, 1440}));
    EXPECT_EQ(ResolvePreviewPixelDemand(1280, 720, 1, 0.5, 0.5, true), (CompositionSize{2560, 1440}));
    EXPECT_EQ(ResolvePreviewPixelDemand(1280, 720, 2, 1, 1, false), (CompositionSize{}));
    EXPECT_FALSE(ResolvePreviewPixelDemand(1280, 720, 0, 1, 1, true));
    EXPECT_FALSE(ResolvePreviewPixelDemand(1280, 720, 2, 0, 1, true));
}
