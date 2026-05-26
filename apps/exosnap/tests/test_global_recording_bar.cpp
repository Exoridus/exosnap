#include <gtest/gtest.h>

#include <array>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>

#include "ui/chrome/GlobalRecordingBar.h"

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

void FindRequiredLabel(ui::chrome::GlobalRecordingBar& bar, const char* object_name, QLabel*& label) {
    label = bar.findChild<QLabel*>(object_name);
    ASSERT_NE(label, nullptr);
}

void FindRequiredButton(ui::chrome::GlobalRecordingBar& bar, const char* object_name, QPushButton*& button) {
    button = bar.findChild<QPushButton*>(object_name);
    ASSERT_NE(button, nullptr);
}

QString RuntimePlaceholder() {
    return QStringLiteral("DUR --:--:-- ") + QChar(0x00B7) + QStringLiteral(" SIZE -");
}

QString RuntimeLiveValue() {
    return QStringLiteral("DUR 00:03 ") + QChar(0x00B7) + QStringLiteral(" SIZE 1.5 MB");
}

class GlobalRecordingBarTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(GlobalRecordingBarTest, ConstructorAndRuntimeSummaryTransitions_StayCanonical) {
    ui::chrome::GlobalRecordingBar bar;

    QLabel* profile = nullptr;
    QLabel* target = nullptr;
    QLabel* output = nullptr;
    QLabel* runtime = nullptr;
    QWidget* status_chip = nullptr;
    FindRequiredLabel(bar, "globalBarProfileSummaryValue", profile);
    FindRequiredLabel(bar, "globalBarTargetSummaryValue", target);
    FindRequiredLabel(bar, "globalBarOutputSummaryValue", output);
    FindRequiredLabel(bar, "globalBarRuntimeSummaryValue", runtime);
    status_chip = bar.findChild<QWidget*>("globalBarStatusChip");
    ASSERT_NE(status_chip, nullptr);

    EXPECT_EQ(profile->text(), QStringLiteral("-"));
    EXPECT_EQ(target->text(), QStringLiteral("-"));
    EXPECT_EQ(output->text(), QStringLiteral("-"));
    EXPECT_EQ(runtime->text(), RuntimePlaceholder());
    EXPECT_EQ(status_chip->toolTip(), QStringLiteral("Current recording status: READY."));
    EXPECT_EQ(status_chip->accessibleName(), QStringLiteral("Recording status: READY"));

    bar.setRuntimeSummary(RuntimePlaceholder());
    EXPECT_EQ(runtime->text(), RuntimePlaceholder());

    bar.setRuntimeSummary(RuntimeLiveValue());
    EXPECT_EQ(runtime->text(), RuntimeLiveValue());

    bar.setRuntimeSummary(RuntimePlaceholder());
    EXPECT_EQ(runtime->text(), RuntimePlaceholder());
}

TEST_F(GlobalRecordingBarTest, ContextSummarySetters_RefreshValuesAfterProfileOrOutputChanges) {
    ui::chrome::GlobalRecordingBar bar;

    QLabel* profile = nullptr;
    QLabel* target = nullptr;
    QLabel* output = nullptr;
    FindRequiredLabel(bar, "globalBarProfileSummaryValue", profile);
    FindRequiredLabel(bar, "globalBarTargetSummaryValue", target);
    FindRequiredLabel(bar, "globalBarOutputSummaryValue", output);

    bar.setProfileSummary(QStringLiteral("Window AV1 1080p60"));
    bar.setTargetSummary(QStringLiteral("Brave - Dashboard"));
    bar.setOutputSummary(QStringLiteral("WebM AV1 Opus"));

    EXPECT_EQ(profile->text(), QStringLiteral("Window AV1 1080p60"));
    EXPECT_EQ(target->text(), QStringLiteral("Brave - Dashboard"));
    EXPECT_EQ(output->text(), QStringLiteral("WebM AV1 Opus"));

    bar.setProfileSummary(QString());
    bar.setTargetSummary(QStringLiteral("   "));
    bar.setOutputSummary(QStringLiteral(""));

    EXPECT_EQ(profile->text(), QStringLiteral("-"));
    EXPECT_EQ(target->text(), QStringLiteral("-"));
    EXPECT_EQ(output->text(), QStringLiteral("-"));
}

