#include "models/RecordingPreset.h"
#include "models/RecordingPresetRegistry.h"
#include "settings/RecordingPresetStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include <sstream>

namespace exosnap {
namespace {

class VisualPersistenceIdentity : public ::testing::Test {
  protected:
    QTemporaryDir directory;
    QString path = directory.filePath(QStringLiteral("presets.toml"));
    RecordingPresetStore store{path};

    QByteArray bytes() const {
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    }
    void write(const toml::table& doc) {
        std::ostringstream text;
        text << doc;
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        const auto data = QByteArray::fromStdString(text.str());
        ASSERT_EQ(file.write(data), data.size());
    }
    toml::table read() const {
        return toml::parse_file(path.toStdString());
    }
    static void legacy(toml::table& config) {
        const auto* visual = config["visual"].as_table();
        const auto& camera = *(*(*visual)["sources"].as_array())[0].as_table();
        const auto& pad = *(*(*visual)["sources"].as_array())[1].as_table();
        const auto& camera_item = *(*(*visual)["items"].as_array())[0].as_table();
        const auto& pad_item = *(*(*visual)["items"].as_array())[1].as_table();
        auto webcam = *camera["settings"].as_table();
        webcam.insert_or_assign("enabled", camera["enabled"].value_or(false));
        webcam.insert_or_assign("overlay_x", camera_item["x"].value_or(0.0));
        webcam.insert_or_assign("overlay_y", camera_item["y"].value_or(0.0));
        webcam.insert_or_assign("overlay_w", camera_item["width"].value_or(0.25));
        webcam.insert_or_assign("overlay_h", camera_item["height"].value_or(0.25));
        webcam.insert_or_assign("mirror", camera_item["mirror"].value_or(false));
        webcam.insert_or_assign("opacity", camera_item["opacity"].value_or(1.0));
        auto gamepad = *pad["settings"].as_table();
        gamepad.insert_or_assign("enabled", pad["enabled"].value_or(false));
        gamepad.insert_or_assign("layout", -1);
        gamepad.insert_or_assign("opacity", pad_item["opacity"].value_or(1.0));
        gamepad.insert_or_assign("x", pad_item["x"].value_or(0.7));
        gamepad.insert_or_assign("y", pad_item["y"].value_or(0.65));
        gamepad.insert_or_assign("width", pad_item["width"].value_or(0.25));
        gamepad.insert_or_assign("height", pad_item["height"].value_or(0.3));
        config.insert_or_assign("webcam", std::move(webcam));
        config.insert_or_assign("gamepad", std::move(gamepad));
        config.erase("visual");
    }
    static void legacyStamp(toml::table& doc) {
        doc.insert_or_assign("schema_version", 26);
        doc.erase("visual_contract");
    }
};

TEST_F(VisualPersistenceIdentity, DefaultHydrationDoesNotBecomeDirty) {
    ASSERT_TRUE(store.Save({}, std::string(kDefaultPresetId), MakeDefaultPreset().config));
    auto doc = read();
    legacyStamp(doc);
    legacy(*doc["live"].as_table());
    write(doc);
    const auto original = bytes();
    const auto state = store.Load();
    ASSERT_TRUE(state.live);
    ASSERT_FALSE(state.read_only);
    RecordingPresetRegistry registry;
    registry.LoadState(state.user_presets, state.selected_id);
    EXPECT_FALSE(registry.IsSelectedDirty(*state.live));
    EXPECT_EQ(state.live->visual_scene.sources.front().id, MakeDefaultPreset().config.visual_scene.sources.front().id);
    EXPECT_EQ(bytes(), original);
    ASSERT_TRUE(store.Save(state.user_presets, state.selected_id, *state.live));
    const auto reloaded = store.Load();
    ASSERT_TRUE(reloaded.live);
    EXPECT_FALSE(registry.IsSelectedDirty(*reloaded.live));
}

TEST_F(VisualPersistenceIdentity, SelectedLegacyNamedAndLiveShareStagedIdentities) {
    RecordingPreset preset{"preset.identity", "Identity", MakeDefaultPreset().config};
    preset.config.video.cq = 27;
    preset.config.visual_scene.sources.front().settings.insert_or_assign("device_id", "unavailable camera");
    ASSERT_TRUE(store.Save({preset}, preset.id, preset.config));
    auto doc = read();
    legacyStamp(doc);
    legacy(*doc["live"].as_table());
    legacy(*(*doc["presets"].as_array())[0].as_table());
    write(doc);
    const auto state = store.Load();
    ASSERT_TRUE(state.live);
    ASSERT_EQ(state.user_presets.size(), 1u);
    RecordingPresetRegistry registry;
    registry.LoadState(state.user_presets, state.selected_id);
    EXPECT_FALSE(registry.IsSelectedDirty(*state.live));
    EXPECT_EQ(state.live->visual_scene.sources.front().id,
              state.user_presets.front().config.visual_scene.sources.front().id);
    EXPECT_NE(state.live->visual_scene.sources.front().id, MakeDefaultPreset().config.visual_scene.sources.front().id);
    auto changed = *state.live;
    changed.visual_scene.sources.front().settings.insert_or_assign("device_id", "different camera");
    EXPECT_TRUE(registry.IsSelectedDirty(changed));
}

TEST_F(VisualPersistenceIdentity, SelectedNativePilotIdentitiesAreRetainedByLegacyLive) {
    RecordingPreset preset{"preset.native", "Native", MakeDefaultPreset().config};
    auto& scene = preset.config.visual_scene;
    for (auto& source : scene.sources) {
        const auto previous = source.id;
        source.id = engine::NewId<engine::VisualSourceId>();
        for (auto& item : scene.items)
            if (item.source == previous)
                item.source = source.id;
    }
    for (auto& item : scene.items)
        item.id = engine::NewId<engine::SceneItemId>();
    ASSERT_TRUE(store.Save({preset}, preset.id, preset.config));
    auto doc = read();
    legacyStamp(doc);
    legacy(*doc["live"].as_table());
    write(doc);
    const auto state = store.Load();
    ASSERT_TRUE(state.live);
    ASSERT_EQ(state.user_presets.size(), 1u);
    EXPECT_TRUE(ConfigDirtyEquivalent(*state.live, state.user_presets.front().config));
    EXPECT_EQ(state.user_presets.front().config.visual_scene.sources.front().id, scene.sources.front().id);
    EXPECT_EQ(state.live->visual_scene.sources.front().id, scene.sources.front().id);
}

TEST_F(VisualPersistenceIdentity, OpaqueNativeSceneAndRawUnknownFieldsSurviveUnrelatedSave) {
    auto live = MakeDefaultPreset().config;
    live.visual_scene.sources.push_back({engine::NewId<engine::VisualSourceId>(), "org.vendor.future/image", 99, true,
                                         toml::table{{"nested", toml::table{{"answer", 42}}}}});
    ASSERT_TRUE(store.Save({}, std::string(kDefaultPresetId), live));
    auto doc = read();
    auto& source = *doc["live"]["visual"]["sources"].as_array()->back().as_table();
    source.insert("vendor_top", toml::array{1, 2, 3});
    auto& item = *doc["live"]["visual"]["items"].as_array()->front().as_table();
    item.insert("vendor_item", "keep");
    write(doc);
    const auto expected_sources = *doc["live"]["visual"]["sources"].as_array();
    auto state = store.Load();
    ASSERT_TRUE(state.live);
    ASSERT_FALSE(state.read_only);
    state.live->video.cq = 29;
    ASSERT_TRUE(store.Save(state.user_presets, state.selected_id, *state.live));
    const auto saved = read();
    EXPECT_EQ(*saved["live"]["visual"]["sources"].as_array(), expected_sources);
    const auto& saved_item = *(*saved["live"]["visual"]["items"].as_array())[0].as_table();
    EXPECT_EQ(saved_item["vendor_item"].value<std::string>(), "keep");
}

TEST_F(VisualPersistenceIdentity, ArbitraryNativeSelectedSceneIsNeverReboundToLegacyPilots) {
    RecordingPreset preset{"preset.future", "Future", MakeDefaultPreset().config};
    preset.config.visual_scene.sources.push_back({engine::NewId<engine::VisualSourceId>(), "org.vendor.future/image",
                                                  99, true, toml::table{{"future", "keep"}}});
    ASSERT_TRUE(store.Save({preset}, preset.id, preset.config));
    auto doc = read();
    legacyStamp(doc);
    legacy(*doc["live"].as_table());
    const auto expected = *(*doc["presets"].as_array())[0].as_table()->get("visual")->as_table();
    write(doc);
    const auto state = store.Load();
    ASSERT_TRUE(state.live);
    ASSERT_EQ(state.user_presets.size(), 1u);
    const auto native = WriteVisualScene(state.user_presets.front().config.visual_scene);
    ASSERT_TRUE(native.succeeded());
    EXPECT_EQ(*native.value(), expected);
    EXPECT_NE(state.live->visual_scene.sources.front().id, preset.config.visual_scene.sources.front().id);
    EXPECT_FALSE(ConfigDirtyEquivalent(*state.live, state.user_presets.front().config));
}

TEST_F(VisualPersistenceIdentity, LockAndBackupFailurePreserveOriginalBytesAndStagedIdentities) {
    ASSERT_TRUE(store.Save({}, std::string(kDefaultPresetId), MakeDefaultPreset().config));
    auto doc = read();
    legacyStamp(doc);
    legacy(*doc["live"].as_table());
    write(doc);
    const auto original = bytes();
    const auto state = store.Load();
    ASSERT_TRUE(state.live);
    const auto identities = WriteVisualScene(state.live->visual_scene);
    ASSERT_TRUE(identities.succeeded());
    QLockFile competing(path + QStringLiteral(".lock"));
    ASSERT_TRUE(competing.tryLock(0));
    QString error;
    EXPECT_FALSE(store.Save(state.user_presets, state.selected_id, *state.live, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(bytes(), original);
    competing.unlock();
    const auto hash = QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex();
    const auto backup_path = path + QStringLiteral(".schema26.%1.bak").arg(QString::fromLatin1(hash));
    ASSERT_TRUE(QDir().mkdir(backup_path));
    EXPECT_FALSE(store.Save(state.user_presets, state.selected_id, *state.live, &error));
    EXPECT_EQ(bytes(), original);
    const auto after_failure = WriteVisualScene(state.live->visual_scene);
    ASSERT_TRUE(after_failure.succeeded());
    EXPECT_EQ(*after_failure.value(), *identities.value());
    ASSERT_TRUE(QDir().rmdir(backup_path));
    ASSERT_TRUE(store.Save(state.user_presets, state.selected_id, *state.live));
    const auto reloaded = store.Load();
    ASSERT_TRUE(reloaded.live);
    EXPECT_EQ(reloaded.live->visual_scene.sources.front().id, state.live->visual_scene.sources.front().id);
}

TEST_F(VisualPersistenceIdentity, LegacyImportAllocatesOneFreshTemplatePerBatch) {
    RecordingPreset first{"preset.first", "First", MakeDefaultPreset().config};
    auto second = first;
    second.id = "preset.second";
    second.name = "Second";
    ASSERT_TRUE(store.Save({first, second}, first.id, first.config));
    auto doc = read();
    legacyStamp(doc);
    for (auto& entry : *doc["presets"].as_array())
        legacy(*entry.as_table());
    write(doc);
    const auto initial = RecordingPresetStore::ImportPresetsFromFile(path, {}, nullptr);
    const auto next = RecordingPresetStore::ImportPresetsFromFile(path, {}, nullptr);
    ASSERT_EQ(initial.size(), 2);
    ASSERT_EQ(next.size(), 2);
    EXPECT_EQ(initial[0].config.visual_scene.sources.front().id, initial[1].config.visual_scene.sources.front().id);
    EXPECT_NE(initial[0].config.visual_scene.sources.front().id, next[0].config.visual_scene.sources.front().id);
}

} // namespace
} // namespace exosnap
