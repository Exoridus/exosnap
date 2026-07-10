#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QMenu>
#include <QObject>
#include <QPushButton>
#include <QToolButton>

#include "models/OutputSettingsModel.h"
#include "pages/OutputPage.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "output_page_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class OutputPageTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// Production call site: MainWindow::refreshPresetUi() -> setProfileOptions.
// The sync path must not emit: MainWindow drops its re-entrancy guard on the
// strength of this exact guarantee.
TEST_F(OutputPageTest, SetProfileOptions_DoesNotEmitActiveProfileChanged) {
    OutputPage page{OutputSettingsModel::Defaults()};
    int emitted = 0;
    QObject::connect(&page, &OutputPage::activeProfileChanged, [&](const QString&) { ++emitted; });
    std::vector<OutputPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    opts.push_back({QStringLiteral("preset.abc"), QStringLiteral("Mine"), false, false, true, {}});
    page.setProfileOptions(opts, QStringLiteral("preset.abc"), false);
    EXPECT_EQ(emitted, 0);
}

// Same two visibility rules as the Settings row.
TEST_F(OutputPageTest, TwoRules_SaveAsNewResetOnChanged_DeleteOnUserPreset) {
    OutputPage page{OutputSettingsModel::Defaults()};
    std::vector<OutputPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    opts.push_back({QStringLiteral("preset.abc"), QStringLiteral("Mine"), false, false, true, {}});

    auto* save_as = page.findChild<QPushButton*>(QStringLiteral("outputPresetSaveAsButton"));
    auto* reset = page.findChild<QPushButton*>(QStringLiteral("outputPresetResetButton"));
    auto* del = page.findChild<QPushButton*>(QStringLiteral("outputPresetDeleteButton"));
    ASSERT_NE(save_as, nullptr);
    ASSERT_NE(reset, nullptr);
    ASSERT_NE(del, nullptr);

    page.setProfileOptions(opts, QStringLiteral("preset.default"), /*modified=*/false);
    EXPECT_FALSE(save_as->isVisibleTo(&page));
    EXPECT_FALSE(reset->isVisibleTo(&page));
    EXPECT_FALSE(del->isVisibleTo(&page));

    page.setProfileOptions(opts, QStringLiteral("preset.default"), /*modified=*/true);
    EXPECT_TRUE(save_as->isVisibleTo(&page)); // built-in + changed: Save as new + Reset
    EXPECT_TRUE(reset->isVisibleTo(&page));
    EXPECT_FALSE(del->isVisibleTo(&page));

    page.setProfileOptions(opts, QStringLiteral("preset.abc"), /*modified=*/false);
    EXPECT_TRUE(del->isVisibleTo(&page)); // clean user preset: Delete only
    EXPECT_FALSE(save_as->isVisibleTo(&page));
}

TEST_F(OutputPageTest, OverflowMenu_HasExactlyFourActions) {
    // Mirror of the ConfigPage menu test: Save as new… / Rename… / Export… / Import…,
    // Rename disabled while a built-in is selected.
    OutputPage page{OutputSettingsModel::Defaults()};
    std::vector<OutputPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    page.setProfileOptions(opts, QStringLiteral("preset.default"), false);

    auto* manage_btn = page.findChild<QToolButton*>(QStringLiteral("outputPresetManageButton"));
    ASSERT_NE(manage_btn, nullptr);
    ASSERT_NE(manage_btn->menu(), nullptr);
    QStringList texts;
    for (QAction* a : manage_btn->menu()->actions())
        if (!a->isSeparator())
            texts << a->text();
    EXPECT_EQ(texts, (QStringList() << QStringLiteral("Save as new\xe2\x80\xa6") << QStringLiteral("Rename\xe2\x80\xa6")
                                    << QStringLiteral("Export\xe2\x80\xa6") << QStringLiteral("Import\xe2\x80\xa6")));
    // Save as new stays reachable even when clean; Rename is built-in-gated.
    EXPECT_TRUE(manage_btn->menu()->actions().first()->isEnabled());
    for (QAction* a : manage_btn->menu()->actions())
        if (a->text() == QStringLiteral("Rename\xe2\x80\xa6"))
            EXPECT_FALSE(a->isEnabled());
}

// Combo-driven selection (the user-interaction path) still emits — only the
// programmatic sync path is silent.
TEST_F(OutputPageTest, ComboUserSelection_StillEmitsActiveProfileChanged) {
    OutputPage page{OutputSettingsModel::Defaults()};
    std::vector<OutputPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    opts.push_back({QStringLiteral("preset.abc"), QStringLiteral("Mine"), false, false, true, {}});
    page.setProfileOptions(opts, QStringLiteral("preset.default"), false);

    QString last_id;
    int emitted = 0;
    QObject::connect(&page, &OutputPage::activeProfileChanged, [&](const QString& id) {
        ++emitted;
        last_id = id;
    });

    auto* combo = page.findChild<QComboBox*>();
    ASSERT_NE(combo, nullptr);
    combo->setCurrentIndex(1);

    EXPECT_EQ(emitted, 1);
    EXPECT_EQ(last_id, QStringLiteral("preset.abc"));
}

} // namespace
} // namespace exosnap
