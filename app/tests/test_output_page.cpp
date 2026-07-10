#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>

#include <functional>

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

// Mirror of ConfigPage's ChangedSuffix_AppendedToSelectedComboText: the combo
// text carries the same "(changed)" hint the Settings preset row shows.
TEST_F(OutputPageTest, ChangedSuffix_AppendedWhenLiveConfigDiverges) {
    OutputPage page{OutputSettingsModel::Defaults()};
    std::vector<OutputPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});

    page.setProfileOptions(opts, QStringLiteral("preset.default"), /*active_profile_modified=*/false);
    auto* combo = page.findChild<QComboBox*>();
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentText(), QStringLiteral("Default"));

    page.setProfileOptions(opts, QStringLiteral("preset.default"), /*active_profile_modified=*/true);
    EXPECT_EQ(combo->currentText(), QStringLiteral("Default (changed)"));

    page.setProfileOptions(opts, QStringLiteral("preset.default"), /*active_profile_modified=*/false);
    EXPECT_EQ(combo->currentText(), QStringLiteral("Default"));
}

// The suffix is recomputed from the options model's base label each refresh,
// so refreshing an already-dirty selection must not stack a second suffix.
TEST_F(OutputPageTest, ChangedSuffix_DoesNotDoubleAppendOnRepeatedRefresh) {
    OutputPage page{OutputSettingsModel::Defaults()};
    std::vector<OutputPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});

    page.setProfileOptions(opts, QStringLiteral("preset.default"), /*active_profile_modified=*/true);
    page.setProfileOptions(opts, QStringLiteral("preset.default"), /*active_profile_modified=*/true);

    auto* combo = page.findChild<QComboBox*>();
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentText(), QStringLiteral("Default (changed)"));
}

// Adapted from the Settings-side expectation that a rejected preset name
// re-opens the dialog rather than giving up after one attempt: drive the
// real QInputDialog/QMessageBox modals via queued singleShot callbacks so
// the retry loop inside promptRenameActiveProfile() actually runs.
TEST_F(OutputPageTest, RenameActiveProfile_InvalidNameKeepsDialogOpenForRetry) {
    OutputPage page{OutputSettingsModel::Defaults()};
    std::vector<OutputPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    opts.push_back({QStringLiteral("preset.abc"), QStringLiteral("Mine"), false, false, true, {}});
    page.setProfileOptions(opts, QStringLiteral("preset.abc"), false);

    auto* manage_btn = page.findChild<QToolButton*>(QStringLiteral("outputPresetManageButton"));
    ASSERT_NE(manage_btn, nullptr);
    QAction* rename_action = nullptr;
    for (QAction* a : manage_btn->menu()->actions()) {
        if (a->text() == QStringLiteral("Rename\xe2\x80\xa6")) {
            rename_action = a;
        }
    }
    ASSERT_NE(rename_action, nullptr);

    QString emitted_name;
    int emit_count = 0;
    QObject::connect(&page, &OutputPage::renameActiveProfileRequested, [&](const QString& name) {
        ++emit_count;
        emitted_name = name;
    });

    int input_dialogs_seen = 0;
    std::function<void()> interact = [&]() {
        QWidget* modal = QApplication::activeModalWidget();
        if (auto* input = qobject_cast<QInputDialog*>(modal)) {
            ++input_dialogs_seen;
            if (input_dialogs_seen == 1) {
                input->setTextValue(QStringLiteral("Default")); // collides with the built-in: rejected
                QTimer::singleShot(0, interact);                // catch the warning the rejection opens
            } else {
                input->setTextValue(QStringLiteral("Renamed Mine")); // unique: accepted
            }
            input->accept();
        } else if (auto* box = qobject_cast<QMessageBox*>(modal)) {
            QTimer::singleShot(0, interact); // catch the dialog the retry loop re-opens
            box->accept();
        }
    };
    QTimer::singleShot(0, interact);
    // Safety net: if the retry loop regresses to a hang, force the modal
    // closed so the test fails fast instead of blocking the whole suite.
    QTimer::singleShot(5000, [] {
        if (auto* modal = QApplication::activeModalWidget()) {
            modal->close();
        }
    });

    rename_action->trigger();

    EXPECT_EQ(input_dialogs_seen, 2) << "a rejected name must not end the dialog after one attempt";
    EXPECT_EQ(emit_count, 1);
    EXPECT_EQ(emitted_name, QStringLiteral("Renamed Mine"));
}

} // namespace
} // namespace exosnap
