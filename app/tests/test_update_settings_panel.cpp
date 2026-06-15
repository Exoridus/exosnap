#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QPushButton>

#include "ui/dialogs/UpdateSettingsPanel.h"

namespace exosnap {
namespace {

using ui::dialogs::UpdateSettingsPanel;
using ui::dialogs::UpdateUiModel;
using ui::dialogs::UpdateUiState;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "update_settings_panel_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

UpdateUiModel AvailableModel() {
    UpdateUiModel m;
    m.current_version = QStringLiteral("0.4.0");
    m.available_version = QStringLiteral("0.5.0");
    m.last_checked = QStringLiteral("2 minutes ago");
    m.whats_new = {QStringLiteral("Faster AV1 NVENC frame submission"), QStringLiteral("Fix: rare encoder crash")};
    m.release_url = QStringLiteral("https://github.com/Exoridus/exosnap/releases");
    m.release_notes_url = QStringLiteral("https://github.com/Exoridus/exosnap/releases/tag/v0.5.0");
    m.channel = QStringLiteral("Stable");
    return m;
}

class UpdateSettingsPanelTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(UpdateSettingsPanelTest, IsNotANativeDialog) {
    UpdateSettingsPanel panel;
    EXPECT_EQ(qobject_cast<QDialog*>(&panel), nullptr);
    EXPECT_EQ(panel.objectName(), QStringLiteral("updateSettingsPanel"));
}

TEST_F(UpdateSettingsPanelTest, DefaultStateRendersUpToDate) {
    UpdateSettingsPanel panel;
    auto* title = panel.findChild<QLabel*>(QStringLiteral("updateHeaderTitle"));
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->text(), QStringLiteral("You're up to date"));
    // Check-for-updates action is present in the default state.
    EXPECT_NE(panel.findChild<QPushButton*>(QStringLiteral("updateCheckButton")), nullptr);
    // No version pill or open-releases button when up to date.
    auto* pill = panel.findChild<QLabel*>(QStringLiteral("updateVersionPill"));
    ASSERT_NE(pill, nullptr);
    EXPECT_FALSE(pill->isVisible());
}

TEST_F(UpdateSettingsPanelTest, AvailableStateShowsPillAndOpenReleasesButton) {
    UpdateSettingsPanel panel;
    panel.setModel(AvailableModel());
    panel.setState(UpdateUiState::Available);

    auto* pill = panel.findChild<QLabel*>(QStringLiteral("updateVersionPill"));
    ASSERT_NE(pill, nullptr);
    EXPECT_TRUE(pill->isVisibleTo(&panel));
    EXPECT_TRUE(pill->text().contains(QStringLiteral("0.4.0")));
    EXPECT_TRUE(pill->text().contains(QStringLiteral("0.5.0")));

    auto* open = panel.findChild<QPushButton*>(QStringLiteral("updateOpenReleasesButton"));
    ASSERT_NE(open, nullptr);
    EXPECT_EQ(open->text(), QStringLiteral("Open releases page"));

    // What's-new list carries the model's highlights.
    auto* list = panel.findChild<QWidget*>(QStringLiteral("updateWhatsNewList"));
    ASSERT_NE(list, nullptr);
    bool found = false;
    for (auto* label : list->findChildren<QLabel*>())
        if (label->text().contains(QStringLiteral("AV1 NVENC")))
            found = true;
    EXPECT_TRUE(found);
}

TEST_F(UpdateSettingsPanelTest, ClickingCheckEmitsCheckRequested) {
    UpdateSettingsPanel panel;
    auto* check = panel.findChild<QPushButton*>(QStringLiteral("updateCheckButton"));
    ASSERT_NE(check, nullptr);

    int count = 0;
    QObject::connect(&panel, &UpdateSettingsPanel::checkRequested, &panel, [&count]() { ++count; });
    check->click();
    EXPECT_EQ(count, 1);
}

TEST_F(UpdateSettingsPanelTest, ClickingOpenReleasesEmitsRequest) {
    UpdateSettingsPanel panel;
    panel.setModel(AvailableModel());
    panel.setState(UpdateUiState::Available);

    auto* open = panel.findChild<QPushButton*>(QStringLiteral("updateOpenReleasesButton"));
    ASSERT_NE(open, nullptr);

    int count = 0;
    QObject::connect(&panel, &UpdateSettingsPanel::openReleasesPageRequested, &panel, [&count]() { ++count; });
    open->click();
    EXPECT_EQ(count, 1);
}

TEST_F(UpdateSettingsPanelTest, ChangingChannelEmitsChannelChanged) {
    UpdateSettingsPanel panel;
    auto* preview = panel.findChild<QPushButton*>(QStringLiteral("updateChannelPreview"));
    ASSERT_NE(preview, nullptr);

    int count = 0;
    QString last;
    QObject::connect(&panel, &UpdateSettingsPanel::channelChanged, &panel, [&](const QString& ch) {
        ++count;
        last = ch;
    });
    preview->click();
    EXPECT_EQ(count, 1);
    EXPECT_EQ(last, QStringLiteral("Preview"));
    EXPECT_EQ(panel.channel(), QStringLiteral("Preview"));
}

TEST_F(UpdateSettingsPanelTest, RecordingActiveShowsBannerAndDisablesCheck) {
    UpdateSettingsPanel panel;
    panel.setRecordingActive(true);

    auto* banner = panel.findChild<QFrame*>(QStringLiteral("updateRecordingBanner"));
    ASSERT_NE(banner, nullptr);

    auto* check = panel.findChild<QPushButton*>(QStringLiteral("updateCheckButton"));
    ASSERT_NE(check, nullptr);
    EXPECT_FALSE(check->isEnabled());
}

TEST_F(UpdateSettingsPanelTest, ErrorStateShowsBannerWithMessage) {
    UpdateSettingsPanel panel;
    UpdateUiModel m;
    m.channel = QStringLiteral("Stable");
    m.error_message = QStringLiteral("Network unreachable (timeout)");
    panel.setModel(m);
    panel.setState(UpdateUiState::Error);

    auto* banner = panel.findChild<QFrame*>(QStringLiteral("updateErrorBanner"));
    ASSERT_NE(banner, nullptr);
    bool found = false;
    for (auto* label : banner->findChildren<QLabel*>())
        if (label->text().contains(QStringLiteral("Network unreachable")))
            found = true;
    EXPECT_TRUE(found);

    // Error state offers a retry that funnels through checkRequested.
    auto* check = panel.findChild<QPushButton*>(QStringLiteral("updateCheckButton"));
    ASSERT_NE(check, nullptr);
    EXPECT_EQ(check->text(), QStringLiteral("Try again"));
    int count = 0;
    QObject::connect(&panel, &UpdateSettingsPanel::checkRequested, &panel, [&count]() { ++count; });
    check->click();
    EXPECT_EQ(count, 1);
}

} // namespace
} // namespace exosnap
