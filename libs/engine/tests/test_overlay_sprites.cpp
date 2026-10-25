#include "sprite_layers_observation.h"
#include <exosnap/engine/overlay_sprites.h>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <vector>

using namespace exosnap::engine;

namespace {

SpriteQuad Quad(SpriteAtlasRect source, SpriteContentRect destination, float opacity = 1.0f) {
    SpriteQuad quad;
    quad.source = source;
    quad.destination = destination;
    quad.opacity = opacity;
    return quad;
}

std::shared_ptr<const SpriteAtlas> Atlas(uint64_t generation, int width = 4, int height = 4) {
    auto atlas = std::make_shared<SpriteAtlas>();
    atlas->width = width;
    atlas->height = height;
    atlas->generation = generation;
    atlas->pixels.assign(static_cast<size_t>(width) * height * 4, 0);
    return atlas;
}

std::shared_ptr<const SpriteLayerSnapshot> Layer(std::shared_ptr<const SpriteAtlas> atlas,
                                                 std::vector<SpriteQuad> quads) {
    auto layer = std::make_shared<SpriteLayerSnapshot>();
    layer->atlas = std::move(atlas);
    layer->quads = std::move(quads);
    return layer;
}

} // namespace

TEST(MapSpriteQuad, MapsIntoTheContentRectangleWithTheAtlasSubRect) {
    // 100x40 atlas, source (20,10) 40x20, drawn at a quarter of a 400x200 content
    // rect offset by (8,6).
    const SpriteDraw draw =
        MapSpriteQuad(Quad({20, 10, 40, 20}, {0.25f, 0.5f, 0.25f, 0.25f}, 0.5f), 100, 40, 8, 6, 400, 200);
    ASSERT_TRUE(draw.IsValid());
    EXPECT_EQ(draw.destination.x, 108);
    EXPECT_EQ(draw.destination.y, 106);
    EXPECT_EQ(draw.destination.w, 100);
    EXPECT_EQ(draw.destination.h, 50);
    EXPECT_FLOAT_EQ(draw.u0, 0.2f);
    EXPECT_FLOAT_EQ(draw.v0, 0.25f);
    EXPECT_FLOAT_EQ(draw.u1, 0.6f);
    EXPECT_FLOAT_EQ(draw.v1, 0.75f);
    EXPECT_FLOAT_EQ(draw.opacity, 0.5f);
}

TEST(MapSpriteQuad, ClipsAtTheContentEdgeAndKeepsTheVisiblePartsScale) {
    // Half of the quad hangs off the left edge: the right half of the source shows.
    const SpriteDraw draw = MapSpriteQuad(Quad({0, 0, 10, 10}, {-0.1f, 0.0f, 0.2f, 0.2f}), 10, 10, 0, 0, 100, 100);
    ASSERT_TRUE(draw.IsValid());
    EXPECT_EQ(draw.destination.x, 0);
    EXPECT_EQ(draw.destination.w, 10);
    EXPECT_EQ(draw.destination.h, 20);
    EXPECT_FLOAT_EQ(draw.u0, 0.5f);
    EXPECT_FLOAT_EQ(draw.u1, 1.0f);
    EXPECT_FLOAT_EQ(draw.v0, 0.0f);
    EXPECT_FLOAT_EQ(draw.v1, 1.0f);
}

TEST(MapSpriteQuad, RejectsWhatCannotBeDrawn) {
    const SpriteContentRect inside{0.1f, 0.1f, 0.2f, 0.2f};
    EXPECT_FALSE(MapSpriteQuad(Quad({0, 0, 4, 4}, inside), 4, 4, 0, 0, 0, 100).IsValid());
    EXPECT_FALSE(MapSpriteQuad(Quad({0, 0, 5, 4}, inside), 4, 4, 0, 0, 100, 100).IsValid());
    EXPECT_FALSE(MapSpriteQuad(Quad({-1, 0, 2, 2}, inside), 4, 4, 0, 0, 100, 100).IsValid());
    EXPECT_FALSE(MapSpriteQuad(Quad({0, 0, 0, 2}, inside), 4, 4, 0, 0, 100, 100).IsValid());
    EXPECT_FALSE(MapSpriteQuad(Quad({0, 0, 4, 4}, inside, 0.0f), 4, 4, 0, 0, 100, 100).IsValid());
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(MapSpriteQuad(Quad({0, 0, 4, 4}, {nan, 0.0f, 0.1f, 0.1f}), 4, 4, 0, 0, 100, 100).IsValid());
    EXPECT_FALSE(MapSpriteQuad(Quad({0, 0, 4, 4}, {1.5f, 0.0f, 0.2f, 0.2f}), 4, 4, 0, 0, 100, 100).IsValid());
    EXPECT_FALSE(MapSpriteQuad(Quad({0, 0, 4, 4}, {1e30f, 0.0f, 1e30f, 0.2f}), 4, 4, 0, 0, 100, 100).IsValid());
    // Narrower than half a pixel rounds to nothing.
    EXPECT_FALSE(MapSpriteQuad(Quad({0, 0, 4, 4}, {0.5f, 0.5f, 0.004f, 0.1f}), 4, 4, 0, 0, 100, 100).IsValid());
}