TEST_F(GlobalRecordingBarTest, StatusLabelMapping_ControlsPrimaryPauseAndDetailsTooltips) {
    ui::chrome::GlobalRecordingBar bar;

    QPushButton* primary = nullptr;
    QPushButton* pause = nullptr;
    QWidget* status_chip = nullptr;
    FindRequiredButton(bar, "globalBarPrimaryActionButton", primary);
    FindRequiredButton(bar, "globalBarPauseActionButton", pause);
    status_chip = bar.findChild<QWidget*>("globalBarStatusChip");
    ASSERT_NE(status_chip, nullptr);

    struct Scenario {
        QString status_input;
        QString normalized_status;
        QString primary_text;
        bool primary_enabled;
        QString primary_tooltip;
        QString pause_text;
        bool pause_enabled;
        QString pause_tooltip;
    };

    const std::array<Scenario, 8> scenarios = {{
        {QStringLiteral("READY"), QStringLiteral("READY"), QStringLiteral("Start"), true,
         QStringLiteral("Start recording."), QStringLiteral("Pause"), false,
         QStringLiteral("Pause is available while recording.")},
        {QStringLiteral("REC"), QStringLiteral("REC"), QStringLiteral("Stop"), true, QStringLiteral("Stop recording."),
         QStringLiteral("Pause"), true, QStringLiteral("Pause recording.")},
        {QStringLiteral("PAUSED"), QStringLiteral("PAUSED"), QStringLiteral("Resume"), true,
         QStringLiteral("Resume recording."), QStringLiteral("Paused"), false,
         QStringLiteral("Recording is paused. Use Resume to continue.")},
        {QStringLiteral("BLOCKED by capability probe"), QStringLiteral("BLOCKED"), QStringLiteral("Details"), true,
         QStringLiteral("Open Diagnostics to review blockers and failures."), QStringLiteral("Pause"), false,
         QStringLiteral("Pause is available while recording.")},
        {QStringLiteral("ERROR"), QStringLiteral("ERROR"), QStringLiteral("Details"), true,
         QStringLiteral("Open Diagnostics to review blockers and failures."), QStringLiteral("Pause"), false,
         QStringLiteral("Pause is available while recording.")},
        {QStringLiteral("checking capabilities"), QStringLiteral("CHECKING"), QStringLiteral("Working..."), false,
         QStringLiteral("State transition in progress. Action is temporarily unavailable."), QStringLiteral("Pause"),
         false, QStringLiteral("Pause is available while recording.")},
        {QStringLiteral("STARTING"), QStringLiteral("STARTING"), QStringLiteral("Working..."), false,
         QStringLiteral("State transition in progress. Action is temporarily unavailable."), QStringLiteral("Pause"),
         false, QStringLiteral("Pause is available while recording.")},
        {QStringLiteral("STOPPING"), QStringLiteral("STOPPING"), QStringLiteral("Working..."), false,
         QStringLiteral("State transition in progress. Action is temporarily unavailable."), QStringLiteral("Pause"),
         false, QStringLiteral("Pause is available while recording.")},
    }};

    for (const Scenario& scenario : scenarios) {
        bar.setStatusLabel(scenario.status_input);
        EXPECT_EQ(bar.statusLabel(), scenario.normalized_status);
        EXPECT_EQ(primary->text(), scenario.primary_text);
        EXPECT_EQ(primary->isEnabled(), scenario.primary_enabled);
        EXPECT_EQ(primary->toolTip(), scenario.primary_tooltip);
        EXPECT_EQ(pause->text(), scenario.pause_text);
        EXPECT_EQ(pause->isEnabled(), scenario.pause_enabled);
        EXPECT_EQ(pause->toolTip(), scenario.pause_tooltip);
        EXPECT_EQ(status_chip->toolTip(),
                  QStringLiteral("Current recording status: %1.").arg(scenario.normalized_status));
        EXPECT_EQ(status_chip->accessibleName(),
                  QStringLiteral("Recording status: %1").arg(scenario.normalized_status));
    }
}

