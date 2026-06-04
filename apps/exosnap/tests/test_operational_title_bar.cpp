#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QList>
#include <QPushButton>
#include <QSet>
#include <QString>

#include "ui/chrome/OperationalTitleBar.h"
#include "ui/chrome/RecordingStatusGuards.h"
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
    static QVector<ui::chrome::OperationalTitleBar::NavItem> DefaultNavItems() {
        return {
            {QStringLiteral("Record"), 0},      {QStringLiteral("Settings"), 1}, {QStringLiteral("Hotkeys"), 2},
            {QStringLiteral("Diagnostics"), 3}, {QStringLiteral("Logs"), 4},     {QStringLiteral("About"), -1},
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

    const QStringList expected = {QStringLiteral("Record"),      QStringLiteral("Settings"), QStringLiteral("Hotkeys"),
                                  QStringLiteral("Diagnostics"), QStringLiteral("Logs"),     QStringLiteral("About")};
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

    // The five routed pages are checkable tabs; About opens a dialog and must not be checkable.
    EXPECT_EQ(checkable_count, 5);
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
    // The title-bar bottom border must still be active (non-neutral) while paused;
    // the view distinguishes coral vs amber via the status label.
    ui::chrome::OperationalTitleBar bar;
    bar.setNavItems(DefaultNavItems());
    bar.setRecordingActive(true);
    bar.setStatusLabel(QStringLiteral("PAUSED"));
    EXPECT_TRUE(bar.isRecordingActive());
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

} // namespace
} // namespace exosnap
