#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QWidget>

#include "services/WhatsNewPayload.h"
#include "ui/dialogs/WhatsNewOverlay.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "whats_new_overlay_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

QVector<WhatsNewNote> MakeSections() {
    return {
        {QStringLiteral("1.2.0"), QStringLiteral("## 1.2.0\n- Feature C"), QStringLiteral("https://gh/r/v1.2.0")},
        {QStringLiteral("1.1.0"), QStringLiteral("## 1.1.0\n- Feature B"), QStringLiteral("https://gh/r/v1.1.0")},
    };
}

class WhatsNewOverlayTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(WhatsNewOverlayTest, IsNotNativeDialog) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), /*post_update_mode=*/false,
                                         QStringLiteral("https://gh/releases"));
    EXPECT_EQ(qobject_cast<QDialog*>(&overlay), nullptr);
}

TEST_F(WhatsNewOverlayTest, CardAndTitlePresent) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), false, QStringLiteral("https://gh/releases"));
    EXPECT_NE(overlay.findChild<QFrame*>(QStringLiteral("whatsNewCard")), nullptr);
    auto* title = overlay.findChild<QLabel*>(QStringLiteral("whatsNewTitle"));
    ASSERT_NE(title, nullptr);
    EXPECT_TRUE(title->text().contains(QStringLiteral("new"), Qt::CaseInsensitive));
}

TEST_F(WhatsNewOverlayTest, NotesBrowserShowsEveryVersionConcatenated) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), false, QStringLiteral("https://gh/releases"));
    auto* browser = overlay.findChild<QTextBrowser*>(QStringLiteral("whatsNewNotesBrowser"));
    ASSERT_NE(browser, nullptr);
    const QString text = browser->toPlainText();
    EXPECT_TRUE(text.contains(QStringLiteral("1.2.0")));
    EXPECT_TRUE(text.contains(QStringLiteral("1.1.0")));
    EXPECT_TRUE(text.contains(QStringLiteral("Feature C")));
    EXPECT_TRUE(text.contains(QStringLiteral("Feature B")));
    // Newest first: 1.2.0's content appears before 1.1.0's.
    EXPECT_LT(text.indexOf(QStringLiteral("Feature C")), text.indexOf(QStringLiteral("Feature B")));
}

TEST_F(WhatsNewOverlayTest, SuppressCheckboxOnlyInPostUpdateMode) {
    ui::dialogs::WhatsNewOverlay pre(MakeSections(), /*post_update_mode=*/false, QStringLiteral("https://gh/releases"));
    EXPECT_EQ(pre.findChild<QAbstractButton*>(QStringLiteral("whatsNewSuppressCheck")), nullptr);

    ui::dialogs::WhatsNewOverlay post(MakeSections(), /*post_update_mode=*/true, QStringLiteral("https://gh/releases"));
    EXPECT_NE(post.findChild<QAbstractButton*>(QStringLiteral("whatsNewSuppressCheck")), nullptr);
}

TEST_F(WhatsNewOverlayTest, SuppressCheckboxDefaultCheckedMeansNotSuppressed) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), /*post_update_mode=*/true,
                                         QStringLiteral("https://gh/releases"));
    auto* check = overlay.findChild<QAbstractButton*>(QStringLiteral("whatsNewSuppressCheck"));
    ASSERT_NE(check, nullptr);
    EXPECT_TRUE(check->isChecked()) << "\"Show release notes after updates\" is on by default";
}

TEST_F(WhatsNewOverlayTest, UncheckingSuppressCheckboxEmitsSuppressedTrue) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), /*post_update_mode=*/true,
                                         QStringLiteral("https://gh/releases"));
    auto* check = overlay.findChild<QAbstractButton*>(QStringLiteral("whatsNewSuppressCheck"));
    ASSERT_NE(check, nullptr);
    ASSERT_TRUE(check->isChecked());

    bool received = false;
    bool suppressed = false;
    QObject::connect(&overlay, &ui::dialogs::WhatsNewOverlay::suppressToggled, &overlay, [&](bool on) {
        received = true;
        suppressed = on;
    });
    check->click(); // unchecking "show after updates" means the user IS suppressing it
    EXPECT_TRUE(received);
    EXPECT_TRUE(suppressed) << "unchecking the box must emit suppressToggled(true)";
}

TEST_F(WhatsNewOverlayTest, AllReleasesFooterPresent) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), false, QStringLiteral("https://gh/releases"));
    auto* btn = overlay.findChild<QPushButton*>(QStringLiteral("whatsNewAllReleasesBtn"));
    ASSERT_NE(btn, nullptr);
    EXPECT_TRUE(btn->text().contains(QStringLiteral("releases"), Qt::CaseInsensitive));
    EXPECT_FALSE(btn->icon().isNull()) << "must carry the external-link glyph";
}

TEST_F(WhatsNewOverlayTest, OpenCloseState) {
    QWidget host;
    auto* overlay =
        new ui::dialogs::WhatsNewOverlay(MakeSections(), false, QStringLiteral("https://gh/releases"), &host);
    EXPECT_FALSE(overlay->isOpen());
    overlay->openOverlay();
    EXPECT_TRUE(overlay->isOpen());
    overlay->closeOverlay();
    EXPECT_FALSE(overlay->isOpen());
}

TEST_F(WhatsNewOverlayTest, CloseEmitsClosed) {
    QWidget host;
    auto* overlay =
        new ui::dialogs::WhatsNewOverlay(MakeSections(), false, QStringLiteral("https://gh/releases"), &host);
    overlay->openOverlay();
    int closed_count = 0;
    QObject::connect(overlay, &ui::dialogs::WhatsNewOverlay::closed, overlay, [&]() { ++closed_count; });
    overlay->closeOverlay();
    EXPECT_EQ(closed_count, 1);
    EXPECT_FALSE(overlay->isOpen());
}

} // namespace
} // namespace exosnap