TEST(MapSpriteQuad, NonFiniteOpacityIsOpaqueAndExcessIsClamped) {
    const SpriteContentRect inside{0.0f, 0.0f, 0.5f, 0.5f};
    EXPECT_FLOAT_EQ(
        MapSpriteQuad(Quad({0, 0, 4, 4}, inside, std::numeric_limits<float>::quiet_NaN()), 4, 4, 0, 0, 8, 8).opacity,
        1.0f);
    EXPECT_FLOAT_EQ(MapSpriteQuad(Quad({0, 0, 4, 4}, inside, 3.0f), 4, 4, 0, 0, 8, 8).opacity, 1.0f);
}

TEST(SpriteLayerChanged, ComparesWhatIsDrawn) {
    const auto atlas = Atlas(1);
    const auto quad = Quad({0, 0, 2, 2}, {0.0f, 0.0f, 0.1f, 0.1f});
    const auto base = Layer(atlas, {quad});

    EXPECT_FALSE(SpriteLayerChanged(base.get(), base.get()));
    EXPECT_FALSE(SpriteLayerChanged(nullptr, nullptr));
    EXPECT_TRUE(SpriteLayerChanged(nullptr, base.get()));
    EXPECT_TRUE(SpriteLayerChanged(base.get(), nullptr));

    // An equal copy is not a change, so a source may rebuild its snapshot freely.
    EXPECT_FALSE(SpriteLayerChanged(base.get(), Layer(Atlas(1), {quad}).get()));

    EXPECT_TRUE(SpriteLayerChanged(base.get(), Layer(Atlas(2), {quad}).get()));
    EXPECT_TRUE(SpriteLayerChanged(base.get(), Layer(Atlas(1, 8, 4), {quad}).get()));
    EXPECT_TRUE(SpriteLayerChanged(base.get(), Layer(nullptr, {quad}).get()));
    EXPECT_TRUE(SpriteLayerChanged(base.get(), Layer(atlas, {}).get()));
    EXPECT_TRUE(SpriteLayerChanged(base.get(), Layer(atlas, {quad, quad}).get()));

    auto moved = quad;
    moved.destination.x = 0.01f;
    EXPECT_TRUE(SpriteLayerChanged(base.get(), Layer(atlas, {moved}).get()));
    auto cropped = quad;
    cropped.source.w = 1;
    EXPECT_TRUE(SpriteLayerChanged(base.get(), Layer(atlas, {cropped}).get()));
    auto faded = quad;
    faded.opacity = 0.5f;
    EXPECT_TRUE(SpriteLayerChanged(base.get(), Layer(atlas, {faded}).get()));
}

namespace {

struct ScriptedSource final : SpriteSourceProvider {
    std::shared_ptr<const SpriteLayerSnapshot> next;
    uint64_t last_time = 0;
    int calls = 0;

    std::shared_ptr<const SpriteLayerSnapshot> Snapshot(uint64_t frame_time_qpc_100ns) override {
        last_time = frame_time_qpc_100ns;
        ++calls;
        return next;
    }
};

} // namespace

TEST(SpriteLayersObservation, AdvancesTheGenerationOnlyWhenALayerChanges) {
    ScriptedSource lower;
    ScriptedSource upper;
    const std::vector<SpriteSourceProvider*> sources{&lower, nullptr, &upper};
    const auto atlas = Atlas(1);
    const auto quad = Quad({0, 0, 2, 2}, {0.0f, 0.0f, 0.1f, 0.1f});

    VisualGenerations generations;
    SpriteLayersObservation observation;

    EXPECT_FALSE(observation.Observe(generations, sources, 100));
    EXPECT_EQ(generations.sprites, 0u);
    EXPECT_FALSE(observation.AnyVisible());
    EXPECT_EQ(observation.Layers().size(), 3u);
    EXPECT_EQ(lower.last_time, 100u);
    EXPECT_EQ(upper.last_time, 100u);

    upper.next = Layer(atlas, {quad});
    EXPECT_TRUE(observation.Observe(generations, sources, 200));
    EXPECT_EQ(generations.sprites, 1u);
    EXPECT_TRUE(observation.AnyVisible());
    EXPECT_EQ(observation.Layers()[2], upper.next);

    // Rebuilt but identical: no new generation, so a held screen is not recomposited.
    upper.next = Layer(atlas, {quad});
    EXPECT_FALSE(observation.Observe(generations, sources, 300));
    EXPECT_EQ(generations.sprites, 1u);

    // A pressed button: one quad more on the lower layer.
    lower.next = Layer(atlas, {quad});
    EXPECT_TRUE(observation.Observe(generations, sources, 400));
    EXPECT_EQ(generations.sprites, 2u);
    EXPECT_EQ(lower.calls, 4);
}