TEST_F(GlobalRecordingBarTest, PrimaryAction_OnlyEmitsWhenCurrentStatusAllowsAction) {
    ui::chrome::GlobalRecordingBar bar;

    QPushButton* primary = nullptr;
    FindRequiredButton(bar, "globalBarPrimaryActionButton", primary);

    int emit_count = 0;
    QObject::connect(&bar, &ui::chrome::GlobalRecordingBar::primaryActionRequested, [&emit_count]() { ++emit_count; });

    struct Scenario {
        QString status_input;
        bool expect_emit;
    };

    const std::array<Scenario, 8> scenarios = {{
        {QStringLiteral("READY"), true},
        {QStringLiteral("REC"), true},
        {QStringLiteral("PAUSED"), true},
        {QStringLiteral("BLOCKED"), true},
        {QStringLiteral("ERROR"), true},
        {QStringLiteral("CHECKING"), false},
        {QStringLiteral("STARTING"), false},
        {QStringLiteral("STOPPING"), false},
    }};

    for (const Scenario& scenario : scenarios) {
        bar.setStatusLabel(scenario.status_input);
        const int before = emit_count;
        primary->click();
        QApplication::processEvents();
        const int after = emit_count;
        EXPECT_EQ(after > before, scenario.expect_emit) << "status=" << scenario.status_input.toStdString();
    }
}

TEST_F(GlobalRecordingBarTest, LongSummaryValues_ElideDisplayedTextAndKeepFullTooltips) {
    ui::chrome::GlobalRecordingBar bar;
    bar.resize(980, ui::chrome::GlobalRecordingBar::kHeight);
    bar.show();
    QApplication::processEvents();

    const QString profile_long =
        QStringLiteral("  Custom AV1 Baseline Profile Name For Team Capture Validation Run   ");
    const QString target_long = QStringLiteral("Microsoft Edge - Deep Operational Dashboard "
                                               "| Service Desk - Incident Acknowledgement Queue");
    const QString output_long =
        QStringLiteral("C:/Users/User/Videos/ExoSnap/Very/Long/Project/Folder/Name/For/Capture/Sessions");
    const QString runtime_long = QStringLiteral("DUR 12:34:56 ") + QChar(0x00B7) + QStringLiteral(" SIZE 123.4 GB");

    bar.setProfileSummary(profile_long);
    bar.setTargetSummary(target_long);
    bar.setOutputSummary(output_long);
    bar.setRuntimeSummary(runtime_long);
    QApplication::processEvents();

    QLabel* profile = nullptr;
    QLabel* target = nullptr;
    QLabel* output = nullptr;
    QLabel* runtime = nullptr;
    FindRequiredLabel(bar, "globalBarProfileSummaryValue", profile);
    FindRequiredLabel(bar, "globalBarTargetSummaryValue", target);
    FindRequiredLabel(bar, "globalBarOutputSummaryValue", output);
    FindRequiredLabel(bar, "globalBarRuntimeSummaryValue", runtime);

    EXPECT_EQ(profile->toolTip(), QStringLiteral("Custom AV1 Baseline Profile Name For Team Capture Validation Run"));
    EXPECT_EQ(target->toolTip(), target_long);
    EXPECT_EQ(output->toolTip(), output_long);
    EXPECT_EQ(runtime->toolTip(), runtime_long);

    EXPECT_NE(profile->text(), profile->toolTip());
    EXPECT_NE(target->text(), target->toolTip());
    EXPECT_NE(output->text(), output->toolTip());
    EXPECT_EQ(runtime->text(), runtime->toolTip());
}

