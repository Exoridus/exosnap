#include "SettingsAdapter.h"
#include "viewmodels/RecordViewModel.h"

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
        adapter.setRecordVisualEditingAllowed(true);
        adapter.setRecordGamepadTarget({engine::VisualSourceId{{3}}, {}}, engine::SceneItemId{{4}});
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

TEST_F(SettingsVisualCutover, WebcamPreviewReflectsOwnerLifecycleAndRejectsStaleStatus) {
    adapter.setWebcamDevices({QVariantMap{{"value", "camera"}, {"label", "Camera"}}});
    const engine::VisualAddress camera{engine::VisualSourceId{{1}}, engine::VisualRuntimeGeneration{7}};
    adapter.setVisualTargets(camera, engine::SceneItemId{{2}}, {engine::VisualSourceId{{3}}, {}},
                             engine::SceneItemId{{4}});
    EXPECT_EQ(adapter.webcamPreviewState(), SettingsAdapter::CameraInactive);
    SourceStatus status{{camera, engine::SourceRequestRevision{2}}, SourcePhase::Preparing, {}};
    adapter.setWebcamSourceStatus(status);
    EXPECT_EQ(adapter.webcamPreviewState(), SettingsAdapter::CameraStarting);
    status.phase = SourcePhase::Ready;
    adapter.setWebcamSourceStatus(status);
    EXPECT_EQ(adapter.webcamPreviewState(), SettingsAdapter::CameraReady);
    status.phase = SourcePhase::Waiting;
    adapter.setWebcamSourceStatus(status);
    EXPECT_EQ(adapter.webcamPreviewState(), SettingsAdapter::CameraUnavailable);
    EXPECT_FALSE(adapter.webcamPreviewMessage().isEmpty());
    status.phase = SourcePhase::Failed;
    status.reason = engine::EngineError{.context = "Device in use"};
    adapter.setWebcamSourceStatus(status);
    EXPECT_EQ(adapter.webcamPreviewState(), SettingsAdapter::CameraUnavailable);
    EXPECT_TRUE(adapter.webcamPreviewMessage().contains("Device in use"));
    auto stale = status;
    stale.token.request = engine::SourceRequestRevision{1};
    stale.phase = SourcePhase::Ready;
    adapter.setWebcamSourceStatus(stale);
    EXPECT_EQ(adapter.webcamPreviewState(), SettingsAdapter::CameraUnavailable);
    stale = status;
    stale.token.address.runtime = engine::VisualRuntimeGeneration{6};
    adapter.setWebcamSourceStatus(stale);
    EXPECT_EQ(adapter.webcamPreviewState(), SettingsAdapter::CameraUnavailable);
    adapter.setWebcamDevices({});
    EXPECT_EQ(adapter.webcamPreviewState(), SettingsAdapter::CameraDisconnected);
    adapter.setWebcamEnabled(false);
    EXPECT_EQ(adapter.webcamPreviewState(), SettingsAdapter::CameraOff);
    const auto before = edits;
    adapter.setWebcamSourceStatus(std::nullopt);
    EXPECT_EQ(adapter.webcamPreviewState(), SettingsAdapter::CameraOff);
    EXPECT_EQ(edits, before);
}

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

