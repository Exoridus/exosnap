#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QList>
#include <QPushButton>
#include <QSet>
#include <QString>

#include "ui/chrome/OperationalTitleBar.h"
#include "ui/chrome/RecordingStatusGuards.h"
#include "ui/widgets/NotificationBell.h"
#include "ui/widgets/StatusPill.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "operational_title_bar_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class OperationalTitleBarTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    // Page indices mirror the MainWindow page stack ordering used in production.
    // Hotkeys is at stack index 2 but no longer appears in the primary nav (PS-PHASE-B).
    static QVector<ui::chrome::OperationalTitleBar::NavItem> DefaultNavItems() {
        return {
            {QStringLiteral("Record"), 0}, {QStringLiteral("Settings"), 1}, {QStringLiteral("Diagnostics"), 3},
            {QStringLiteral("Logs"), 4},   {QStringLiteral("About"), -1},
        };
    }

    static QList<QPushButton*> NavTabs(const ui::chrome::OperationalTitleBar& bar) {
        return bar.findChildren<QPushButton*>(QStringLiteral("titlebarNavTab"));
    }
};

TEST_F(OperationalTitleBarTest, TopNav_ContainsRequiredTabsInOrder) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());

    const QList<QPushButton*> tabs = NavTabs(bar);
    QStringList labels;
    for (const QPushButton* tab : tabs)
        labels << tab->text();

    const QStringList expected = {QStringLiteral("Record"), QStringLiteral("Settings"), QStringLiteral("Diagnostics"),
                                  QStringLiteral("Logs"), QStringLiteral("About")};
    EXPECT_EQ(labels, expected);
}

TEST_F(OperationalTitleBarTest, TopNav_HasNoPageNumbers) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());

    for (const QPushButton* tab : NavTabs(bar)) {
        for (const QChar ch : tab->text())
            EXPECT_FALSE(ch.isDigit()) << "nav label must not contain page numbers: " << tab->text().toStdString();
    }
}

TEST_F(OperationalTitleBarTest, TopNav_AboutIsActionNotCheckablePage) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());

    int checkable_count = 0;
    QPushButton* about = nullptr;
    for (QPushButton* tab : NavTabs(bar)) {
        if (tab->isCheckable())
            ++checkable_count;
        if (tab->text() == QStringLiteral("About"))
            about = tab;
    }

    // PS-PHASE-B: four routed pages are checkable tabs (Record/Settings/Diagnostics/Logs);
    // About opens an in-window overlay and must not be checkable.
    EXPECT_EQ(checkable_count, 4);
    ASSERT_NE(about, nullptr);
    EXPECT_FALSE(about->isCheckable());
}

TEST_F(OperationalTitleBarTest, SetActivePage_HighlightsMatchingTab) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());

    bar.setActivePage(3); // Diagnostics
    for (const QPushButton* tab : NavTabs(bar)) {
        if (!tab->isCheckable())
            continue;
        EXPECT_EQ(tab->isChecked(), tab->text() == QStringLiteral("Diagnostics"))
            << "tab=" << tab->text().toStdString();
    }
}

TEST_F(OperationalTitleBarTest, StatusPill_ReflectsReadyRecordingPaused) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());

    auto* pill = bar.findChild<ui::widgets::StatusPill*>(QStringLiteral("titlebarStatusChip"));
    ASSERT_NE(pill, nullptr);

    bar.setStatusLabel(QStringLiteral("READY"));
    EXPECT_EQ(pill->text(), QStringLiteral("Ready"));
    EXPECT_EQ(pill->tone(), ui::widgets::StatusPill::Tone::Ready);

    bar.setStatusLabel(QStringLiteral("REC"));
    EXPECT_EQ(pill->text(), QStringLiteral("Recording"));
    EXPECT_EQ(pill->tone(), ui::widgets::StatusPill::Tone::Recording);

    bar.setStatusLabel(QStringLiteral("PAUSED"));
    EXPECT_EQ(pill->text(), QStringLiteral("Paused"));
    EXPECT_EQ(pill->tone(), ui::widgets::StatusPill::Tone::Warn);

    // DF-15: Countdown uses Info (azure) — visually distinct from amber Warn (Paused).
    bar.setStatusLabel(QStringLiteral("COUNTDOWN"));
    EXPECT_EQ(pill->text(), QStringLiteral("Countdown"));
    EXPECT_EQ(pill->tone(), ui::widgets::StatusPill::Tone::Info);
}

TEST_F(OperationalTitleBarTest, StatusPill_ShowsSavedAfterCompletedRecording) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());

    auto* pill = bar.findChild<ui::widgets::StatusPill*>(QStringLiteral("titlebarStatusChip"));
    ASSERT_NE(pill, nullptr);

    // A clean, saved recording reads as green "Saved" (same tone as Ready).
    bar.setStatusLabel(QStringLiteral("SAVED"));
    EXPECT_EQ(pill->text(), QStringLiteral("Saved"));
    EXPECT_EQ(pill->tone(), ui::widgets::StatusPill::Tone::Ready);

    // Starting a new recording reverts the pill away from Saved.
    bar.setStatusLabel(QStringLiteral("READY"));
    EXPECT_EQ(pill->text(), QStringLiteral("Ready"));
    EXPECT_EQ(pill->tone(), ui::widgets::StatusPill::Tone::Ready);
}

