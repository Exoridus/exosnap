#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QFrame>
#include <QLabel>
#include <QMetaMethod>
#include <QProgressBar>
#include <QPushButton>
#include <QWidget>

#include "pages/EditExportPage.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "edit_export_page_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class EditExportPageTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(EditExportPageTest, ConstructsWithoutCrash) {
    EditExportPage page;
    page.show();
    SUCCEED();
}

TEST_F(EditExportPageTest, DefaultPhaseIsReview) {
    EditExportPage page;
    EXPECT_EQ(page.phase(), EditExportPage::Phase::Review);
}

TEST_F(EditExportPageTest, SetPhaseReviewToEdit) {
    EditExportPage page;
    page.setPhase(EditExportPage::Phase::Edit);
    EXPECT_EQ(page.phase(), EditExportPage::Phase::Edit);
}

TEST_F(EditExportPageTest, SetPhaseToOutput) {
    EditExportPage page;
    page.setPhase(EditExportPage::Phase::Output);
    EXPECT_EQ(page.phase(), EditExportPage::Phase::Output);
}

TEST_F(EditExportPageTest, SetPhaseToExporting) {
    EditExportPage page;
    page.setPhase(EditExportPage::Phase::Exporting);
    EXPECT_EQ(page.phase(), EditExportPage::Phase::Exporting);
    auto* bar = page.findChild<QProgressBar*>(QStringLiteral("editExportProgressBar"));
    ASSERT_NE(bar, nullptr);
    // Bar should not be hidden in Exporting phase (isVisible() requires parent shown)
    EXPECT_FALSE(bar->isHidden());
}

TEST_F(EditExportPageTest, SetPhaseToDone) {
    EditExportPage page;
    page.setPhase(EditExportPage::Phase::Done);
    EXPECT_EQ(page.phase(), EditExportPage::Phase::Done);
}

TEST_F(EditExportPageTest, SetPhaseToFailed) {
    EditExportPage page;
    page.setPhase(EditExportPage::Phase::Failed);
    EXPECT_EQ(page.phase(), EditExportPage::Phase::Failed);
}

TEST_F(EditExportPageTest, SetRecordingInfoUpdatesFactLabels) {
    EditExportPage page;
    page.setRecordingInfo(QStringLiteral("C:\\test\\recording.mkv"), QStringLiteral("00:04:18"),
                          QStringLiteral("612 MB"), QStringLiteral("2560 x 1440"), QStringLiteral("60 fps CFR"),
                          QStringLiteral("AV1"), QStringLiteral("Opus"), QStringLiteral("MKV"));

    auto* dur = page.findChild<QLabel*>(QStringLiteral("editFactDuration"));
    ASSERT_NE(dur, nullptr);
    EXPECT_EQ(dur->text(), QStringLiteral("00:04:18"));

    auto* sz = page.findChild<QLabel*>(QStringLiteral("editFactSize"));
    ASSERT_NE(sz, nullptr);
    EXPECT_EQ(sz->text(), QStringLiteral("612 MB"));

    auto* vid = page.findChild<QLabel*>(QStringLiteral("editFactVideo"));
    ASSERT_NE(vid, nullptr);
    EXPECT_EQ(vid->text(), QStringLiteral("AV1"));

    auto* container = page.findChild<QLabel*>(QStringLiteral("editFactContainer"));
    ASSERT_NE(container, nullptr);
    EXPECT_EQ(container->text(), QStringLiteral("MKV"));
}

TEST_F(EditExportPageTest, ExportButtonIsVisibleInOutputPhase) {
    EditExportPage page;
    page.setPhase(EditExportPage::Phase::Output);
    auto* primary = page.findChild<QPushButton*>(QStringLiteral("editExportPrimaryBtn"));
    ASSERT_NE(primary, nullptr);
    EXPECT_FALSE(primary->isHidden()); // isVisible() requires parent shown; isHidden() checks own flag
    EXPECT_EQ(primary->text(), QStringLiteral("Export"));
}

TEST_F(EditExportPageTest, BackButtonTriggersSignal) {
    EditExportPage page;
    int signal_count = 0;
    QObject::connect(&page, &EditExportPage::backRequested, &page, [&signal_count]() { ++signal_count; });
    auto* back_btn = page.findChild<QPushButton*>(QStringLiteral("editExportBackBtn"));
    ASSERT_NE(back_btn, nullptr);
    back_btn->click();
    EXPECT_EQ(signal_count, 1);
}

TEST_F(EditExportPageTest, PlaceholderBannerPresent) {
    EditExportPage page;
    auto* banner = page.findChild<QFrame*>(QStringLiteral("editExportPlaceholderBanner"));
    EXPECT_NE(banner, nullptr);
}

TEST_F(EditExportPageTest, NavRemainsUnaffected) {
    // EditExportPage has no navPageRequested signal — it only emits backRequested
    // and exportCompleted. Verify there is no navPageRequested signal by checking
    // the meta-object.
    const QMetaObject* mo = &EditExportPage::staticMetaObject;
    bool found_nav = false;
    for (int i = 0; i < mo->methodCount(); ++i) {
        if (mo->method(i).methodType() == QMetaMethod::Signal &&
            QByteArray(mo->method(i).name()) == "navPageRequested") {
            found_nav = true;
        }
    }
    EXPECT_FALSE(found_nav);
}

} // namespace
} // namespace exosnap
