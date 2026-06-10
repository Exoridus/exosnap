#include <gtest/gtest.h>

#include "visual_tests/VisualScenario.h"

#include <QSet>

namespace exosnap::visual {
namespace {

TEST(VisualScenarioTest, ScenarioIdsAreUnique) {
    QSet<QString> seen;
    for (const VisualScenario& scenario : VisualScenarioRegistry()) {
        EXPECT_FALSE(scenario.id.isEmpty());
        EXPECT_FALSE(seen.contains(scenario.id)) << scenario.id.toStdString();
        seen.insert(scenario.id);
    }
}

TEST(VisualScenarioTest, RequiredScenariosAreRegistered) {
    const QStringList required = {
        QStringLiteral("record-ready"),
        QStringLiteral("record-ready-countdown-off"),
        QStringLiteral("record-ready-countdown-3s"),
        QStringLiteral("record-countdown-3"),
        QStringLiteral("record-countdown-2"),
        QStringLiteral("record-countdown-1"),
        QStringLiteral("record-countdown-cancelled"),
        QStringLiteral("record-recording-after-countdown"),
        QStringLiteral("record-recording"),
        QStringLiteral("record-paused"),
        QStringLiteral("record-completed"),
        QStringLiteral("settings-display"),
        QStringLiteral("settings-window"),
        QStringLiteral("settings-region"),
        QStringLiteral("source-picker-screens"),
        QStringLiteral("source-picker-windows"),
        QStringLiteral("source-picker-region"),
        QStringLiteral("source-region-empty"),
        QStringLiteral("source-region-selected"),
        QStringLiteral("source-region-editing"),
        QStringLiteral("source-region-preset-16x9"),
        QStringLiteral("source-region-preset-9x16"),
        QStringLiteral("source-region-invalid"),
        QStringLiteral("webcam-active"),
        QStringLiteral("webcam-unavailable"),
        QStringLiteral("record-webcam-disabled"),
        QStringLiteral("record-webcam-default-pip"),
        QStringLiteral("record-webcam-selected"),
        QStringLiteral("record-webcam-dragging"),
        QStringLiteral("record-webcam-resize-top-left"),
        QStringLiteral("record-webcam-resize-bottom-right"),
        QStringLiteral("record-webcam-min-size"),
        QStringLiteral("record-webcam-max-size"),
        QStringLiteral("record-webcam-mirrored"),
        QStringLiteral("record-webcam-countdown-locked"),
        QStringLiteral("record-webcam-recording-locked"),
        QStringLiteral("settings-webcam-mirror-off"),
        QStringLiteral("settings-webcam-mirror-on"),
        QStringLiteral("settings-webcam-unavailable"),
        QStringLiteral("diagnostics"),
        QStringLiteral("hotkeys"),
        QStringLiteral("logs"),
        QStringLiteral("logs-empty"),
        QStringLiteral("logs-all-levels"),
        QStringLiteral("logs-info-filter"),
        QStringLiteral("logs-issues-filter"),
        QStringLiteral("logs-search-results"),
        QStringLiteral("logs-long-message"),
        QStringLiteral("logs-buffer-truncated"),
        QStringLiteral("about"),
        // Complete preset scenarios (COMPLETE-PRESET-R1).
        QStringLiteral("settings-preset-default"),
        QStringLiteral("settings-preset-modified"),
        QStringLiteral("settings-preset-saved"),
        QStringLiteral("settings-preset-menu"),
        QStringLiteral("settings-preset-multiple"),
        QStringLiteral("settings-preset-default-badge"),
        QStringLiteral("settings-preset-delete-confirm"),
        QStringLiteral("settings-preset-save-error"),
        QStringLiteral("record-preset-display"),
        QStringLiteral("record-preset-window"),
        QStringLiteral("record-preset-region"),
        QStringLiteral("record-preset-webcam"),
        QStringLiteral("record-preset-countdown"),
        QStringLiteral("settings-output-native"),
        QStringLiteral("settings-output-4k"),
        QStringLiteral("settings-output-1440p"),
        QStringLiteral("settings-output-1080p"),
        QStringLiteral("settings-output-720p"),
        QStringLiteral("settings-output-custom-resolution"),
        QStringLiteral("settings-output-custom-resolution-invalid"),
        QStringLiteral("settings-format-24-cfr"),
        QStringLiteral("settings-format-30-cfr"),
        QStringLiteral("settings-format-60-cfr"),
        QStringLiteral("settings-format-120-unavailable"),
        QStringLiteral("settings-format-vfr"),
        QStringLiteral("settings-format-container-mkv"),
        QStringLiteral("settings-format-container-mp4"),
        QStringLiteral("settings-format-incompatible"),
        QStringLiteral("settings-format-recording-locked"),
        QStringLiteral("record-output-native"),
        QStringLiteral("record-output-1080p"),
        QStringLiteral("record-output-letterbox"),
        QStringLiteral("record-output-region"),
        QStringLiteral("record-output-webcam"),
        QStringLiteral("record-output-summary"),
        QStringLiteral("completed-output-1080p"),
        QStringLiteral("completed-output-custom-resolution"),
        QStringLiteral("completed-output-fallback"),
        // Device discovery scenarios (DEVICE-DISCOVERY-R1).
        QStringLiteral("settings-audio-devices-normal"),
        QStringLiteral("settings-audio-selected-missing"),
        QStringLiteral("settings-audio-default-changed"),
        QStringLiteral("settings-webcam-devices-normal"),
        QStringLiteral("settings-webcam-selected-missing"),
        QStringLiteral("settings-webcam-reconnected"),
        QStringLiteral("source-displays-normal"),
        QStringLiteral("source-display-selected-missing"),
        QStringLiteral("record-display-unavailable"),
        QStringLiteral("record-region-monitor-missing"),
        // Split recording scenarios (SPLIT-RECORDING-R1).
        QStringLiteral("record-split-available"),
        QStringLiteral("paused-split-available"),
        QStringLiteral("completed-recording-segments"),
        QStringLiteral("completed-recording-segment-missing"),
    };

    for (const QString& id : required)
        EXPECT_NE(FindVisualScenario(id), nullptr) << id.toStdString();
}

// 45. Every webcam scenario is registered with the expected routing + state.
TEST(VisualScenarioTest, WebcamPipScenariosCarryDeterministicState) {
    const VisualScenario* def = FindVisualScenario(QStringLiteral("record-webcam-default-pip"));
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->page, VisualPage::Record);
    EXPECT_TRUE(def->webcam_pip_enabled);
    EXPECT_FALSE(def->webcam_pip_selected);
    EXPECT_FALSE(def->webcam_mirror);