TEST_F(OperationalTitleBarTest, SavedStatus_IsScopedToRecordPage) {
    using ui::chrome::ScopeStatusLabelForActivePage;

    // On the Record page the post-recording "Saved" pill is allowed while the
    // result dock is visible.
    EXPECT_EQ(ScopeStatusLabelForActivePage(QStringLiteral("SAVED"), /*on_record_page=*/true), QStringLiteral("SAVED"));

    // Navigating away from Record must not leave a stale global "Saved" — it
    // falls back to the steady READY status.
    EXPECT_EQ(ScopeStatusLabelForActivePage(QStringLiteral("SAVED"), /*on_record_page=*/false),
              QStringLiteral("READY"));
}

TEST_F(OperationalTitleBarTest, NonSavedStatus_IsGlobalAcrossPages) {
    using ui::chrome::ScopeStatusLabelForActivePage;

    // Every status other than Saved is global and unaffected by the current page.
    for (const QString& label : {QStringLiteral("READY"), QStringLiteral("REC"), QStringLiteral("PAUSED"),
                                 QStringLiteral("BLOCKED"), QStringLiteral("ERROR")}) {
        EXPECT_EQ(ScopeStatusLabelForActivePage(label, /*on_record_page=*/true), label);
        EXPECT_EQ(ScopeStatusLabelForActivePage(label, /*on_record_page=*/false), label);
    }
}

TEST_F(OperationalTitleBarTest, SavedScope_DrivesPillLabelOnNavigation) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());

    auto* pill = bar.findChild<ui::widgets::StatusPill*>(QStringLiteral("titlebarStatusChip"));
    ASSERT_NE(pill, nullptr);

    // Record page + saved result → "Saved".
    bar.setStatusLabel(ui::chrome::ScopeStatusLabelForActivePage(QStringLiteral("SAVED"), /*on_record_page=*/true));
    EXPECT_EQ(pill->text(), QStringLiteral("Saved"));

    // Same underlying state, but viewed from another page → "Ready", not stale "Saved".
    bar.setStatusLabel(ui::chrome::ScopeStatusLabelForActivePage(QStringLiteral("SAVED"), /*on_record_page=*/false));
    EXPECT_EQ(pill->text(), QStringLiteral("Ready"));
}

TEST_F(OperationalTitleBarTest, Shell_HasNoGlobalTransportButtons) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());

    const QStringList forbidden = {QStringLiteral("Start"),  QStringLiteral("Stop"),         QStringLiteral("Pause"),
                                   QStringLiteral("Resume"), QStringLiteral("Record again"), QStringLiteral("Mic"),
                                   QStringLiteral("Marker")};
    for (const QPushButton* button : bar.findChildren<QPushButton*>()) {
        EXPECT_FALSE(forbidden.contains(button->text()))
            << "shell must not host transport control: " << button->text().toStdString();
    }
}

// ── Recording-border state (HYBRID-FIDELITY-R1 Part C) ───────────────────────

TEST_F(OperationalTitleBarTest, RecordingBorder_IdleByDefault) {
    ui::chrome::OperationalTitleBar bar;
    EXPECT_FALSE(bar.isRecordingActive());
}

TEST_F(OperationalTitleBarTest, RecordingBorder_ActiveWhileRecording) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());
    bar.setRecordingActive(true);
    bar.setStatusLabel(QStringLiteral("REC"));
    EXPECT_TRUE(bar.isRecordingActive());
}

TEST_F(OperationalTitleBarTest, RecordingBorder_ActiveWhilePaused) {
    // recording_active_ must stay true while paused: it drives the brand mark (coral)
    // and status pill. The chrome border is always neutral; recording emphasis is
    // provided by the status pill and the preview border only.
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());
    bar.setRecordingActive(true);
    bar.setStatusLabel(QStringLiteral("PAUSED"));
    EXPECT_TRUE(bar.isRecordingActive());
}

TEST_F(OperationalTitleBarTest, ChromeBorder_RecordingStatePreservedForBrandMark) {
    // Verify that setRecordingActive() still propagates the state for the brand mark and
    // QSS "recording" property while the chrome border itself remains visually neutral.
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());

    EXPECT_FALSE(bar.isRecordingActive());
    EXPECT_EQ(bar.property("recording").toBool(), false);

    bar.setRecordingActive(true);
    EXPECT_TRUE(bar.isRecordingActive());
    EXPECT_EQ(bar.property("recording").toBool(), true);

    bar.setRecordingActive(false);
    EXPECT_FALSE(bar.isRecordingActive());
    EXPECT_EQ(bar.property("recording").toBool(), false);
}

