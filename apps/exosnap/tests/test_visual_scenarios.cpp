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
        QStringLiteral("completed-output-fallback"),
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

} // namespace
} // namespace exosnap::visual
