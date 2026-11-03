#include "models/RecordingPreset.h"
#include "settings/RecordingPresetStore.h"

#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <sstream>
#include <toml++/toml.hpp>

namespace exosnap {
namespace {

class VisualPresetStoreCutover : public ::testing::Test {
  protected:
    QTemporaryDir directory;
    QString path = directory.filePath(QStringLiteral("presets.toml"));
    RecordingPresetStore store{path};

    void write(const char* text) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        const QByteArray bytes(text);
        ASSERT_EQ(file.write(bytes), bytes.size());
    }

    toml::table read() const {
        return toml::parse_file(path.toStdString());
    }
};

TEST_F(VisualPresetStoreCutover, KnownSourceSettingsCannotOverwriteAValidDocument) {
    auto config = MakeDefaultPreset().config;
    QString error;
    ASSERT_TRUE(store.Save({}, std::string(kDefaultPresetId), config, &error));
    QFile original(path);
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    const auto bytes = original.readAll();
    original.close();
    config.visual_scene.sources.front().settings.insert_or_assign("fps", 0);
    EXPECT_FALSE(store.Save({}, std::string(kDefaultPresetId), config, &error));
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_EQ(original.readAll(), bytes);
    original.close();
    config = MakeDefaultPreset().config;
    config.visual_scene.sources[1].settings.insert_or_assign("layout", "invalid");
    EXPECT_FALSE(store.Save({}, std::string(kDefaultPresetId), config, &error));
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_EQ(original.readAll(), bytes);
    original.close();
    config.visual_scene.sources[1].settings_version = 2;
    EXPECT_TRUE(store.Save({}, std::string(kDefaultPresetId), config, &error)) << error.toStdString();
}

TEST_F(VisualPresetStoreCutover, MigratesLiveAndNamedWithoutDiscardingUnknownLegacyData) {
    write(R"(
schema_version = 26
audio_contract = "exosnap.audio.v1.3-final.1"
vendor = "keep"
[live.webcam]
enabled = false
device_id = "unavailable camera"
mirror = true
vendor_camera = "keep"
[live.webcam.controls.brightness]
auto = true
value = -12
vendor_control = "keep"
[live.webcam.controls.future_control]
value = 91
[[presets]]
id = "preset.custom"
name = "Custom"
[presets.gamepad]
enabled = false
layout = 19
opacity = 0.37
glass = true
vendor_pad = "keep"
)");
    auto state = store.Load();
    ASSERT_TRUE(state.live);
    ASSERT_EQ(state.user_presets.size(), 1u);
    ASSERT_FALSE(state.read_only);
    const auto& camera = state.live->visual_scene.sources.front();
    EXPECT_FALSE(camera.enabled);
    EXPECT_EQ(camera.settings["device_id"].value<std::string>(), "unavailable camera");
    EXPECT_EQ(camera.settings["controls"]["brightness"]["vendor_control"].value<std::string>(), "keep");
    EXPECT_EQ(camera.settings["controls"]["future_control"]["value"].value<int64_t>(), 91);
    const auto& pad = state.user_presets.front().config.visual_scene.sources[1];
    EXPECT_EQ(pad.settings["layout"].value<std::string>(), "xbox_elite");
    EXPECT_FLOAT_EQ(state.user_presets.front().config.visual_scene.items[1].properties.opacity, 0.37f);
    const auto camera_id = camera.id;
    ASSERT_TRUE(store.Save(state.user_presets, state.selected_id, *state.live));
    auto doc = read();
    EXPECT_EQ(doc["schema_version"].value<int64_t>(), 27);
    EXPECT_EQ(doc["visual_contract"].value<std::string>(), "exosnap.visual.v1");
    EXPECT_EQ(doc["vendor"].value<std::string>(), "keep");
    EXPECT_EQ(doc["live"]["webcam"]["vendor_camera"].value<std::string>(), "keep");
    EXPECT_FALSE(doc["live"]["webcam"]["enabled"]);
    const auto* named = (*doc["presets"].as_array())[0].as_table();
    ASSERT_NE(named, nullptr);
    EXPECT_EQ((*named)["gamepad"]["vendor_pad"].value<std::string>(), "keep");
    EXPECT_FALSE((*named)["gamepad"]["glass"]);
    EXPECT_EQ(store.Load().live->visual_scene.sources.front().id, camera_id);
}

TEST_F(VisualPresetStoreCutover, FailedSaveKeepsStagedIdentitiesForRetry) {
    write(R"(
schema_version = 26
audio_contract = "exosnap.audio.v1.3-final.1"
[live.webcam]
enabled = false
)");
    auto state = store.Load();
    ASSERT_TRUE(state.live);
    const auto id = state.live->visual_scene.sources.front().id;
    QLockFile competing(path + QStringLiteral(".lock"));
    ASSERT_TRUE(competing.tryLock(0));
    QString error;
    EXPECT_FALSE(store.Save({}, state.selected_id, *state.live, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(read()["schema_version"].value<int64_t>(), 26);
    competing.unlock();
    ASSERT_TRUE(store.Save({}, state.selected_id, *state.live, &error));
    EXPECT_EQ(store.Load().live->visual_scene.sources.front().id, id);
}

TEST_F(VisualPresetStoreCutover, ExplicitEmptySceneDoesNotSeedPilots) {
    auto config = MakeDefaultPreset().config;
    config.visual_scene = {};
    ASSERT_TRUE(store.Save({}, std::string(kDefaultPresetId), config));
    auto state = store.Load();
    ASSERT_TRUE(state.live);
    EXPECT_TRUE(state.live->visual_scene.sources.empty());
    EXPECT_TRUE(state.live->visual_scene.items.empty());
    ASSERT_TRUE(store.Save({}, state.selected_id, *state.live));
    EXPECT_TRUE(store.Load().live->visual_scene.sources.empty());
}

TEST_F(VisualPresetStoreCutover, KnownEditsPreserveUnknownRecordsAndDeletionDoesNotResurrect) {
    auto config = MakeDefaultPreset().config;
    ASSERT_TRUE(store.Save({}, std::string(kDefaultPresetId), config));
    auto doc = read();
    auto* sources = doc["live"]["visual"]["sources"].as_array();
    auto* items = doc["live"]["visual"]["items"].as_array();
    (*sources)[0].as_table()->insert("future_source", "keep");
    (*items)[0].as_table()->insert("future_item", "keep");
    std::ostringstream text;
    text << doc;
    write(text.str().c_str());
    auto state = store.Load();
    ASSERT_TRUE(state.live);
    state.live->visual_scene.sources.front().enabled = true;
    state.live->visual_scene.items.front().properties.opacity = 0.3f;
    ASSERT_TRUE(store.Save({}, state.selected_id, *state.live));
    doc = read();
    EXPECT_EQ(doc["live"]["visual"]["sources"][0]["future_source"].value<std::string>(), "keep");
    EXPECT_EQ(doc["live"]["visual"]["items"][0]["future_item"].value<std::string>(), "keep");
    state.live->visual_scene.sources.erase(state.live->visual_scene.sources.begin());
    state.live->visual_scene.items.erase(state.live->visual_scene.items.begin());
    ASSERT_TRUE(store.Save({}, state.selected_id, *state.live));
    EXPECT_EQ(store.Load().live->visual_scene.sources.size(), 1u);
}

TEST_F(VisualPresetStoreCutover, PreviousAudioContractAndFutureVisualContractRemainProtected) {
    write(R"(
schema_version = 26
audio_contract = "future.audio"
[live]
)");
    EXPECT_TRUE(store.Load().read_only);
    write(R"(
schema_version = 27
audio_contract = "exosnap.audio.v1.3-final.1"
visual_contract = "future.visual"
[live]
)");
    EXPECT_TRUE(store.Load().read_only);
}

TEST_F(VisualPresetStoreCutover, ClearingKnownControlsKeepsUnknownControlsWithoutResurrection) {
    auto config = MakeDefaultPreset().config;
    auto& settings = config.visual_scene.sources.front().settings;
    settings.insert_or_assign("controls", toml::table{{"brightness", toml::table{{"auto", false}, {"value", 7}}},
                                                      {"future_control", toml::table{{"value", 91}}}});
    ASSERT_TRUE(store.Save({}, std::string(kDefaultPresetId), config));
    auto state = store.Load();
    ASSERT_TRUE(state.live);
    state.live->visual_scene.sources.front().settings["controls"].as_table()->erase("brightness");
    ASSERT_TRUE(store.Save({}, state.selected_id, *state.live));
    state = store.Load();
    const auto& controls = state.live->visual_scene.sources.front().settings;
    EXPECT_FALSE(controls["controls"]["brightness"]);
    EXPECT_EQ(controls["controls"]["future_control"]["value"].value<int64_t>(), 91);
}

TEST_F(VisualPresetStoreCutover, BuiltinDefaultsKeepStableIdentitiesAndPlacement) {
    auto first = MakeDefaultPreset().config;
    auto second = MakeDefaultPreset().config;
    EXPECT_EQ(first.visual_scene.sources.front().id, second.visual_scene.sources.front().id);
    EXPECT_FLOAT_EQ(first.visual_scene.items.front().properties.placement.x, 0.72f);
    EXPECT_FLOAT_EQ(first.visual_scene.items.front().properties.placement.y, 0.72f);
}

} // namespace
} // namespace exosnap
