#include "SettingsAdapter.h"

#include <QGuiApplication>

#include <gtest/gtest.h>

namespace exosnap::quick {
namespace {

class SettingsVisualCutover : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char name[] = "settings_visual_cutover";
            static char* argv[] = {name, nullptr};
            static QGuiApplication application(argc, argv);
        }
    }
    void SetUp() override {
        SourceFactory unused = [](SourceCreateRequest) -> engine::Outcome<std::unique_ptr<VisualSource>> {
            return engine::EngineError{.context = "Source creation is unavailable in this test"};
        };
        CachedVisualOptions devices = [](const VisualSourceRecord&) {
            return ChoiceSnapshot{1,
                                  {{"", {"VisualTests", "Automatic"}},
                                   {"camera", {"VisualTests", "Camera"}},
                                   {"blocked", {"VisualTests", "Blocked"}, false}}};
        };
        CachedVisualOptions layouts = [](const VisualSourceRecord&) {
            return ChoiceSnapshot{
                1, {{"auto", {"VisualTests", "Automatic"}}, {"xbox_elite", {"VisualTests", "Xbox Elite"}}}};
        };
        ASSERT_TRUE(
            registry.Register({{"org.exosnap.webcam"}, {MakeWebcamVisualSourceType(unused, devices)}}).succeeded());
        ASSERT_TRUE(
            registry.Register({{"org.exosnap.gamepad"}, {MakeGamepadVisualSourceType(unused, devices, layouts)}})
                .succeeded());
        ASSERT_TRUE(registry.Seal().succeeded());
        config = MakeDefaultPreset().config;
        config.visual_scene = {};
        config.visual_scene.sources.push_back({engine::VisualSourceId{{1}}, "org.exosnap.webcam/camera", 1, true, {}});
        config.visual_scene.sources.push_back(
            {engine::VisualSourceId{{3}}, "org.exosnap.gamepad/controller", 1, true, {}});
        SceneItemRecord webcam_item;
        webcam_item.id = engine::SceneItemId{{2}};
        webcam_item.source = engine::VisualSourceId{{1}};
        config.visual_scene.items.push_back(webcam_item);
        webcam_item.id = engine::SceneItemId{{4}};
        webcam_item.source = engine::VisualSourceId{{3}};
        config.visual_scene.items.push_back(webcam_item);
        adapter.setVisualRegistry(&registry);
        adapter.setVisualTargets({engine::VisualSourceId{{1}}, {}}, engine::SceneItemId{{2}},
                                 {engine::VisualSourceId{{3}}, {}}, engine::SceneItemId{{4}});
        adapter.setConfig(config);
        QObject::connect(&adapter, &SettingsAdapter::configEdited, &adapter, [this] { ++edits; });
        QObject::connect(&adapter, &SettingsAdapter::activeVisualSceneEdited, &adapter, [this] { ++active_edits; });
    }
    QVariantMap State(bool gamepad = false) {
        return adapter.visualSourceState(gamepad ? adapter.gamepadSourceId() : adapter.webcamSourceId());
    }
    bool Select(const QVariantMap& state, const char* action, const char* choice) {
        return adapter.selectVisualSetting(state["sourceId"].toString(), state["itemId"].toString(),
                                           state["editRevision"].toString(), QString::fromLatin1(action),
                                           QString::fromLatin1(choice));
    }
    CapabilityRegistry registry;
    SettingsAdapter adapter;
    RecordingPresetConfig config;
    int edits = 0;
    int active_edits = 0;
};

TEST_F(SettingsVisualCutover, HydrationAndRowsNeverWriteIntentOrRequireNativeRuntime) {
    adapter.setConfig(config);
    EXPECT_EQ(edits, 0);
    EXPECT_EQ(adapter.config().visual_scene.sources.front().settings, config.visual_scene.sources.front().settings);
    EXPECT_FALSE(adapter.visualSettingsRows(adapter.webcamSourceId()).isEmpty());
    EXPECT_EQ(edits, 0);
    EXPECT_TRUE(Select(State(), "select-device", "camera"));
    EXPECT_EQ(edits, 1);
    EXPECT_EQ(adapter.webcamDeviceId(), QStringLiteral("camera"));
}

TEST_F(SettingsVisualCutover, RejectedChoiceStaleRowAndUnchangedSelectionPublishNoEdit) {
    const auto stale = State();
    EXPECT_FALSE(Select(stale, "select-device", "blocked"));
    EXPECT_FALSE(Select(stale, "select-device", "missing"));
    EXPECT_TRUE(Select(stale, "select-device", ""));
    EXPECT_TRUE(Select(State(true), "select-layout", "auto"));
    EXPECT_EQ(edits, 0);
    adapter.setConfig(config);
    EXPECT_FALSE(Select(stale, "select-device", "camera"));
    EXPECT_EQ(edits, 0);
}

TEST_F(SettingsVisualCutover, LiveSelectionPreservesNextRecordingAndNextSessionSelectionIsLocked) {
    adapter.setControlsLocked(true);
    EXPECT_FALSE(Select(State(), "select-device", "camera"));
    EXPECT_TRUE(Select(State(true), "select-layout", "xbox_elite"));
    EXPECT_EQ(active_edits, 1);
    EXPECT_EQ(edits, 0);
    ASSERT_NE(adapter.activeVisualScene(), nullptr);
    EXPECT_EQ(adapter.activeVisualScene()->sources[1].settings["layout"].value<std::string>(), "xbox_elite");
    EXPECT_FALSE(adapter.config().visual_scene.sources[1].settings.contains("layout"));
    adapter.editNextRecording();
    EXPECT_TRUE(Select(State(), "select-device", "camera"));
    EXPECT_EQ(edits, 1);
    EXPECT_FALSE(adapter.activeVisualScene()->sources.front().settings.contains("device_id"));
}

