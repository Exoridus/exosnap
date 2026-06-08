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
        QStringLiteral("record-recording"),
        QStringLiteral("record-paused"),
        QStringLiteral("record-completed"),
        QStringLiteral("settings-display"),
        QStringLiteral("settings-window"),
        QStringLiteral("settings-region"),
        QStringLiteral("source-picker-screens"),
        QStringLiteral("source-picker-windows"),
        QStringLiteral("source-picker-region"),
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
    for (const QString& id : {QStringLiteral("record-ready"), QStringLiteral("record-recording"),
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

} // namespace
} // namespace exosnap::visual
