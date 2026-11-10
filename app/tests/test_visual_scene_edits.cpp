#include "extensions/VisualPresetMigration.h"
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

TEST(VisualSceneEdits, GamepadViewRoundTripsAndUnrelatedEditsKeepItsIdentity) {
    const auto registry = Registry();
    auto scene = CameraScene();
    scene.sources.front().type_id = "org.exosnap.gamepad/controller";
    scene.sources.front().settings.insert("future", true);
    const auto source = scene.sources.front().id;
    const auto item = scene.items.front().id;
    auto desired = ProjectSceneGamepad(scene, source, item);
    ASSERT_TRUE(desired.succeeded());
    desired.value()->opacity = 0.5f;
    auto edited = EditSceneGamepad(scene, source, item, *desired.value(), registry);
    ASSERT_TRUE(edited.succeeded());
    EXPECT_FALSE(edited.value()->sources.front().settings["view"]);
    scene = *edited.value();
    for (const auto view : {GamepadView::NativeNormal, GamepadView::NativeGhost, GamepadView::TwoDimensional}) {
        desired = ProjectSceneGamepad(scene, source, item);
        ASSERT_TRUE(desired.succeeded());
        desired.value()->view = view;
        edited = EditSceneGamepad(scene, source, item, *desired.value(), registry);
        ASSERT_TRUE(edited.succeeded());
        const auto encoded = WriteVisualScene(*edited.value());
        ASSERT_TRUE(encoded.succeeded());
        const auto decoded = ReadVisualScene(*encoded.value());
        ASSERT_TRUE(decoded.succeeded());
        scene = *decoded.value();
        const auto restored = ProjectSceneGamepad(scene, source, item);
        ASSERT_TRUE(restored.succeeded());
        EXPECT_EQ(restored.value()->view, view);
        EXPECT_FLOAT_EQ(restored.value()->opacity, 0.5f);
        EXPECT_TRUE(scene.sources.front().settings["future"].value_or(false));
    }
    desired.value()->view = static_cast<GamepadView>(255);
    EXPECT_FALSE(EditSceneGamepad(scene, source, item, *desired.value(), registry).succeeded());
}

VisualSceneConfig LayeredScene() {
    auto scene = CameraScene();
    scene.sources.push_back({engine::VisualSourceId{{3}}, "org.exosnap.gamepad/controller", 1, true, {}});
    SceneItemRecord pad;
    pad.id = engine::SceneItemId{{4}};
    pad.source = scene.sources.back().id;
    scene.items.push_back(pad);
    SceneItemRecord second_camera = scene.items.front();
    second_camera.id = engine::SceneItemId{{6}};
    scene.items.push_back(second_camera);
    return scene;
}

std::vector<engine::SceneItemId> BackToFront(const VisualSceneConfig& scene) {
    std::vector<engine::SceneItemId> order(scene.items.size());
    for (const auto& item : scene.items)
        order[*SceneItemStackIndex(scene, item.id)] = item.id;
    return order;
}

TEST(VisualSceneEdits, StackEdgesRenumberDenselyAndResolveTiesByPublicationOrder) {
    const auto registry = Registry();
    const auto scene = LayeredScene();
    const engine::SceneItemId camera{{2}}, pad{{4}}, second{{6}};
    EXPECT_EQ(BackToFront(scene), (std::vector{camera, pad, second}));
    auto front = MoveSceneItemToEdge(scene, scene.sources.front().id, camera, SceneItemStackEdge::Front, registry);
    ASSERT_TRUE(front.succeeded());
    EXPECT_EQ(BackToFront(*front.value()), (std::vector{pad, second, camera}));
    for (const auto& item : front.value()->items)
        EXPECT_EQ(item.properties.order, static_cast<int32_t>(*SceneItemStackIndex(*front.value(), item.id)));
    auto back = MoveSceneItemToEdge(*front.value(), scene.sources.back().id, pad, SceneItemStackEdge::Back, registry);
    ASSERT_TRUE(back.succeeded());
    EXPECT_EQ(BackToFront(*back.value()), (std::vector{pad, second, camera}));
    auto again =
        MoveSceneItemToEdge(*back.value(), scene.sources.front().id, camera, SceneItemStackEdge::Front, registry);
    ASSERT_TRUE(again.succeeded());
    EXPECT_EQ(BackToFront(*again.value()), BackToFront(*back.value()));
}