TEST_F(SettingsVisualCutover, RuntimeActionsRejectZeroOrStaleIncarnationBeforeDispatch) {
    int retries = 0;
    QObject::connect(&adapter, &SettingsAdapter::visualRetryRequested, &adapter, [&](const auto&) { ++retries; });
    const auto state = State();
    EXPECT_FALSE(adapter.dispatchVisualAction(state["sourceId"].toString(), state["itemId"].toString(),
                                              QStringLiteral("0"), QStringLiteral("retry")));
    adapter.setVisualTargets({engine::VisualSourceId{{1}}, engine::VisualRuntimeGeneration{7}},
                             engine::SceneItemId{{2}}, {engine::VisualSourceId{{3}}, {}}, engine::SceneItemId{{4}});
    EXPECT_FALSE(adapter.dispatchVisualAction(state["sourceId"].toString(), state["itemId"].toString(),
                                              QStringLiteral("6"), QStringLiteral("retry")));
    EXPECT_TRUE(adapter.dispatchVisualAction(state["sourceId"].toString(), state["itemId"].toString(),
                                             QStringLiteral("7"), QStringLiteral("retry")));
    EXPECT_EQ(retries, 1);
    EXPECT_EQ(edits, 0);
}

TEST_F(SettingsVisualCutover, NativeControlsClampAndResetPreservesFutureControlNames) {
    config.visual_scene.sources.front().settings.insert("controls", toml::table{{"future_control", true}});
    adapter.setConfig(config);
    adapter.setWebcamControlRanges({}, {{.control = WebcamControl::Brightness,
                                         .minimum = 0,
                                         .maximum = 100,
                                         .step = 10,
                                         .current = 40,
                                         .can_automatic = true}});
    EXPECT_FALSE(adapter.webcamControlsAdjusted());
    adapter.setWebcamControlValue(static_cast<int>(WebcamControl::Brightness), 57);
    EXPECT_EQ(edits, 1);
    EXPECT_EQ(adapter.webcamSettings().controls.front().value, 60);
    adapter.setWebcamControlAutomatic(static_cast<int>(WebcamControl::Brightness), true);
    EXPECT_EQ(edits, 2);
    EXPECT_TRUE(adapter.webcamSettings().controls.front().automatic);
    adapter.resetWebcamControls();
    EXPECT_EQ(edits, 3);
    EXPECT_FALSE(adapter.webcamControlsAdjusted());
    EXPECT_TRUE(adapter.config().visual_scene.sources.front().settings["controls"]["future_control"].value_or(false));
}

TEST_F(SettingsVisualCutover, FutureSourceSettingsStayReadOnlyAndStaleScopeCannotEnableSources) {
    const auto stale = State(true);
    adapter.setControlsLocked(true);
    EXPECT_FALSE(adapter.setVisualEnabled(stale["sourceId"].toString(), stale["itemId"].toString(),
                                          stale["editRevision"].toString(), false));
    adapter.setControlsLocked(false);
    config.visual_scene.sources.front().settings_version = 2;
    adapter.setConfig(config);
    EXPECT_TRUE(adapter.visualSettingsRows(adapter.webcamSourceId()).isEmpty());
    adapter.setWebcamMirror(true);
    EXPECT_EQ(edits, 0);
    EXPECT_EQ(active_edits, 0);
    EXPECT_EQ(adapter.config().visual_scene.sources.front().settings_version, 2u);
}

TEST_F(SettingsVisualCutover, ProtectedVisualDocumentRejectsKnownSourceEdits) {
    config.visual_read_only = true;
    adapter.setConfig(config);
    const auto state = State();
    EXPECT_FALSE(state["editable"].toBool());
    EXPECT_FALSE(Select(state, "select-device", "camera"));
    EXPECT_FALSE(adapter.setVisualEnabled(state["sourceId"].toString(), state["itemId"].toString(),
                                          state["editRevision"].toString(), false));
    adapter.setWebcamMirror(true);
    EXPECT_EQ(edits, 0);
    EXPECT_EQ(active_edits, 0);
    EXPECT_TRUE(adapter.config().visual_scene.sources.front().enabled);
    EXPECT_FALSE(adapter.config().visual_scene.items.front().properties.placement.mirror);
}

TEST_F(SettingsVisualCutover, ScopeTransitionRevokesNativeActionsBeforeHydrationSignals) {
    adapter.setControlsLocked(true);
    adapter.setVisualTargets({engine::VisualSourceId{{1}}, engine::VisualRuntimeGeneration{7}},
                             engine::SceneItemId{{2}}, {engine::VisualSourceId{{3}}, {}}, engine::SceneItemId{{4}});
    const auto state = State();
    int callbacks = 0;
    QObject::connect(&adapter, &SettingsAdapter::configChanged, &adapter, [&] {
        ++callbacks;
        EXPECT_FALSE(adapter.dispatchVisualAction(state["sourceId"].toString(), state["itemId"].toString(),
                                                  QStringLiteral("7"), QStringLiteral("retry")));
    });
    adapter.editNextRecording();
    EXPECT_GT(callbacks, 0);
    EXPECT_EQ(State()["runtime"].toString(), QStringLiteral("0"));
}

} // namespace
} // namespace exosnap::quick