TEST_F(GlobalRecordingBarTest, CompactLayout_HidesPlannedDisabledActionsButKeepsTransportUsable) {
    constexpr int kHidePlannedActionsBelowWidth = 1340;
    constexpr int kWideWidth = kHidePlannedActionsBelowWidth + 20;
    constexpr int kCompactWidth = kHidePlannedActionsBelowWidth - 20;

    ui::chrome::GlobalRecordingBar bar;
    bar.show();
    QApplication::processEvents();

    QPushButton* primary = nullptr;
    QPushButton* pause = nullptr;
    QPushButton* mic = nullptr;
    QPushButton* marker = nullptr;
    QPushButton* overlay = nullptr;
    FindRequiredButton(bar, "globalBarPrimaryActionButton", primary);
    FindRequiredButton(bar, "globalBarPauseActionButton", pause);
    FindRequiredButton(bar, "globalBarMicActionButton", mic);
    FindRequiredButton(bar, "globalBarMarkerActionButton", marker);
    FindRequiredButton(bar, "globalBarOverlayActionButton", overlay);

    bar.resize(kWideWidth, ui::chrome::GlobalRecordingBar::kHeight);
    QApplication::processEvents();
    EXPECT_TRUE(primary->isVisible());
    EXPECT_TRUE(pause->isVisible());
    EXPECT_TRUE(mic->isVisible());
    EXPECT_TRUE(marker->isVisible());
    EXPECT_TRUE(overlay->isVisible());
    EXPECT_FALSE(mic->isEnabled());
    EXPECT_FALSE(marker->isEnabled());
    EXPECT_FALSE(overlay->isEnabled());

    bar.setStatusLabel(QStringLiteral("REC"));
    EXPECT_TRUE(primary->isEnabled());
    EXPECT_TRUE(pause->isEnabled());

    bar.resize(kCompactWidth, ui::chrome::GlobalRecordingBar::kHeight);
    QApplication::processEvents();
    EXPECT_TRUE(primary->isVisible());
    EXPECT_TRUE(pause->isVisible());
    EXPECT_FALSE(mic->isVisible());
    EXPECT_FALSE(marker->isVisible());
    EXPECT_FALSE(overlay->isVisible());
    EXPECT_TRUE(primary->isEnabled());
    EXPECT_TRUE(pause->isEnabled());
    EXPECT_FALSE(mic->isEnabled());
    EXPECT_FALSE(marker->isEnabled());
    EXPECT_FALSE(overlay->isEnabled());
}

TEST_F(GlobalRecordingBarTest, PlannedActions_RemainDisabledWithExplicitMvpTooltips) {
    ui::chrome::GlobalRecordingBar bar;

    QPushButton* mic = nullptr;
    QPushButton* marker = nullptr;
    QPushButton* overlay = nullptr;
    FindRequiredButton(bar, "globalBarMicActionButton", mic);
    FindRequiredButton(bar, "globalBarMarkerActionButton", marker);
    FindRequiredButton(bar, "globalBarOverlayActionButton", overlay);

    EXPECT_FALSE(mic->isEnabled());
    EXPECT_FALSE(marker->isEnabled());
    EXPECT_FALSE(overlay->isEnabled());

    EXPECT_EQ(mic->toolTip(), QStringLiteral("Global mic toggle is not available in this MVP build. "
                                             "Use Audio settings to change microphone state."));
    EXPECT_EQ(marker->toolTip(), QStringLiteral("Markers are not available in this MVP build."));
    EXPECT_EQ(overlay->toolTip(), QStringLiteral("Overlay/HUD controls are not available in this MVP build."));

    EXPECT_EQ(mic->accessibleName(), QStringLiteral("Microphone control unavailable"));
    EXPECT_EQ(marker->accessibleName(), QStringLiteral("Marker control unavailable"));
    EXPECT_EQ(overlay->accessibleName(), QStringLiteral("Overlay control unavailable"));
}

} // namespace
} // namespace exosnap