TEST_F(SettingsVisualCutover, GamepadDetailsIsAbsentAndRejectedInSettingsAndRecordScopes) {
    adapter.setVisualTargets(
        {engine::VisualSourceId{{1}}, engine::VisualRuntimeGeneration{7}}, engine::SceneItemId{{2}},
        {engine::VisualSourceId{{3}}, engine::VisualRuntimeGeneration{7}}, engine::SceneItemId{{4}});
    adapter.setRecordGamepadTarget({engine::VisualSourceId{{3}}, engine::VisualRuntimeGeneration{7}},
                                   engine::SceneItemId{{4}});
    int details = 0;
    int retries = 0;
    QObject::connect(&adapter, &SettingsAdapter::visualDetailsRequested, &adapter,
                     [&](const auto&, const auto&) { ++details; });
    QObject::connect(&adapter, &SettingsAdapter::visualRetryRequested, &adapter, [&](const auto&, bool) { ++retries; });

    for (const bool record : {false, true}) {
        const auto state = adapter.visualSourceState(adapter.gamepadSourceId(), record);
        const auto source = state["sourceId"].toString();
        const auto item = state["itemId"].toString();
        const auto runtime = state["runtime"].toString();
        for (const auto& row : adapter.visualSettingsRows(source, record))
            EXPECT_NE(row.toMap()["action"].toString(), QStringLiteral("details"));
        EXPECT_FALSE(adapter.dispatchVisualAction(source, item, runtime, QStringLiteral("details"), record));
        EXPECT_TRUE(adapter.dispatchVisualAction(source, item, runtime, QStringLiteral("retry"), record));
    }
    EXPECT_EQ(details, 0);
    EXPECT_EQ(retries, 2);
    EXPECT_EQ(edits, 0);
    EXPECT_EQ(active_edits, 0);

    const auto webcam = State();
    EXPECT_TRUE(adapter.dispatchVisualAction(webcam["sourceId"].toString(), webcam["itemId"].toString(),
                                             webcam["runtime"].toString(), QStringLiteral("details")));
    EXPECT_EQ(details, 1);
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

TEST_F(SettingsVisualCutover, RecordGamepadEditsActiveSceneWhileNextRecordingRemainsSelected) {
    adapter.setControlsLocked(true);
    adapter.editNextRecording();
    ASSERT_TRUE(Select(State(true), "select-layout", "xbox_elite"));
    const auto next = adapter.displayedConfig();
    const auto state = adapter.visualSourceState(adapter.recordGamepadSourceId(), true);
    const auto source = state["sourceId"].toString();
    const auto item = state["itemId"].toString();
    const auto token = state["editRevision"].toString();
    EXPECT_EQ(state["scope"].toString(), "active");
    EXPECT_TRUE(adapter.setVisualOpacity(source, item, token, 0.5, true));
    EXPECT_TRUE(adapter.setRecordGamepadOutputVisible(source, item, token, false));
    EXPECT_TRUE(adapter.setRecordGamepadPlacement(source, item, token, {0.2, 0.3, 0.4, 0.4}));
    EXPECT_TRUE(adapter.setRecordGamepadPlacement(source, item, token, {0.3, 0.3, 0.4, 0.4}));
    EXPECT_TRUE(adapter.selectVisualSetting(source, item, token, "select-layout", "xbox_elite", true));
    EXPECT_TRUE(adapter.setVisualEnabled(source, item, token, false, true));
    const auto active =
        ProjectSceneGamepad(*adapter.activeVisualScene(), engine::VisualSourceId{{3}}, engine::SceneItemId{{4}});
    ASSERT_TRUE(active.succeeded());
    EXPECT_FALSE(active.value()->enabled);
    EXPECT_FALSE(active.value()->output_visible);
    EXPECT_FLOAT_EQ(active.value()->opacity, 0.5f);
    EXPECT_FLOAT_EQ(active.value()->x, 0.3f);
    EXPECT_TRUE(adapter.editingNextRecording());
    EXPECT_TRUE(NormalizedConfigEquals(next, adapter.displayedConfig()));
    EXPECT_EQ(edits, 1);
    EXPECT_EQ(active_edits, 6);
    adapter.setControlsLocked(false);
    EXPECT_TRUE(NormalizedConfigEquals(next, adapter.config()));
}

TEST_F(SettingsVisualCutover, RecordGestureTokenRejectsDifferentScopeSessionAndRuntime) {
    const auto state = adapter.visualSourceState(adapter.recordGamepadSourceId(), true);
    const auto source = state["sourceId"].toString();
    const auto item = state["itemId"].toString();
    const auto token = state["editRevision"].toString();
    EXPECT_FALSE(adapter.setVisualEnabled(source, item, token, false));
    EXPECT_FALSE(adapter.setRecordGamepadPlacement(source, adapter.webcamSourceId(), token, {0, 0, 0.4, 0.4}));
    EXPECT_FALSE(adapter.setRecordGamepadPlacement(source, item, token, {-0.1, 0, 0.4, 0.4}));
    adapter.setControlsLocked(true);
    EXPECT_FALSE(adapter.setRecordGamepadPlacement(source, item, token, {0, 0, 0.4, 0.4}));
    const auto active = adapter.visualSourceState(source, true);
    adapter.setRecordGamepadTarget({engine::VisualSourceId{{3}}, engine::VisualRuntimeGeneration{8}},
                                   engine::SceneItemId{{4}});
    EXPECT_FALSE(adapter.setRecordGamepadPlacement(source, item, active["editRevision"].toString(), {0, 0, 0.4, 0.4}));
    EXPECT_EQ(edits, 0);
    EXPECT_EQ(active_edits, 0);
}

TEST_F(SettingsVisualCutover, RecordGamepadHasIndependentValidityAndIdleEditsPublishIntent) {
    config.visual_scene.sources.front().settings_version = 2;
    adapter.setConfig(config);
    EXPECT_FALSE(State()["editable"].toBool());
    const auto state = adapter.visualSourceState(adapter.recordGamepadSourceId(), true);
    ASSERT_TRUE(state["editable"].toBool());
    EXPECT_TRUE(adapter.setVisualEnabled(state["sourceId"].toString(), state["itemId"].toString(),
                                         state["editRevision"].toString(), false, true));
    EXPECT_EQ(edits, 1);
    EXPECT_FALSE(adapter.config().visual_scene.sources[1].enabled);
    EXPECT_EQ(adapter.config().visual_scene.sources.front().settings_version, 2u);
}

TEST_F(SettingsVisualCutover, RecordScopeHonorsActiveReadOnlyDocumentWhileNextIsEditable) {
    config.visual_read_only = true;
    adapter.setConfig(config);
    adapter.setControlsLocked(true);
    adapter.editNextRecording();
    config.visual_read_only = false;
    adapter.setConfig(config);
    const auto state = adapter.visualSourceState(adapter.recordGamepadSourceId(), true);
    EXPECT_FALSE(state["editable"].toBool());
    EXPECT_FALSE(adapter.setVisualEnabled(state["sourceId"].toString(), state["itemId"].toString(),
                                          state["editRevision"].toString(), false, true));
    EXPECT_FALSE(adapter.setRecordGamepadOutputVisible(state["sourceId"].toString(), state["itemId"].toString(),
                                                       state["editRevision"].toString(), false));
    EXPECT_EQ(edits, 0);
    EXPECT_EQ(active_edits, 0);
}

TEST_F(SettingsVisualCutover, RecordGamepadRejectsFutureSettingsAndMissingPlacement) {
    for (const bool missing_item : {false, true}) {
        auto unsupported = config;
        if (missing_item)
            unsupported.visual_scene.items.pop_back();
        else
            unsupported.visual_scene.sources[1].settings_version = 2;
        adapter.setConfig(unsupported);
        const auto state = adapter.visualSourceState(adapter.recordGamepadSourceId(), true);
        EXPECT_FALSE(state["editable"].toBool());
        EXPECT_FALSE(adapter.setVisualOpacity(state["sourceId"].toString(), state["itemId"].toString(),
                                              state["editRevision"].toString(), 0.5, true));
        EXPECT_FALSE(adapter.setRecordGamepadPlacement(state["sourceId"].toString(), state["itemId"].toString(),
                                                       state["editRevision"].toString(), {0, 0, 0.4, 0.4}));
    }
    EXPECT_EQ(edits, 0);
}

TEST_F(SettingsVisualCutover, RecordEditingDefaultsToDeniedUntilCompositionProvidesLifecycle) {
    SettingsAdapter fresh;
    fresh.setVisualRegistry(&registry);
    fresh.setConfig(config);
    fresh.setRecordGamepadTarget({engine::VisualSourceId{{3}}, {}}, engine::SceneItemId{{4}});
    const auto state = fresh.visualSourceState(fresh.recordGamepadSourceId(), true);
    EXPECT_FALSE(state["editable"].toBool());
    EXPECT_FALSE(fresh.setVisualEnabled(state["sourceId"].toString(), state["itemId"].toString(),
                                        state["editRevision"].toString(), false, true));
}

TEST_F(SettingsVisualCutover, RecordLifecycleGatesEveryRecordingStateAndCommand) {
    struct Case {
        UiRecordingState state;
        bool editable;
    };
    const Case cases[] = {{UiRecordingState::LoadingCapabilities, false},
                          {UiRecordingState::Ready, true},
                          {UiRecordingState::Blocked, false},
                          {UiRecordingState::Countdown, true},
                          {UiRecordingState::Preparing, false},
                          {UiRecordingState::RegionSelecting, false},
                          {UiRecordingState::Recording, true},
                          {UiRecordingState::Paused, true},
                          {UiRecordingState::ArmedFromRecovery, true},
                          {UiRecordingState::Stopping, false},
                          {UiRecordingState::Saving, false},
                          {UiRecordingState::Completed, true},
                          {UiRecordingState::Failed, false}};
    adapter.setRecordGamepadTarget({engine::VisualSourceId{{3}}, engine::VisualRuntimeGeneration{7}},
                                   engine::SceneItemId{{4}});
    for (const auto& entry : cases) {
        SCOPED_TRACE(static_cast<int>(entry.state));
        EXPECT_EQ(IsWebcamOverlayEditable(entry.state), entry.editable);
        adapter.setRecordVisualEditingAllowed(IsWebcamOverlayEditable(entry.state));
        const auto state = adapter.visualSourceState(adapter.recordGamepadSourceId(), true);
        const auto source = state["sourceId"].toString();
        const auto item = state["itemId"].toString();
        const auto token = state["editRevision"].toString();
        EXPECT_EQ(state["editable"].toBool(), entry.editable);
        for (const auto& row : adapter.visualSettingsRows(source, true))
            EXPECT_EQ(row.toMap()["enabled"].toBool(), entry.editable);
        EXPECT_EQ(adapter.setVisualEnabled(source, item, token, true, true), entry.editable);
        EXPECT_EQ(adapter.setVisualOpacity(source, item, token, 1, true), entry.editable);
        EXPECT_EQ(adapter.setRecordGamepadOutputVisible(source, item, token, true), entry.editable);
        EXPECT_EQ(adapter.setRecordGamepadPlacement(source, item, token, {0.2, 0.3, 0.4, 0.4}), entry.editable);
        EXPECT_EQ(adapter.selectVisualSetting(source, item, token, "select-layout", "auto", true), entry.editable);
        EXPECT_EQ(adapter.dispatchVisualAction(source, item, "7", "retry", true), entry.editable);
    }
}

TEST_F(SettingsVisualCutover, LifecycleRevokesGestureWithoutLockingNextSettings) {
    adapter.setControlsLocked(true);
    adapter.editNextRecording();
    const auto captured = adapter.visualSourceState(adapter.recordGamepadSourceId(), true);
    int changed = 0;
    QObject::connect(&adapter, &SettingsAdapter::configChanged, &adapter, [&] { ++changed; });
    adapter.setRecordVisualEditingAllowed(true);
    EXPECT_EQ(changed, 0);
    adapter.setRecordVisualEditingAllowed(false);
    EXPECT_EQ(changed, 1);
    adapter.setRecordVisualEditingAllowed(false);
    EXPECT_EQ(changed, 1);
    EXPECT_FALSE(adapter.setRecordGamepadPlacement(captured["sourceId"].toString(), captured["itemId"].toString(),
                                                   captured["editRevision"].toString(), {0.2, 0.3, 0.4, 0.4}));
    EXPECT_TRUE(Select(State(true), "select-layout", "xbox_elite"));
    EXPECT_TRUE(adapter.editingNextRecording());
    EXPECT_EQ(edits, 1);
    EXPECT_EQ(active_edits, 0);
    adapter.setRecordVisualEditingAllowed(true);
    EXPECT_FALSE(adapter.setRecordGamepadPlacement(captured["sourceId"].toString(), captured["itemId"].toString(),
                                                   captured["editRevision"].toString(), {0.2, 0.3, 0.4, 0.4}));
    const auto fresh = adapter.visualSourceState(adapter.recordGamepadSourceId(), true);
    EXPECT_TRUE(adapter.setRecordGamepadPlacement(fresh["sourceId"].toString(), fresh["itemId"].toString(),
                                                  fresh["editRevision"].toString(), {0.2, 0.3, 0.4, 0.4}));
    EXPECT_EQ(active_edits, 1);
}

class SceneItemCommands : public SettingsVisualCutover {
  protected:
    void SetUp() override {
        SettingsVisualCutover::SetUp();
        adapter.setRecordWebcamTarget({engine::VisualSourceId{{1}}, engine::VisualRuntimeGeneration{5}},
                                      engine::SceneItemId{{2}});
        adapter.setRecordGamepadTarget({engine::VisualSourceId{{3}}, engine::VisualRuntimeGeneration{6}},
                                       engine::SceneItemId{{4}});
    }
    QVariantMap Record(bool gamepad) {
        return adapter.visualSourceState(gamepad ? adapter.recordGamepadSourceId() : adapter.recordWebcamSourceId(),
                                         true);
    }
    bool Command(const QVariantMap& state, const char* command) {
        return adapter.applySceneItemCommand(state["sourceId"].toString(), state["itemId"].toString(),
                                             state["runtime"].toString(), state["editRevision"].toString(),
                                             QString::fromLatin1(command));
    }
};

TEST_F(SceneItemCommands, MenuEntriesComeFromNodeMenuRegistrationsOfTheAddressedItem) {
    const auto pad = Record(true);
    const auto entries = adapter.sceneItemMenuEntries(pad["sourceId"].toString(), pad["itemId"].toString());
    // Layout is registered as an action but not for the node menu.
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].toMap()["action"], QStringLiteral("select-device"));
    EXPECT_EQ(entries[0].toMap()["valueLabel"], QStringLiteral("Automatic"));
    EXPECT_EQ(entries[0].toMap()["choices"].toList().size(), 3);
    const auto camera = Record(false);
    EXPECT_EQ(adapter.sceneItemMenuEntries(camera["sourceId"].toString(), camera["itemId"].toString()).size(), 1);
    EXPECT_TRUE(adapter.sceneItemMenuEntries(camera["sourceId"].toString(), pad["itemId"].toString()).isEmpty());
}