TEST(VisualSceneEdits, NodeOperationsRejectItemsOfAnotherSource) {
    const auto registry = Registry();
    const auto scene = LayeredScene();
    const engine::VisualSourceId camera_source{{1}};
    const engine::SceneItemId pad{{4}};
    EXPECT_FALSE(MoveSceneItemToEdge(scene, camera_source, pad, SceneItemStackEdge::Front, registry).succeeded());
    EXPECT_FALSE(SetSceneItemOutputVisible(scene, camera_source, pad, false, registry).succeeded());
    EXPECT_FALSE(RemoveSceneItem(scene, camera_source, pad, registry).succeeded());
    EXPECT_FALSE(RemoveSceneItem(scene, camera_source, {}, registry).succeeded());
    EXPECT_FALSE(RemoveSceneItem(scene, engine::VisualSourceId{{9}}, pad, registry).succeeded());
}

TEST(VisualSceneEdits, HidingChangesOnlyTheAddressedItem) {
    const auto registry = Registry();
    const auto scene = LayeredScene();
    auto hidden = SetSceneItemOutputVisible(scene, scene.sources.front().id, engine::SceneItemId{{6}}, false, registry);
    ASSERT_TRUE(hidden.succeeded());
    EXPECT_TRUE(hidden.value()->items[0].properties.output_visible);
    EXPECT_FALSE(hidden.value()->items[2].properties.output_visible);
    EXPECT_TRUE(hidden.value()->sources.front().enabled);
}

TEST(VisualSceneEdits, RemovingTheLastItemDisablesItsSourceAndAnEditRestoresOne) {
    const auto registry = Registry();
    const auto scene = LayeredScene();
    const auto camera_source = scene.sources.front().id;
    auto removed = RemoveSceneItem(scene, camera_source, engine::SceneItemId{{6}}, registry);
    ASSERT_TRUE(removed.succeeded());
    EXPECT_EQ(removed.value()->items.size(), 2u);
    EXPECT_TRUE(removed.value()->sources.front().enabled);
    removed = RemoveSceneItem(*removed.value(), camera_source, engine::SceneItemId{{2}}, registry);
    ASSERT_TRUE(removed.succeeded());
    ASSERT_EQ(removed.value()->items.size(), 1u);
    EXPECT_EQ(removed.value()->items.front().id, (engine::SceneItemId{{4}}));
    EXPECT_FALSE(removed.value()->sources.front().enabled);
    EXPECT_EQ(removed.value()->sources.size(), 2u);

    EXPECT_FALSE(ProjectSceneWebcam(*removed.value(), camera_source, engine::SceneItemId{{2}}).succeeded());
    auto desired = ProjectSceneWebcam(*removed.value(), camera_source, {});
    ASSERT_TRUE(desired.succeeded());
    EXPECT_FALSE(desired.value()->enabled);
    desired.value()->enabled = true;
    auto restored = EditSceneWebcam(*removed.value(), camera_source, {}, *desired.value(), registry);
    ASSERT_TRUE(restored.succeeded());
    ASSERT_EQ(restored.value()->items.size(), 2u);
    const auto& item = restored.value()->items.back();
    EXPECT_EQ(item.source, camera_source);
    EXPECT_NE(item.id, engine::SceneItemId{});
    EXPECT_EQ(*SceneItemStackIndex(*restored.value(), item.id), 1u);
    EXPECT_TRUE(restored.value()->sources.front().enabled);
    // Once the source has an item again, a null item no longer addresses it.
    EXPECT_FALSE(ProjectSceneWebcam(*restored.value(), camera_source, {}).succeeded());
}

} // namespace
} // namespace exosnap