    const VisualScenario* mirrored = FindVisualScenario(QStringLiteral("record-webcam-mirrored"));
    ASSERT_NE(mirrored, nullptr);
    EXPECT_TRUE(mirrored->webcam_mirror);

    const VisualScenario* selected = FindVisualScenario(QStringLiteral("record-webcam-selected"));
    ASSERT_NE(selected, nullptr);
    EXPECT_TRUE(selected->webcam_pip_selected);

    const VisualScenario* locked = FindVisualScenario(QStringLiteral("record-webcam-recording-locked"));
    ASSERT_NE(locked, nullptr);
    EXPECT_EQ(locked->record_state, VisualRecordState::Recording);
    EXPECT_TRUE(locked->webcam_pip_edit_locked);

    const VisualScenario* disabled = FindVisualScenario(QStringLiteral("record-webcam-disabled"));
    ASSERT_NE(disabled, nullptr);
    EXPECT_FALSE(disabled->webcam_pip_enabled);
}

// Settings webcam scenarios route to the Settings page and carry mirror/availability.
TEST(VisualScenarioTest, SettingsWebcamScenariosRouteToSettings) {
    const VisualScenario* off = FindVisualScenario(QStringLiteral("settings-webcam-mirror-off"));
    const VisualScenario* on = FindVisualScenario(QStringLiteral("settings-webcam-mirror-on"));
    const VisualScenario* unavailable = FindVisualScenario(QStringLiteral("settings-webcam-unavailable"));
    ASSERT_NE(off, nullptr);
    ASSERT_NE(on, nullptr);
    ASSERT_NE(unavailable, nullptr);

    EXPECT_EQ(off->page, VisualPage::Settings);
    EXPECT_EQ(off->webcam_state, VisualWebcamState::Active);
    EXPECT_FALSE(off->webcam_mirror);
    EXPECT_TRUE(on->webcam_mirror);
    EXPECT_EQ(unavailable->webcam_state, VisualWebcamState::Unavailable);
}

TEST(VisualScenarioTest, RecordStatesRouteToRecordPage) {
    for (const QString& id :
         {QStringLiteral("record-ready"), QStringLiteral("record-countdown-3"), QStringLiteral("record-recording"),
          QStringLiteral("record-paused"), QStringLiteral("record-completed")}) {
        const VisualScenario* scenario = FindVisualScenario(id);
        ASSERT_NE(scenario, nullptr);
        EXPECT_EQ(scenario->page, VisualPage::Record);
        EXPECT_NE(scenario->record_state, VisualRecordState::None);
    }
}

TEST(VisualScenarioTest, SourcePickerScenariosOpenTypedTabs) {
    const VisualScenario* screens = FindVisualScenario(QStringLiteral("source-picker-screens"));
    const VisualScenario* windows = FindVisualScenario(QStringLiteral("source-picker-windows"));
    const VisualScenario* region = FindVisualScenario(QStringLiteral("source-picker-region"));
    ASSERT_NE(screens, nullptr);
    ASSERT_NE(windows, nullptr);
    ASSERT_NE(region, nullptr);

    EXPECT_EQ(screens->source_picker_tab, VisualSourcePickerTab::Screens);
    EXPECT_EQ(windows->source_picker_tab, VisualSourcePickerTab::Windows);
    EXPECT_EQ(region->source_picker_tab, VisualSourcePickerTab::Region);
}

TEST(VisualScenarioTest, ReleaseBuildPolicyDisablesHarness) {
    EXPECT_FALSE(VisualHarnessEnabledForBuildConfig(QStringLiteral("Release")));
    EXPECT_FALSE(VisualHarnessEnabledForBuildConfig(QStringLiteral("release")));
    EXPECT_TRUE(VisualHarnessEnabledForBuildConfig(QStringLiteral("Debug")));
    EXPECT_TRUE(VisualHarnessEnabledForBuildConfig(QStringLiteral("RelWithDebInfo")));
}

TEST(VisualScenarioTest, RunnerExitCodesAreStable) {
    EXPECT_EQ(VisualRunnerExitCode(false, false, false, false, false), 2);
    EXPECT_EQ(VisualRunnerExitCode(true, false, true, true, true), 3);
    EXPECT_EQ(VisualRunnerExitCode(true, true, false, true, true), 4);
    EXPECT_EQ(VisualRunnerExitCode(true, true, true, true, true), 0);
}

TEST(VisualScenarioTest, ManifestSerializationEnumsAreStable) {
    const VisualScenario* scenario = FindVisualScenario(QStringLiteral("record-recording"));
    ASSERT_NE(scenario, nullptr);
    EXPECT_EQ(ToString(scenario->page), QStringLiteral("record"));
    EXPECT_EQ(ToString(scenario->record_state), QStringLiteral("recording"));
    EXPECT_EQ(ToString(VisualRecordState::Countdown), QStringLiteral("countdown"));
    EXPECT_EQ(ToString(VisualRegionState::Preset16x9), QStringLiteral("preset-16x9"));
    EXPECT_EQ(ToString(VisualRegionEditMode::ResizeTopLeft), QStringLiteral("resize-top-left"));
    EXPECT_EQ(ToString(VisualLogFilter::Issues), QStringLiteral("issues"));
}

TEST(VisualScenarioTest, DiffMasksCoverDynamicRecordSurfaces) {
    const VisualScenario* scenario = FindVisualScenario(QStringLiteral("record-recording"));
    ASSERT_NE(scenario, nullptr);
    QStringList masked_objects;
    for (const VisualMask& mask : scenario->masks)
        masked_objects.push_back(mask.object_name);
    EXPECT_TRUE(masked_objects.contains(QStringLiteral("previewSurface")));
    EXPECT_TRUE(masked_objects.contains(QStringLiteral("recordDockTimer")));
    EXPECT_TRUE(masked_objects.contains(QStringLiteral("recordTransportDock")));
}

TEST(VisualScenarioTest, ScenarioParserRejectsInvalidCountdown) {
    VisualScenario scenario;
    scenario.id = QStringLiteral("bad-countdown");
    scenario.countdown_seconds = 4;
    QString error;
    EXPECT_FALSE(ValidateVisualScenario(scenario, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("countdown")));
}

TEST(VisualScenarioTest, ScenarioParserRejectsInvalidRegionGeometry) {
    VisualScenario scenario;
    scenario.id = QStringLiteral("bad-region");
    scenario.region_state = VisualRegionState::Selected;
    scenario.region_width = 63;
    scenario.region_height = 720;
    QString error;
    EXPECT_FALSE(ValidateVisualScenario(scenario, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("region")));
}

TEST(VisualScenarioTest, ScenarioParserRejectsInvalidOutputDimensions) {
    VisualScenario scenario;
    scenario.id = QStringLiteral("bad-output");
    scenario.requested_width = 1920;
    scenario.requested_height = 0;
    QString error;
    EXPECT_FALSE(ValidateVisualScenario(scenario, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("output")));
}

TEST(VisualScenarioTest, ScenarioParserRejectsInvalidFrameRate) {
    VisualScenario scenario;
    scenario.id = QStringLiteral("bad-frame-rate");
    scenario.frame_rate_num = 0;
    scenario.frame_rate_den = 1;
    QString error;
    EXPECT_FALSE(ValidateVisualScenario(scenario, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("frame rate")));
}

TEST(VisualScenarioTest, OutputFormatScenariosCarryManifestFields) {
    const VisualScenario* letterbox = FindVisualScenario(QStringLiteral("record-output-letterbox"));
    ASSERT_NE(letterbox, nullptr);
    EXPECT_EQ(letterbox->output_resolution_mode, OutputResolutionMode::FHD1080);
    EXPECT_EQ(letterbox->effective_width, 1920);
    EXPECT_EQ(letterbox->effective_height, 1080);
    EXPECT_EQ(letterbox->content_x, 240);
    EXPECT_EQ(letterbox->content_width, 1440);

    const VisualScenario* unavailable = FindVisualScenario(QStringLiteral("settings-format-120-unavailable"));
    ASSERT_NE(unavailable, nullptr);
    EXPECT_EQ(unavailable->frame_rate_num, 60u);
    EXPECT_FALSE(unavailable->reconciliation_warning.isEmpty());
}

TEST(VisualScenarioTest, RegisteredScenariosValidate) {
    for (const VisualScenario& scenario : VisualScenarioRegistry()) {
        QString error;
        EXPECT_TRUE(ValidateVisualScenario(scenario, &error))
            << scenario.id.toStdString() << ": " << error.toStdString();
    }
}

// 46. Invalid webcam PiP placement inputs are rejected by validation.
TEST(VisualScenarioTest, ScenarioParserRejectsInvalidWebcamPlacement) {
    {
        VisualScenario scenario;
        scenario.id = QStringLiteral("bad-webcam-overflow");
        scenario.webcam_pip_enabled = true;
        scenario.webcam_x = 0.9f;
        scenario.webcam_w = 0.5f; // x + w = 1.4 > 1
        QString error;
        EXPECT_FALSE(ValidateVisualScenario(scenario, &error));
        EXPECT_TRUE(error.contains(QStringLiteral("Webcam")));
    }
    {
        VisualScenario scenario;
        scenario.id = QStringLiteral("bad-webcam-negative");
        scenario.webcam_pip_enabled = true;
        scenario.webcam_x = -0.1f;
        QString error;
        EXPECT_FALSE(ValidateVisualScenario(scenario, &error));
    }
    {
        VisualScenario scenario;
        scenario.id = QStringLiteral("bad-webcam-too-small");
        scenario.webcam_pip_enabled = true;
        scenario.webcam_w = 0.001f; // below kMinSize
        QString error;
        EXPECT_FALSE(ValidateVisualScenario(scenario, &error));
    }
}

// 47. Webcam handle enum serialization is stable.
TEST(VisualScenarioTest, WebcamHandleSerializationIsStable) {
    EXPECT_EQ(ToString(VisualWebcamHandle::None), QStringLiteral("none"));
    EXPECT_EQ(ToString(VisualWebcamHandle::Move), QStringLiteral("move"));
    EXPECT_EQ(ToString(VisualWebcamHandle::ResizeTopLeft), QStringLiteral("tl"));
    EXPECT_EQ(ToString(VisualWebcamHandle::ResizeBottomRight), QStringLiteral("br"));
}

// ---- Device Discovery scenarios (DEVICE-DISCOVERY-R1) -----------------------

// 48. Audio device discovery scenarios carry deterministic discovery fields.
TEST(VisualScenarioTest, DeviceDiscovery_AudioScenariosCarryDiscoveryFields) {
    const VisualScenario* normal = FindVisualScenario(QStringLiteral("settings-audio-devices-normal"));
    ASSERT_NE(normal, nullptr);
    EXPECT_EQ(normal->page, VisualPage::Settings);
    EXPECT_EQ(normal->dd_audio_input_count, 2);
    EXPECT_TRUE(normal->dd_selected_mic_available);
    EXPECT_EQ(normal->dd_last_discovery_reason, QStringLiteral("Startup"));

    const VisualScenario* missing = FindVisualScenario(QStringLiteral("settings-audio-selected-missing"));
    ASSERT_NE(missing, nullptr);
    EXPECT_EQ(missing->page, VisualPage::Settings);
    EXPECT_FALSE(missing->dd_selected_mic_available);
    EXPECT_FALSE(missing->dd_selected_mic_stable_id.isEmpty())
        << "Missing-mic scenario must preserve the configured stable id";
    EXPECT_EQ(missing->dd_last_discovery_reason, QStringLiteral("DeviceRemoved"));

    const VisualScenario* def = FindVisualScenario(QStringLiteral("settings-audio-default-changed"));
    ASSERT_NE(def, nullptr);
    EXPECT_TRUE(def->dd_selected_mic_stable_id.isEmpty()) << "semantic Default scenario has no stable id";
    EXPECT_EQ(def->dd_last_discovery_reason, QStringLiteral("DefaultChanged"));
}

// 49. Webcam discovery scenarios carry deterministic discovery fields.
TEST(VisualScenarioTest, DeviceDiscovery_WebcamScenariosCarryDiscoveryFields) {
    const VisualScenario* normal = FindVisualScenario(QStringLiteral("settings-webcam-devices-normal"));
    ASSERT_NE(normal, nullptr);
    EXPECT_EQ(normal->page, VisualPage::Settings);
    EXPECT_EQ(normal->webcam_state, VisualWebcamState::Active);
    EXPECT_EQ(normal->dd_webcam_count, 1);
    EXPECT_TRUE(normal->dd_selected_webcam_available);

    const VisualScenario* missing = FindVisualScenario(QStringLiteral("settings-webcam-selected-missing"));
    ASSERT_NE(missing, nullptr);
    EXPECT_EQ(missing->webcam_state, VisualWebcamState::Unavailable);
    EXPECT_FALSE(missing->dd_selected_webcam_available);
    EXPECT_EQ(missing->dd_webcam_count, 0);
    EXPECT_EQ(missing->dd_last_discovery_reason, QStringLiteral("DeviceRemoved"));

    const VisualScenario* reconnected = FindVisualScenario(QStringLiteral("settings-webcam-reconnected"));
    ASSERT_NE(reconnected, nullptr);
    EXPECT_EQ(reconnected->webcam_state, VisualWebcamState::Active);
    EXPECT_TRUE(reconnected->dd_selected_webcam_available);
    EXPECT_EQ(reconnected->dd_last_discovery_reason, QStringLiteral("DeviceAdded"));
}

// 50. Display discovery scenarios carry deterministic discovery fields.
TEST(VisualScenarioTest, DeviceDiscovery_DisplayScenariosCarryDiscoveryFields) {
    const VisualScenario* normal = FindVisualScenario(QStringLiteral("source-displays-normal"));
    ASSERT_NE(normal, nullptr);
    EXPECT_EQ(normal->page, VisualPage::Record);
    EXPECT_EQ(normal->source_picker_tab, VisualSourcePickerTab::Screens);
    EXPECT_EQ(normal->dd_display_count, 2);
    EXPECT_TRUE(normal->dd_selected_display_available);
    EXPECT_TRUE(normal->dd_current_target_resolved);

    const VisualScenario* missing = FindVisualScenario(QStringLiteral("source-display-selected-missing"));
    ASSERT_NE(missing, nullptr);
    EXPECT_FALSE(missing->dd_selected_display_available);
    EXPECT_FALSE(missing->dd_current_target_resolved);
    EXPECT_FALSE(missing->dd_selected_display_stable_id.isEmpty());
    EXPECT_EQ(missing->dd_last_discovery_reason, QStringLiteral("DeviceRemoved"));
}

// 51. Record-page unavailability scenarios represent honest unresolved state.
TEST(VisualScenarioTest, DeviceDiscovery_RecordPageUnavailabilityIsHonest) {
    const VisualScenario* display_gone = FindVisualScenario(QStringLiteral("record-display-unavailable"));
    ASSERT_NE(display_gone, nullptr);
    EXPECT_EQ(display_gone->page, VisualPage::Record);
    EXPECT_EQ(display_gone->record_state, VisualRecordState::Ready);
    EXPECT_FALSE(display_gone->dd_current_target_resolved);
    EXPECT_FALSE(display_gone->dd_selected_display_available);

    const VisualScenario* region_gone = FindVisualScenario(QStringLiteral("record-region-monitor-missing"));
    ASSERT_NE(region_gone, nullptr);
    EXPECT_EQ(region_gone->page, VisualPage::Record);
    EXPECT_EQ(region_gone->settings_target, VisualSettingsTarget::Region);
    EXPECT_FALSE(region_gone->dd_current_target_resolved);
    // Region invalid: geometry is 64x64 (minimum valid for the region field).
    EXPECT_EQ(region_gone->region_state, VisualRegionState::Invalid);
}

// 52. Device discovery sentinel fields are defaulted correctly for non-discovery scenarios.
TEST(VisualScenarioTest, DeviceDiscovery_DefaultSentinelsForNonDiscoveryScenarios) {
    const VisualScenario* record_ready = FindVisualScenario(QStringLiteral("record-ready"));
    ASSERT_NE(record_ready, nullptr);
    // Sentinels: -1 means "not applicable"
    EXPECT_EQ(record_ready->dd_audio_input_count, -1);
    EXPECT_EQ(record_ready->dd_webcam_count, -1);
    EXPECT_EQ(record_ready->dd_display_count, -1);
    EXPECT_TRUE(record_ready->dd_selected_mic_stable_id.isEmpty());
    EXPECT_TRUE(record_ready->dd_selected_display_stable_id.isEmpty());
    EXPECT_TRUE(record_ready->dd_last_discovery_reason.isEmpty());
}

// 53. Applying a discovery scenario does not dirty the preset state.
// Harness hooks are non-persistent: the scenario fields are driven by
// in-memory page methods that never call RecordingPresetStore::Save() or
// AppSettingsStore::Save().  We verify this at unit level by confirming that
// the scenario struct itself carries no preset-dirty signal.
TEST(VisualScenarioTest, DeviceDiscovery_ScenarioStructDoesNotSetPresetDirty) {
    for (const VisualScenario& s : VisualScenarioRegistry()) {
        if (!s.id.startsWith(QStringLiteral("settings-audio")) &&
            !s.id.startsWith(QStringLiteral("settings-webcam-devices")) &&
            !s.id.startsWith(QStringLiteral("settings-webcam-reconnected")) &&
            !s.id.startsWith(QStringLiteral("source-displays")) &&
            !s.id.startsWith(QStringLiteral("source-display-selected")) &&
            !s.id.startsWith(QStringLiteral("record-display-unavailable")) &&
            !s.id.startsWith(QStringLiteral("record-region-monitor-missing")))
            continue;
        // Discovery scenarios must NOT set preset_dirty = true (they do not
        // represent a user edit; they only represent device availability state).
        EXPECT_FALSE(s.preset_dirty) << "Discovery scenario " << s.id.toStdString() << " must not set preset_dirty";
    }
}

// 54. Split recording scenarios carry deterministic state (SPLIT-RECORDING-R1).
TEST(VisualScenarioTest, SplitRecordingScenariosCarryDeterministicState) {
    const VisualScenario* rec_split = FindVisualScenario(QStringLiteral("record-split-available"));
    ASSERT_NE(rec_split, nullptr);
    EXPECT_EQ(rec_split->page, VisualPage::Record);
    EXPECT_EQ(rec_split->record_state, VisualRecordState::Recording);
    EXPECT_TRUE(rec_split->split_action_visible);
    EXPECT_TRUE(rec_split->split_action_enabled);

    const VisualScenario* paused_split = FindVisualScenario(QStringLiteral("paused-split-available"));
    ASSERT_NE(paused_split, nullptr);
    EXPECT_EQ(paused_split->record_state, VisualRecordState::Paused);
    EXPECT_TRUE(paused_split->split_action_visible);
    EXPECT_TRUE(paused_split->split_action_enabled);

    const VisualScenario* segments = FindVisualScenario(QStringLiteral("completed-recording-segments"));
    ASSERT_NE(segments, nullptr);
    EXPECT_EQ(segments->record_state, VisualRecordState::Completed);
    EXPECT_EQ(segments->completed_segment_count, 3);
    EXPECT_FALSE(segments->completed_segment_missing);

    const VisualScenario* missing = FindVisualScenario(QStringLiteral("completed-recording-segment-missing"));
    ASSERT_NE(missing, nullptr);
    EXPECT_EQ(missing->completed_segment_count, 3);
    EXPECT_TRUE(missing->completed_segment_missing);
}

} // namespace
} // namespace exosnap::visual