TEST_F(SceneItemCommands, StackingCommandsMoveOnlyTheAddressedItem) {
    EXPECT_EQ(Record(false)["stackIndex"], 0);
    EXPECT_EQ(Record(true)["stackIndex"], 1);
    EXPECT_FALSE(Record(false)["canSendToBack"].toBool());
    EXPECT_TRUE(Command(Record(false), "front"));
    EXPECT_EQ(Record(false)["stackIndex"], 1);
    EXPECT_EQ(Record(true)["stackIndex"], 0);
    EXPECT_FALSE(Record(false)["canBringToFront"].toBool());
    EXPECT_TRUE(Command(Record(false), "back"));
    EXPECT_EQ(Record(false)["stackIndex"], 0);
    EXPECT_EQ(edits, 2);
}

TEST_F(SceneItemCommands, HideShowAndRemoveUseTheHostSceneState) {
    EXPECT_TRUE(Command(Record(true), "hide"));
    EXPECT_FALSE(Record(true)["outputVisible"].toBool());
    EXPECT_TRUE(Record(true)["enabled"].toBool());
    EXPECT_TRUE(Command(Record(true), "show"));
    EXPECT_TRUE(Record(true)["outputVisible"].toBool());
    const auto before_remove = Record(true);
    EXPECT_TRUE(Command(before_remove, "remove"));
    EXPECT_EQ(adapter.config().visual_scene.items.size(), 1u);
    EXPECT_EQ(adapter.config().visual_scene.items.front().id, (engine::SceneItemId{{2}}));
    EXPECT_FALSE(adapter.config().visual_scene.sources[1].enabled);
    EXPECT_TRUE(adapter.config().visual_scene.sources.front().enabled);
    EXPECT_FALSE(Command(before_remove, "remove"));
    EXPECT_FALSE(Command(before_remove, "front"));
}

