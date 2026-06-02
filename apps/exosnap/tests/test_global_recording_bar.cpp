#include <gtest/gtest.h>

#include <array>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>

#include "ui/chrome/GlobalRecordingBar.h"
#include "ui/chrome/RecordingStatusGuards.h"
#include "ui/widgets/StatusPill.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "global_recording_bar_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class GlobalRecordingBarTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(GlobalRecordingBarTest, Constructor_IsStatusOnlyChrome) {
    ui::chrome::GlobalRecordingBar bar;

    const auto* status_chip = bar.findChild<ui::widgets::StatusPill*>(QStringLiteral("globalBarStatusChip"));
    ASSERT_NE(status_chip, nullptr);
    EXPECT_EQ(status_chip->text(), QStringLiteral("READY"));
    EXPECT_EQ(status_chip->toolTip(), QStringLiteral("Current recording status: READY."));
    EXPECT_EQ(status_chip->accessibleName(), QStringLiteral("Recording status: READY"));

    EXPECT_TRUE(bar.findChildren<QPushButton*>().empty());
    EXPECT_EQ(bar.findChild<QLabel*>(QStringLiteral("globalBarProfileSummaryValue")), nullptr);
    EXPECT_EQ(bar.findChild<QLabel*>(QStringLiteral("globalBarTargetSummaryValue")), nullptr);
    EXPECT_EQ(bar.findChild<QLabel*>(QStringLiteral("globalBarOutputSummaryValue")), nullptr);
    EXPECT_EQ(bar.findChild<QLabel*>(QStringLiteral("globalBarRuntimeSummaryValue")), nullptr);
}

TEST_F(GlobalRecordingBarTest, StatusLabelMapping_UpdatesOnlyStatusChip) {
    ui::chrome::GlobalRecordingBar bar;

    auto* status_chip = bar.findChild<ui::widgets::StatusPill*>(QStringLiteral("globalBarStatusChip"));
    ASSERT_NE(status_chip, nullptr);

    struct Scenario {
        QString status_input;
        QString normalized_status;
        ui::widgets::StatusPill::Tone tone;
        bool dot_visible;
    };

    const std::array<Scenario, 8> scenarios = {{
        {QStringLiteral("READY"), QStringLiteral("READY"), ui::widgets::StatusPill::Tone::Ready, false},
        {QStringLiteral("REC"), QStringLiteral("REC"), ui::widgets::StatusPill::Tone::Recording, true},
        {QStringLiteral("PAUSED"), QStringLiteral("PAUSED"), ui::widgets::StatusPill::Tone::Warn, true},
        {QStringLiteral("BLOCKED by capability probe"), QStringLiteral("BLOCKED"),
         ui::widgets::StatusPill::Tone::Blocked, true},
        {QStringLiteral("ERROR"), QStringLiteral("ERROR"), ui::widgets::StatusPill::Tone::Blocked, true},
        {QStringLiteral("checking capabilities"), QStringLiteral("CHECKING"), ui::widgets::StatusPill::Tone::Warn,
         true},
        {QStringLiteral("STARTING"), QStringLiteral("STARTING"), ui::widgets::StatusPill::Tone::Warn, true},
        {QStringLiteral("STOPPING"), QStringLiteral("STOPPING"), ui::widgets::StatusPill::Tone::Warn, true},
    }};

    for (const Scenario& scenario : scenarios) {
        bar.setStatusLabel(scenario.status_input);
        EXPECT_EQ(bar.statusLabel(), scenario.normalized_status);
        EXPECT_EQ(status_chip->text(), scenario.normalized_status);
        EXPECT_EQ(status_chip->tone(), scenario.tone);
        EXPECT_EQ(status_chip->isDotVisible(), scenario.dot_visible);
        EXPECT_EQ(status_chip->toolTip(),
                  QStringLiteral("Current recording status: %1.").arg(scenario.normalized_status));
        EXPECT_EQ(status_chip->accessibleName(),
                  QStringLiteral("Recording status: %1").arg(scenario.normalized_status));
    }
}

TEST(GlobalRecordingBarStatusGuardTest, RuntimeVisibility_OnlyForRecPausedStopping) {
    struct Scenario {
        QString status;
        bool should_show_runtime;
    };

    const std::array<Scenario, 10> scenarios = {{
        {QStringLiteral("READY"), false},
        {QStringLiteral("REC"), true},
        {QStringLiteral("PAUSED"), true},
        {QStringLiteral("STOPPING"), true},
        {QStringLiteral("BLOCKED"), false},
        {QStringLiteral("ERROR"), false},
        {QStringLiteral("CHECKING"), false},
        {QStringLiteral("STARTING"), false},
        {QStringLiteral(" stopping "), true},
        {QStringLiteral("rec"), true},
    }};

    for (const Scenario& scenario : scenarios) {
        EXPECT_EQ(ui::chrome::ShouldShowRecordingRuntimeForStatus(scenario.status), scenario.should_show_runtime)
            << "status=" << scenario.status.toStdString();
    }
}

TEST(GlobalRecordingBarStatusGuardTest, DiagnosticsNavigation_OnlyForBlockedOrError) {
    struct Scenario {
        QString status;
        bool should_open_diagnostics;
    };

    const std::array<Scenario, 10> scenarios = {{
        {QStringLiteral("BLOCKED"), true},
        {QStringLiteral("ERROR"), true},
        {QStringLiteral("blocked"), true},
        {QStringLiteral("error"), true},
        {QStringLiteral(" BLOCKED "), true},
        {QStringLiteral("READY"), false},
        {QStringLiteral("REC"), false},
        {QStringLiteral("PAUSED"), false},
        {QStringLiteral("CHECKING"), false},
        {QStringLiteral("STARTING"), false},
    }};

    for (const Scenario& scenario : scenarios) {
        EXPECT_EQ(ui::chrome::ShouldOpenRecordingDiagnosticsForStatus(scenario.status),
                  scenario.should_open_diagnostics)
            << "status=" << scenario.status.toStdString();
    }
}

} // namespace
} // namespace exosnap
