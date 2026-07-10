#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
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

TEST_F(WhatsNewOverlayTest, RendersOneSectionPerNote) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), false, QStringLiteral("https://gh/releases"));
    EXPECT_NE(overlay.findChild<QLabel*>(QStringLiteral("whatsNewBody_0")), nullptr);
    EXPECT_NE(overlay.findChild<QLabel*>(QStringLiteral("whatsNewBody_1")), nullptr);
    EXPECT_EQ(overlay.findChild<QLabel*>(QStringLiteral("whatsNewBody_2")), nullptr);
}

TEST_F(WhatsNewOverlayTest, NewestExpandedOlderCollapsed) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), false, QStringLiteral("https://gh/releases"));
    auto* body0 = overlay.findChild<QLabel*>(QStringLiteral("whatsNewBody_0"));
    auto* body1 = overlay.findChild<QLabel*>(QStringLiteral("whatsNewBody_1"));
    ASSERT_NE(body0, nullptr);
    ASSERT_NE(body1, nullptr);
    // Use !isHidden() rather than isVisible(): the overlay is not shown in the
    // headless fixture, so isVisible() is transitively false.
    EXPECT_FALSE(body0->isHidden()); // newest expanded
    EXPECT_TRUE(body1->isHidden());  // older collapsed
}

TEST_F(WhatsNewOverlayTest, OlderSectionExpandsOnHeaderClick) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), false, QStringLiteral("https://gh/releases"));
    auto* header1 = overlay.findChild<QAbstractButton*>(QStringLiteral("whatsNewHeader_1"));
    auto* body1 = overlay.findChild<QLabel*>(QStringLiteral("whatsNewBody_1"));
    ASSERT_NE(header1, nullptr);
    ASSERT_NE(body1, nullptr);
    EXPECT_TRUE(body1->isHidden());
    header1->click();
    EXPECT_FALSE(body1->isHidden());
}

TEST_F(WhatsNewOverlayTest, BodyRendersMarkdownContent) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), false, QStringLiteral("https://gh/releases"));
    auto* body0 = overlay.findChild<QLabel*>(QStringLiteral("whatsNewBody_0"));
    ASSERT_NE(body0, nullptr);
    EXPECT_TRUE(body0->text().contains(QStringLiteral("Feature C")));
}

TEST_F(WhatsNewOverlayTest, SuppressCheckboxOnlyInPostUpdateMode) {
    ui::dialogs::WhatsNewOverlay pre(MakeSections(), /*post_update_mode=*/false, QStringLiteral("https://gh/releases"));
    EXPECT_EQ(pre.findChild<QAbstractButton*>(QStringLiteral("whatsNewSuppressCheck")), nullptr);

    ui::dialogs::WhatsNewOverlay post(MakeSections(), /*post_update_mode=*/true, QStringLiteral("https://gh/releases"));
    EXPECT_NE(post.findChild<QAbstractButton*>(QStringLiteral("whatsNewSuppressCheck")), nullptr);
}

TEST_F(WhatsNewOverlayTest, SuppressToggleEmitsSignal) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), /*post_update_mode=*/true,
                                         QStringLiteral("https://gh/releases"));
    auto* check = overlay.findChild<QAbstractButton*>(QStringLiteral("whatsNewSuppressCheck"));
    ASSERT_NE(check, nullptr);

    bool received = false;
    bool value = false;
    QObject::connect(&overlay, &ui::dialogs::WhatsNewOverlay::suppressToggled, &overlay, [&](bool on) {
        received = true;
        value = on;
    });
    check->click();
    EXPECT_TRUE(received);
    EXPECT_TRUE(value);
}

TEST_F(WhatsNewOverlayTest, AllReleasesFooterPresent) {
    ui::dialogs::WhatsNewOverlay overlay(MakeSections(), false, QStringLiteral("https://gh/releases"));
    auto* btn = overlay.findChild<QPushButton*>(QStringLiteral("whatsNewAllReleasesBtn"));
    ASSERT_NE(btn, nullptr);
    EXPECT_TRUE(btn->text().contains(QStringLiteral("releases"), Qt::CaseInsensitive));
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
