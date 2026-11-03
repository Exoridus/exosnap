#include "extensions/CapabilityRegistry.h"
#include "extensions/VisualPresetMigration.h"

#include <gtest/gtest.h>

#include <limits>

namespace exosnap {
namespace {

const VisualSourceRecord* Source(const VisualSceneConfig& scene, std::string_view type) {
    const auto found = std::find_if(scene.sources.begin(), scene.sources.end(),
                                    [type](const auto& source) { return source.type_id == type; });
    return found == scene.sources.end() ? nullptr : &*found;
}

const SceneItemRecord* Item(const VisualSceneConfig& scene, engine::VisualSourceId source) {
    const auto found = std::find_if(scene.items.begin(), scene.items.end(),
                                    [source](const auto& item) { return item.source == source; });
    return found == scene.items.end() ? nullptr : &*found;
}

TEST(VisualPresetMigration, EveryIntegratedLayoutAndAutoKeepsItsId) {
    for (int layout = -1; layout < static_cast<int>(kGamepadOverlayArtworkIds.size()); ++layout) {
        GamepadOverlaySettings pad;
        pad.layout = layout;
        auto migrated = MigrateLegacyVisualConfig({}, pad);
        ASSERT_TRUE(migrated.succeeded());
        const auto* source = Source(*migrated.value(), "org.exosnap.gamepad/controller");
        ASSERT_NE(source, nullptr);
        const auto expected = layout < 0 ? std::string_view("auto") : kGamepadOverlayArtworkIds[layout];
        EXPECT_EQ(source->settings["layout"].value<std::string>(), expected);
    }
}

TEST(VisualPresetMigration, DisabledNondefaultCameraAndGamepadIntentSurvives) {
    WebcamSettings webcam;
    webcam.device_id = "missing-camera";
    webcam.width = 1920;
    webcam.height = 1080;
    webcam.fps = 60;
    webcam.controls = {{WebcamControl::Exposure, false, -5},
                       {WebcamControl::Brightness, false, 1},
                       {WebcamControl::Brightness, true, -12}};
    webcam.overlay = {0.1f, 0.2f, 0.4f, 0.3f};
    webcam.overlay_user_placed = true;
    webcam.aspect_ratio_locked = false;
    webcam.mirror = true;
    webcam.opacity = 0.37f;
    webcam.output_visible = false;
    webcam.chroma_key.enabled = true;
    webcam.chroma_key.color_mode = WebcamChromaKeyColorMode::Custom;
    webcam.chroma_key.custom_r = 21;
    webcam.chroma_key.custom_g = 43;
    webcam.chroma_key.custom_b = 65;
    webcam.chroma_key.tolerance = 0.22f;
    webcam.chroma_key.softness = 0.33f;
    webcam.chroma_key.spill_reduction = 0.44f;
    GamepadOverlaySettings pad;
    pad.device = std::string(64, 'a');
    pad.layout = 19;
    pad.opacity = 0.61f;
    auto migrated = MigrateLegacyVisualConfig(webcam, pad);
    ASSERT_TRUE(migrated.succeeded());
    const auto* camera = Source(*migrated.value(), "org.exosnap.webcam/camera");
    const auto* controller = Source(*migrated.value(), "org.exosnap.gamepad/controller");
    ASSERT_NE(camera, nullptr);
    ASSERT_NE(controller, nullptr);
    EXPECT_FALSE(camera->enabled);
    EXPECT_FALSE(controller->enabled);
    EXPECT_EQ(camera->settings["device_id"].value<std::string>(), webcam.device_id);
    EXPECT_EQ(camera->settings["width"].value<int64_t>(), 1920);
    EXPECT_EQ(camera->settings["height"].value<int64_t>(), 1080);
    EXPECT_EQ(camera->settings["fps"].value<int64_t>(), 60);
    EXPECT_EQ(camera->settings["controls"]["brightness"]["value"].value<int64_t>(), -12);
    EXPECT_EQ(camera->settings["controls"]["brightness"]["auto"].value<bool>(), true);
    EXPECT_EQ(camera->settings["controls"]["exposure"]["value"].value<int64_t>(), -5);
    EXPECT_EQ(controller->settings["device"].value<std::string>(), pad.device);
    EXPECT_EQ(controller->settings["layout"].value<std::string>(), "xbox_elite");
    const auto* item = Item(*migrated.value(), camera->id);
    ASSERT_NE(item, nullptr);
    EXPECT_FLOAT_EQ(item->properties.placement.x, webcam.overlay.x_norm);
    EXPECT_FLOAT_EQ(item->properties.placement.y, webcam.overlay.y_norm);
    EXPECT_FLOAT_EQ(item->properties.placement.w, webcam.overlay.w_norm);
    EXPECT_FLOAT_EQ(item->properties.placement.h, webcam.overlay.h_norm);
    EXPECT_FLOAT_EQ(item->properties.opacity, webcam.opacity);
    EXPECT_TRUE(item->properties.placement.mirror);
    EXPECT_TRUE(item->properties.user_placed);
    EXPECT_FALSE(item->properties.aspect_ratio_locked);
    EXPECT_FALSE(item->properties.output_visible);
    EXPECT_EQ(item->chroma_key, webcam.chroma_key);
    auto written = WriteVisualScene(*migrated.value());
    ASSERT_TRUE(written.succeeded());
    auto read = ReadVisualScene(*written.value());
    ASSERT_TRUE(read.succeeded());
    auto rewritten = WriteVisualScene(*read.value());
    ASSERT_TRUE(rewritten.succeeded());
    EXPECT_EQ(*written.value(), *rewritten.value());
}

TEST(VisualPresetMigration, LegacySanitizersKeepTheirDistinctGeometryRules) {
    WebcamSettings webcam;
    webcam.overlay = {0.9f, 0, 0.5f, 0.25f};
    webcam.width = -1;
    webcam.opacity = std::numeric_limits<float>::quiet_NaN();
    GamepadOverlaySettings pad;
    pad.x = 0.9f;
    pad.width = 0.5f;
    pad.device = std::string(64, 'A');
    pad.layout = 99;
    auto migrated = MigrateLegacyVisualConfig(webcam, pad);
    ASSERT_TRUE(migrated.succeeded());
    const auto* camera = Source(*migrated.value(), "org.exosnap.webcam/camera");
    const auto* controller = Source(*migrated.value(), "org.exosnap.gamepad/controller");
    ASSERT_NE(camera, nullptr);
    ASSERT_NE(controller, nullptr);
    const auto* camera_item = Item(*migrated.value(), camera->id);
    const auto* pad_item = Item(*migrated.value(), controller->id);
    ASSERT_NE(camera_item, nullptr);
    ASSERT_NE(pad_item, nullptr);
    EXPECT_NEAR(camera_item->properties.placement.w, 0.1f, 0.000001f);
    EXPECT_FLOAT_EQ(pad_item->properties.placement.x, 0.5f);
    EXPECT_FLOAT_EQ(camera_item->properties.opacity, 1.0f);
    EXPECT_EQ(camera->settings["width"].value<int64_t>(), 1280);
    EXPECT_EQ(controller->settings["device"].value<std::string>(), "");
    EXPECT_EQ(controller->settings["layout"].value<std::string>(), "generic");
}

TEST(VisualPresetMigration, StableIdsOnSaveRetry) {
    auto migrated = MigrateLegacyVisualConfig({}, {});
    ASSERT_TRUE(migrated.succeeded());
    ASSERT_EQ(migrated.value()->sources.size(), 2u);
    ASSERT_EQ(migrated.value()->items.size(), 2u);
    EXPECT_NE(migrated.value()->sources[0].id, migrated.value()->sources[1].id);
    EXPECT_NE(migrated.value()->items[0].id, migrated.value()->items[1].id);
    auto first = WriteVisualScene(*migrated.value());
    auto retry = WriteVisualScene(*migrated.value());
    ASSERT_TRUE(first.succeeded());
    ASSERT_TRUE(retry.succeeded());
    EXPECT_EQ(*first.value(), *retry.value());
}

TEST(VisualPresetMigration, LegacyProjectionPreservesIdentitiesAcrossEditsAndRetries) {
    auto migrated = MigrateLegacyVisualConfig({}, {});
    ASSERT_TRUE(migrated.succeeded());
    auto identities = *migrated.value();
    std::reverse(identities.sources.begin(), identities.sources.end());
    std::reverse(identities.items.begin(), identities.items.end());
    const auto before = WriteVisualScene(identities);
    ASSERT_TRUE(before.succeeded());
    WebcamSettings camera;
    camera.enabled = true;
    camera.device_id = "camera-b";
    camera.controls = {{WebcamControl::Brightness, false, 7}};
    camera.overlay = {0.1f, 0.2f, 0.3f, 0.4f};
    GamepadOverlaySettings pad;
    pad.enabled = true;
    pad.layout = 1;
    auto projected = ProjectLegacyVisualConfig(camera, pad, identities);
    auto retry = ProjectLegacyVisualConfig(camera, pad, identities);
    ASSERT_TRUE(projected.succeeded());
    ASSERT_TRUE(retry.succeeded());
    for (const auto& source : identities.sources) {
        const auto* actual = Source(*projected.value(), source.type_id);
        ASSERT_NE(actual, nullptr);
        EXPECT_EQ(actual->id, source.id);
        const auto* actual_item = Item(*projected.value(), source.id);
        const auto* original_item = Item(identities, source.id);
        ASSERT_NE(actual_item, nullptr);
        ASSERT_NE(original_item, nullptr);
        EXPECT_EQ(actual_item->id, original_item->id);
    }
    const auto* actual_camera = Source(*projected.value(), "org.exosnap.webcam/camera");
    ASSERT_NE(actual_camera, nullptr);
    EXPECT_TRUE(actual_camera->enabled);
    EXPECT_EQ(actual_camera->settings["device_id"].value<std::string>(), camera.device_id);
    EXPECT_EQ(actual_camera->settings["controls"]["brightness"]["value"].value<int64_t>(), 7);
    const auto* actual_item = Item(*projected.value(), actual_camera->id);
    ASSERT_NE(actual_item, nullptr);
    EXPECT_FLOAT_EQ(actual_item->properties.placement.x, camera.overlay.x_norm);
    auto written = WriteVisualScene(*projected.value());
    auto written_retry = WriteVisualScene(*retry.value());
    auto unchanged = WriteVisualScene(identities);
    ASSERT_TRUE(written.succeeded());
    ASSERT_TRUE(written_retry.succeeded());
    ASSERT_TRUE(unchanged.succeeded());
    EXPECT_EQ(*written.value(), *written_retry.value());
    EXPECT_EQ(*before.value(), *unchanged.value());
}

TEST(VisualPresetMigration, LegacyProjectionRejectsMissingSourceAndItemIdentities) {
    const auto identities = MakeDefaultVisualScene();
    auto missing_source = identities;
    missing_source.sources.pop_back();
    EXPECT_FALSE(ProjectLegacyVisualConfig({}, {}, missing_source).succeeded());
    auto missing_item = identities;
    missing_item.items.pop_back();
    EXPECT_FALSE(ProjectLegacyVisualConfig({}, {}, missing_item).succeeded());
}

TEST(VisualPresetMigration, DefaultHydrationUsesStableDisabledIdentities) {
    const auto first = MakeDefaultVisualScene();
    const auto second = MakeDefaultVisualScene();
    ASSERT_EQ(first.sources.size(), 2u);
    ASSERT_EQ(first.items.size(), 2u);
    for (size_t index = 0; index < first.sources.size(); ++index) {
        EXPECT_FALSE(first.sources[index].enabled);
        EXPECT_EQ(first.sources[index].id, second.sources[index].id);
        EXPECT_NE(first.sources[index].id, engine::VisualSourceId{});
        EXPECT_EQ(first.items[index].id, second.items[index].id);
    }
    auto written = WriteVisualScene(first);
    auto again = WriteVisualScene(second);
    ASSERT_TRUE(written.succeeded());
    ASSERT_TRUE(again.succeeded());
    EXPECT_EQ(*written.value(), *again.value());
}

TEST(VisualPresetMigration, UnknownSettingsSurviveKnownEditAndFutureTypesRoundTrip) {
    auto scene = MakeDefaultVisualScene();
    ASSERT_FALSE(scene.sources.empty());
    scene.sources.front().type_id = "future/image";
    scene.sources.front().settings_version = 91;
    scene.sources.front().settings.insert("vendor", toml::table{{"future", 17}});
    auto written = WriteVisualScene(scene);
    ASSERT_TRUE(written.succeeded());
    auto read = ReadVisualScene(*written.value());
    ASSERT_TRUE(read.succeeded());
    read.value()->items.front().properties.opacity = 0.5f;
    auto edited = WriteVisualScene(*read.value());
    ASSERT_TRUE(edited.succeeded());
    auto final_scene = ReadVisualScene(*edited.value());
    ASSERT_TRUE(final_scene.succeeded());
    const auto& source = final_scene.value()->sources.front();
    EXPECT_EQ(source.type_id, "future/image");
    EXPECT_EQ(source.settings_version, 91u);
    EXPECT_EQ(source.settings["vendor"]["future"].value<int64_t>(), 17);
}

TEST(VisualPresetMigration, RetiredLegacyEffectsAreNotEmittedAndClearedControlsStayEmpty) {
    GamepadOverlaySettings pad;
    pad.glass = true;
    pad.glow = true;
    WebcamSettings camera;
    camera.controls = {{WebcamControl::Brightness, false, 10}};
    auto migrated = MigrateLegacyVisualConfig(camera, pad);
    ASSERT_TRUE(migrated.succeeded());
    for (auto& source : migrated.value()->sources) {
        if (source.type_id == "org.exosnap.gamepad/controller") {
            EXPECT_FALSE(source.settings.contains("glass"));
            EXPECT_FALSE(source.settings.contains("glow"));
        } else {
            source.settings.insert_or_assign("controls", toml::table{});
        }
    }
    auto written = WriteVisualScene(*migrated.value());
    ASSERT_TRUE(written.succeeded());
    auto read = ReadVisualScene(*written.value());
    ASSERT_TRUE(read.succeeded());
    const auto* source = Source(*read.value(), "org.exosnap.webcam/camera");
    ASSERT_NE(source, nullptr);
    const auto* controls = source->settings["controls"].as_table();
    ASSERT_NE(controls, nullptr);
    EXPECT_TRUE(controls->empty());
}

TEST(VisualPresetMigration, MalformedDuplicateAndDanglingIdsRejectWholeRead) {
    auto written = WriteVisualScene(MakeDefaultVisualScene());
    ASSERT_TRUE(written.succeeded());
    for (const auto& id : {"bad-uuid", "00000000-0000-0000-0000-000000000000"}) {
        auto invalid = *written.value();
        invalid["sources"].as_array()->get(0)->as_table()->insert_or_assign("id", id);
        EXPECT_FALSE(ReadVisualScene(invalid).succeeded());
    }
    auto duplicate = *written.value();
    auto* sources = duplicate["sources"].as_array();
    sources->push_back(*sources->get(0)->as_table());
    EXPECT_FALSE(ReadVisualScene(duplicate).succeeded());
    auto dangling = *written.value();
    dangling["items"].as_array()->get(0)->as_table()->insert_or_assign("source_id",
                                                                       "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    EXPECT_FALSE(ReadVisualScene(dangling).succeeded());
}

TEST(VisualPresetMigration, InvalidCurrentFieldTypesAndNonfiniteGeometryAreRejected) {
    auto written = WriteVisualScene(MakeDefaultVisualScene());
    ASSERT_TRUE(written.succeeded());
    auto wrong_type = *written.value();
    wrong_type["sources"].as_array()->get(0)->as_table()->insert_or_assign("enabled", "true");
    EXPECT_FALSE(ReadVisualScene(wrong_type).succeeded());
    auto nonfinite = *written.value();
    nonfinite["items"].as_array()->get(0)->as_table()->insert_or_assign("x", std::numeric_limits<double>::infinity());
    EXPECT_FALSE(ReadVisualScene(nonfinite).succeeded());
    auto invalid = MakeDefaultVisualScene();
    invalid.items.front().properties.opacity = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(WriteVisualScene(invalid).succeeded());
}

TEST(VisualPresetMigration, EmptyNativeSceneDoesNotRecreateRemovedSources) {
    auto read = ReadVisualScene(toml::table{});
    ASSERT_TRUE(read.succeeded());
    EXPECT_TRUE(read.value()->sources.empty());
    EXPECT_TRUE(read.value()->items.empty());
    EXPECT_FALSE(ReadVisualScene(toml::table{{"sources", "invalid"}}).succeeded());
}

TEST(VisualPresetMigration, IntegerBooleanFieldsAreRejected) {
    auto written = WriteVisualScene(MakeDefaultVisualScene());
    ASSERT_TRUE(written.succeeded());
    for (const int value : {0, 1, 2}) {
        auto source = *written.value();
        source["sources"].as_array()->get(0)->as_table()->insert_or_assign("enabled", value);
        EXPECT_FALSE(ReadVisualScene(source).succeeded());
    }
    auto item = *written.value();
    item["items"].as_array()->get(0)->as_table()->insert_or_assign("output_visible", 1);
    EXPECT_FALSE(ReadVisualScene(item).succeeded());
    auto chroma = *written.value();
    auto* chroma_table = chroma["items"].as_array()->get(0)->as_table()->get("chroma_key")->as_table();
    chroma_table->insert_or_assign("enabled", 1);
    EXPECT_FALSE(ReadVisualScene(chroma).succeeded());
    EXPECT_TRUE(ReadVisualScene(*written.value()).succeeded());
}

TEST(VisualPresetMigration, MissingSettingsVersionCannotActivateAsCurrentSchema) {
    auto written = WriteVisualScene(MakeDefaultVisualScene());
    ASSERT_TRUE(written.succeeded());
    written.value()->get("sources")->as_array()->get(0)->as_table()->erase("settings_version");
    EXPECT_FALSE(ReadVisualScene(*written.value()).succeeded());
}

} // namespace
} // namespace exosnap