TEST_F(SceneItemCommands, StaleTokensForeignItemsAndLockedEditingAreRejected) {
    const auto pad = Record(true);
    auto foreign = Record(false);
    foreign["itemId"] = pad["itemId"];
    EXPECT_FALSE(Command(foreign, "remove"));
    auto stale_runtime = pad;
    stale_runtime["runtime"] = QStringLiteral("5");
    EXPECT_FALSE(Command(stale_runtime, "hide"));
    EXPECT_FALSE(Command(pad, "unknown"));
    adapter.setRecordGamepadTarget({engine::VisualSourceId{{3}}, engine::VisualRuntimeGeneration{7}},
                                   engine::SceneItemId{{4}});
    EXPECT_FALSE(Command(pad, "hide"));
    adapter.setRecordVisualEditingAllowed(false);
    EXPECT_FALSE(Command(Record(true), "hide"));
    EXPECT_EQ(edits, 0);
}

TEST_F(SceneItemCommands, EnablingASourceWithoutItemsRestoresOneItem) {
    EXPECT_TRUE(Command(Record(true), "remove"));
    adapter.setRecordGamepadTarget({engine::VisualSourceId{{3}}, {}}, {});
    const auto orphan = Record(true);
    EXPECT_EQ(orphan["itemId"], QString());
    EXPECT_FALSE(orphan["enabled"].toBool());
    EXPECT_TRUE(adapter.setVisualEnabled(orphan["sourceId"].toString(), QString(), orphan["editRevision"].toString(),
                                         true, true));
    const auto& scene = adapter.config().visual_scene;
    ASSERT_EQ(scene.items.size(), 2u);
    EXPECT_EQ(scene.items.back().source, (engine::VisualSourceId{{3}}));
    EXPECT_TRUE(scene.sources[1].enabled);
    // A repeated enable against the same orphan state must not add a second item.
    EXPECT_FALSE(adapter.setVisualEnabled(orphan["sourceId"].toString(), QString(), orphan["editRevision"].toString(),
                                          true, true));
    EXPECT_EQ(adapter.config().visual_scene.items.size(), 2u);
}

} // namespace
} // namespace exosnap::quick
