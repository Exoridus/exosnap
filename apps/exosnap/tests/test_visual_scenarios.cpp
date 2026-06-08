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
        QStringLiteral("diagnostics"),
        QStringLiteral("hotkeys"),
        QStringLiteral("logs"),
        QStringLiteral("about"),
    };

    for (const QString& id : required)
        EXPECT_NE(FindVisualScenario(id), nullptr) << id.toStdString();
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

TEST(VisualScenarioTest, RegisteredScenariosValidate) {
    for (const VisualScenario& scenario : VisualScenarioRegistry()) {
        QString error;
        EXPECT_TRUE(ValidateVisualScenario(scenario, &error))
            << scenario.id.toStdString() << ": " << error.toStdString();
    }
}

} // namespace
} // namespace exosnap::visual
