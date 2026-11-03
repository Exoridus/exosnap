#include "models/VisualSceneEdits.h"

#include <gtest/gtest.h>

namespace exosnap {
namespace {

CapabilityRegistry Registry() {
    CapabilityRegistry registry;
    SourceFactory unused = [](SourceCreateRequest) -> engine::Outcome<std::unique_ptr<VisualSource>> {
        return engine::EngineError{.context = "Source creation is unavailable in this test"};
    };
    CachedVisualOptions empty = [](const VisualSourceRecord&) { return ChoiceSnapshot{}; };
    EXPECT_TRUE(registry.Register({{"org.exosnap.webcam"}, {MakeWebcamVisualSourceType(unused, empty)}}).succeeded());
    EXPECT_TRUE(
        registry.Register({{"org.exosnap.gamepad"}, {MakeGamepadVisualSourceType(unused, empty, empty)}}).succeeded());
    EXPECT_TRUE(registry.Seal().succeeded());
    return registry;
}

VisualSceneConfig CameraScene() {
    VisualSceneConfig scene;
    scene.sources.push_back({engine::VisualSourceId{{1}}, "org.exosnap.webcam/camera", 1, true, {}});
    SceneItemRecord item;
    item.id = engine::SceneItemId{{2}};
    item.source = scene.sources.front().id;
    scene.items.push_back(item);
    return scene;
}

TEST(VisualSceneEdits, PresentationEditPreservesAbsentKnownAndUnknownSourceSettings) {
    const auto registry = Registry();
    auto scene = CameraScene();
    scene.sources.front().settings.insert("future", toml::table{{"mode", "keep"}});
    auto desired = ProjectSceneWebcam(scene, scene.sources.front().id, scene.items.front().id);
    ASSERT_TRUE(desired.succeeded());
    desired.value()->opacity = 0.5f;
    const auto edited =
        EditSceneWebcam(scene, scene.sources.front().id, scene.items.front().id, *desired.value(), registry);
    ASSERT_TRUE(edited.succeeded());
    EXPECT_EQ(edited.value()->sources.front().settings, scene.sources.front().settings);
    EXPECT_FLOAT_EQ(edited.value()->items.front().properties.opacity, 0.5f);
    EXPECT_FLOAT_EQ(scene.items.front().properties.opacity, 1.0f);
}

TEST(VisualSceneEdits, KnownControlChangesRetainUnknownChildrenAndResetRetainsFutureNames) {
    const auto registry = Registry();
    auto scene = CameraScene();
    scene.sources.front().settings.insert(
        "controls", toml::table{{"brightness", toml::table{{"auto", false}, {"value", 10}, {"future", "keep"}}},
                                {"future_control", toml::table{{"unit", "keep"}}}});
    auto desired = ProjectSceneWebcam(scene, scene.sources.front().id, scene.items.front().id);
    ASSERT_TRUE(desired.succeeded());
    desired.value()->controls.front().value = 20;
    auto edited = EditSceneWebcam(scene, scene.sources.front().id, scene.items.front().id, *desired.value(), registry);
    ASSERT_TRUE(edited.succeeded());
    const auto& settings = edited.value()->sources.front().settings;
    EXPECT_EQ(settings["controls"]["brightness"]["future"].value<std::string>(), "keep");
    desired.value()->controls.clear();
    edited =
        EditSceneWebcam(*edited.value(), scene.sources.front().id, scene.items.front().id, *desired.value(), registry);
    ASSERT_TRUE(edited.succeeded());
    EXPECT_FALSE(edited.value()->sources.front().settings["controls"].as_table()->contains("brightness"));
    EXPECT_EQ(edited.value()->sources.front().settings["controls"]["future_control"]["unit"].value<std::string>(),
              "keep");
}

TEST(VisualSceneEdits, FutureVersionsWrongItemAndInvalidGraphCannotBeMutated) {
    const auto registry = Registry();
    auto scene = CameraScene();
    auto desired = ProjectSceneWebcam(scene, scene.sources.front().id, scene.items.front().id);
    ASSERT_TRUE(desired.succeeded());
    desired.value()->enabled = false;
    EXPECT_FALSE(EditSceneWebcam(scene, scene.sources.front().id, engine::SceneItemId{{9}}, *desired.value(), registry)
                     .succeeded());
    scene.sources.front().settings_version = 2;
    EXPECT_FALSE(EditSceneWebcam(scene, scene.sources.front().id, scene.items.front().id, *desired.value(), registry)
                     .succeeded());
    scene.sources.front().settings_version = 1;
    scene.items.push_back(scene.items.front());
    EXPECT_FALSE(EditSceneWebcam(scene, scene.sources.front().id, scene.items.front().id, *desired.value(), registry)
                     .succeeded());
    EXPECT_TRUE(scene.sources.front().enabled);
}

TEST(VisualSceneEdits, AllGamepadLayoutsRoundTripWithoutChangingIdentityOrFutureSettings) {
    const auto registry = Registry();
    auto scene = CameraScene();
    scene.sources.front().type_id = "org.exosnap.gamepad/controller";
    scene.sources.front().settings.insert("future", true);
    for (int layout = -1; layout < static_cast<int>(kGamepadOverlayArtworkIds.size()); ++layout) {
        auto desired = ProjectSceneGamepad(scene, scene.sources.front().id, scene.items.front().id);
        ASSERT_TRUE(desired.succeeded());
        desired.value()->layout = layout;
        const auto edited =
            EditSceneGamepad(scene, scene.sources.front().id, scene.items.front().id, *desired.value(), registry);
        ASSERT_TRUE(edited.succeeded());
        const auto projected = ProjectSceneGamepad(*edited.value(), scene.sources.front().id, scene.items.front().id);
        ASSERT_TRUE(projected.succeeded());
        EXPECT_EQ(projected.value()->layout, layout);
        EXPECT_EQ(edited.value()->sources.front().id, scene.sources.front().id);
        EXPECT_EQ(edited.value()->items.front().id, scene.items.front().id);
        EXPECT_TRUE(edited.value()->sources.front().settings["future"].value_or(false));
    }
}

} // namespace
} // namespace exosnap
