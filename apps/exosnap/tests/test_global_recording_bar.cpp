#include <gtest/gtest.h>

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

QLabel* FindRequiredLabel(ui::chrome::GlobalRecordingBar& bar, const char* object_name) {
    auto* label = bar.findChild<QLabel*>(object_name);
    EXPECT_NE(label, nullptr);
    return label;
}

QPushButton* FindRequiredButton(ui::chrome::GlobalRecordingBar& bar, const char* object_name) {
    auto* button = bar.findChild<QPushButton*>(object_name);
    EXPECT_NE(button, nullptr);
    return button;
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

    const auto* profile = FindRequiredLabel(bar, "globalBarProfileSummaryValue");
    const auto* target = FindRequiredLabel(bar, "globalBarTargetSummaryValue");
    const auto* output = FindRequiredLabel(bar, "globalBarOutputSummaryValue");
    const auto* runtime = FindRequiredLabel(bar, "globalBarRuntimeSummaryValue");

    EXPECT_EQ(profile->text(), QStringLiteral("-"));
    EXPECT_EQ(target->text(), QStringLiteral("-"));
    EXPECT_EQ(output->text(), QStringLiteral("-"));
    EXPECT_EQ(runtime->text(), RuntimePlaceholder());

    bar.setRuntimeSummary(RuntimePlaceholder());
    EXPECT_EQ(runtime->text(), RuntimePlaceholder());

    bar.setRuntimeSummary(RuntimeLiveValue());
    EXPECT_EQ(runtime->text(), RuntimeLiveValue());

    bar.setRuntimeSummary(RuntimePlaceholder());
    EXPECT_EQ(runtime->text(), RuntimePlaceholder());
}

TEST_F(GlobalRecordingBarTest, ContextSummarySetters_RefreshValuesAfterProfileOrOutputChanges) {
    ui::chrome::GlobalRecordingBar bar;

    auto* profile = FindRequiredLabel(bar, "globalBarProfileSummaryValue");
    auto* target = FindRequiredLabel(bar, "globalBarTargetSummaryValue");
    auto* output = FindRequiredLabel(bar, "globalBarOutputSummaryValue");

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

TEST_F(GlobalRecordingBarTest, StatusLabelMapping_ControlsPrimaryAndPauseButtons) {
    ui::chrome::GlobalRecordingBar bar;

    auto* primary = FindRequiredButton(bar, "globalBarPrimaryActionButton");
    auto* pause = FindRequiredButton(bar, "globalBarPauseActionButton");

    bar.setStatusLabel(QStringLiteral("PAUSED"));
    EXPECT_EQ(primary->text(), QStringLiteral("Resume"));
    EXPECT_TRUE(primary->isEnabled());
    EXPECT_EQ(pause->text(), QStringLiteral("Paused"));
    EXPECT_FALSE(pause->isEnabled());

    bar.setStatusLabel(QStringLiteral("REC"));
    EXPECT_EQ(primary->text(), QStringLiteral("Stop"));
    EXPECT_TRUE(primary->isEnabled());
    EXPECT_EQ(pause->text(), QStringLiteral("Pause"));
    EXPECT_TRUE(pause->isEnabled());

    bar.setStatusLabel(QStringLiteral("READY"));
    EXPECT_EQ(primary->text(), QStringLiteral("Start"));
    EXPECT_TRUE(primary->isEnabled());
    EXPECT_EQ(pause->text(), QStringLiteral("Pause"));
    EXPECT_FALSE(pause->isEnabled());
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

    const auto* profile = FindRequiredLabel(bar, "globalBarProfileSummaryValue");
    const auto* target = FindRequiredLabel(bar, "globalBarTargetSummaryValue");
    const auto* output = FindRequiredLabel(bar, "globalBarOutputSummaryValue");
    const auto* runtime = FindRequiredLabel(bar, "globalBarRuntimeSummaryValue");

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
    ui::chrome::GlobalRecordingBar bar;
    bar.show();
    QApplication::processEvents();

    auto* primary = FindRequiredButton(bar, "globalBarPrimaryActionButton");
    auto* pause = FindRequiredButton(bar, "globalBarPauseActionButton");
    auto* mic = FindRequiredButton(bar, "globalBarMicActionButton");
    auto* marker = FindRequiredButton(bar, "globalBarMarkerActionButton");
    auto* overlay = FindRequiredButton(bar, "globalBarOverlayActionButton");

    bar.resize(1500, ui::chrome::GlobalRecordingBar::kHeight);
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

    bar.resize(1200, ui::chrome::GlobalRecordingBar::kHeight);
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

} // namespace
} // namespace exosnap