TEST_F(OperationalTitleBarTest, RecordingBorder_NeutralAfterStop) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());
    bar.setRecordingActive(true);
    bar.setStatusLabel(QStringLiteral("REC"));
    bar.setRecordingActive(false);
    bar.setStatusLabel(QStringLiteral("READY"));
    EXPECT_FALSE(bar.isRecordingActive());
}

TEST_F(OperationalTitleBarTest, RecordingBorder_NeutralAfterSaved) {
    // After a completed recording the border must return to neutral.
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());
    bar.setRecordingActive(false);
    bar.setStatusLabel(QStringLiteral("SAVED"));
    EXPECT_FALSE(bar.isRecordingActive());
}

// ── DF-11: Recording pill drop-frame telemetry ───────────────────────────────

TEST_F(OperationalTitleBarTest, RecordingPill_ShowsZeroDropsAsPlainLabel) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());

    auto* pill = bar.findChild<ui::widgets::StatusPill*>(QStringLiteral("titlebarStatusChip"));
    ASSERT_NE(pill, nullptr);

    bar.setRecordingActive(true);
    bar.setStatusLabel(QStringLiteral("REC"));
    bar.setRecordingDropCount(0);
    // No drops: pill reads "Recording" without any suffix.
    EXPECT_EQ(pill->text(), QStringLiteral("Recording"));
}

TEST_F(OperationalTitleBarTest, RecordingPill_ShowsDropCountWhenNonZero) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());

    auto* pill = bar.findChild<ui::widgets::StatusPill*>(QStringLiteral("titlebarStatusChip"));
    ASSERT_NE(pill, nullptr);

    bar.setRecordingActive(true);
    bar.setStatusLabel(QStringLiteral("REC"));
    bar.setRecordingDropCount(3);
    EXPECT_EQ(pill->text(), QStringLiteral("Recording · 3↓"));
}

TEST_F(OperationalTitleBarTest, RecordingPill_DropCountResetOnRecordingStop) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());

    auto* pill = bar.findChild<ui::widgets::StatusPill*>(QStringLiteral("titlebarStatusChip"));
    ASSERT_NE(pill, nullptr);

    bar.setRecordingActive(true);
    bar.setStatusLabel(QStringLiteral("REC"));
    bar.setRecordingDropCount(5);
    EXPECT_EQ(pill->text(), QStringLiteral("Recording · 5↓"));

    // After stopping, drop count resets; the next Recording session starts clean.
    bar.setRecordingActive(false);
    bar.setStatusLabel(QStringLiteral("READY"));
    bar.setRecordingActive(true);
    bar.setStatusLabel(QStringLiteral("REC"));
    EXPECT_EQ(pill->text(), QStringLiteral("Recording"));
}

// ── DF-15: Starting state also uses Info tone ────────────────────────────────

TEST_F(OperationalTitleBarTest, StatusPill_StartingUsesInfoTone) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());

    auto* pill = bar.findChild<ui::widgets::StatusPill*>(QStringLiteral("titlebarStatusChip"));
    ASSERT_NE(pill, nullptr);

    bar.setStatusLabel(QStringLiteral("STARTING"));
    EXPECT_EQ(pill->text(), QStringLiteral("Starting"));
    EXPECT_EQ(pill->tone(), ui::widgets::StatusPill::Tone::Info);
}

// ── PS-PHASE-B: 5-item nav + notification bell ───────────────────────────────

TEST_F(OperationalTitleBarTest, TopNav_HasFiveItems) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());
    EXPECT_EQ(NavTabs(bar).size(), 5);
}

TEST_F(OperationalTitleBarTest, TopNav_DoesNotContainHotkeys) {
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());
    for (const QPushButton* tab : NavTabs(bar))
        EXPECT_NE(tab->text(), QStringLiteral("Hotkeys")) << "Hotkeys must not appear in the slim nav";
}

TEST_F(OperationalTitleBarTest, Bell_PresentInTitleBar) {
    ui::chrome::OperationalTitleBar bar;
    EXPECT_NE(bar.bellWidget(), nullptr);
}

TEST_F(OperationalTitleBarTest, Bell_DefaultZeroCount) {
    ui::chrome::OperationalTitleBar bar;
    ASSERT_NE(bar.bellWidget(), nullptr);
    EXPECT_EQ(bar.bellWidget()->unreadCount(), 0);
}

TEST_F(OperationalTitleBarTest, Bell_SetUnreadCountPropagates) {
    ui::chrome::OperationalTitleBar bar;
    bar.setBellUnreadCount(3);
    ASSERT_NE(bar.bellWidget(), nullptr);
    EXPECT_EQ(bar.bellWidget()->unreadCount(), 3);
}

TEST_F(OperationalTitleBarTest, Bell_ClickEmitsBellClickedSignal) {
    ui::chrome::OperationalTitleBar bar;
    int click_count = 0;
    QObject::connect(&bar, &ui::chrome::OperationalTitleBar::bellClicked, [&]() { ++click_count; });
    ASSERT_NE(bar.bellWidget(), nullptr);
    bar.bellWidget()->click();
    EXPECT_EQ(click_count, 1);
}

} // namespace
} // namespace exosnap
