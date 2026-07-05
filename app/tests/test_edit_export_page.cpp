#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QFrame>
#include <QLabel>
#include <QMetaMethod>
#include <QProgressBar>
#include <QPushButton>
#include <QWidget>

#include "models/RecordingMarker.h"
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

// Nested layouts (QScrollArea > QVBoxLayout > ...) settle across several posted
// LayoutRequest events rather than a single processEvents() call.
void SettleLayout() {
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents();
}

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

TEST_F(EditExportPageTest, PlaceholderBannerRemoved) {
    // The 0.11 placeholder banner was removed in 0.9.0 when the real engine shipped.
    // Verify it is no longer present in the widget tree.
    EditExportPage page;
    auto* banner = page.findChild<QFrame*>(QStringLiteral("editExportPlaceholderBanner"));
    EXPECT_EQ(banner, nullptr);
}

// ---- Three-step flow (Review -> Edit -> Output) reachability ----

TEST_F(EditExportPageTest, PrimaryButton_FromReview_AdvancesToEdit) {
    EditExportPage page;
    ASSERT_EQ(page.phase(), EditExportPage::Phase::Review);
    auto* primary = page.findChild<QPushButton*>(QStringLiteral("editExportPrimaryBtn"));
    ASSERT_NE(primary, nullptr);
    primary->click();
    EXPECT_EQ(page.phase(), EditExportPage::Phase::Edit);
}

TEST_F(EditExportPageTest, PrimaryButton_FromEdit_AdvancesToOutput) {
    EditExportPage page;
    page.setPhase(EditExportPage::Phase::Edit);
    auto* primary = page.findChild<QPushButton*>(QStringLiteral("editExportPrimaryBtn"));
    ASSERT_NE(primary, nullptr);
    primary->click();
    EXPECT_EQ(page.phase(), EditExportPage::Phase::Output);
}

TEST_F(EditExportPageTest, BackButton_FromEdit_ReturnsToReview) {
    EditExportPage page;
    page.setPhase(EditExportPage::Phase::Edit);
    int signal_count = 0;
    QObject::connect(&page, &EditExportPage::backRequested, &page, [&signal_count]() { ++signal_count; });
    auto* back_btn = page.findChild<QPushButton*>(QStringLiteral("editExportBackBtn"));
    ASSERT_NE(back_btn, nullptr);
    back_btn->click();
    EXPECT_EQ(page.phase(), EditExportPage::Phase::Review);
    EXPECT_EQ(signal_count, 0) << "Back from Edit steps to Review in-page, it must not close the overlay";
}

TEST_F(EditExportPageTest, BackButton_FromOutput_ReturnsToEdit) {
    EditExportPage page;
    page.setPhase(EditExportPage::Phase::Output);
    int signal_count = 0;
    QObject::connect(&page, &EditExportPage::backRequested, &page, [&signal_count]() { ++signal_count; });
    auto* back_btn = page.findChild<QPushButton*>(QStringLiteral("editExportBackBtn"));
    ASSERT_NE(back_btn, nullptr);
    back_btn->click();
    EXPECT_EQ(page.phase(), EditExportPage::Phase::Edit);
    EXPECT_EQ(signal_count, 0) << "Back from Output steps to Edit in-page, it must not close the overlay";
}

TEST_F(EditExportPageTest, BackButton_FromReview_StillClosesOverlay) {
    // Review is the first step: Back has nowhere in-page to go, so it must keep
    // emitting backRequested() (existing behavior, relied on by the overlay).
    EditExportPage page;
    ASSERT_EQ(page.phase(), EditExportPage::Phase::Review);
    int signal_count = 0;
    QObject::connect(&page, &EditExportPage::backRequested, &page, [&signal_count]() { ++signal_count; });
    auto* back_btn = page.findChild<QPushButton*>(QStringLiteral("editExportBackBtn"));
    ASSERT_NE(back_btn, nullptr);
    back_btn->click();
    EXPECT_EQ(signal_count, 1);
}

TEST_F(EditExportPageTest, PrimaryButtonLabel_MatchesActualNextStep) {
    EditExportPage page;
    auto* primary = page.findChild<QPushButton*>(QStringLiteral("editExportPrimaryBtn"));
    ASSERT_NE(primary, nullptr);

    page.setPhase(EditExportPage::Phase::Review);
    EXPECT_EQ(primary->text(), QStringLiteral("Continue to edit"));

    page.setPhase(EditExportPage::Phase::Edit);
    EXPECT_EQ(primary->text(), QStringLiteral("Continue to output"));
}

TEST_F(EditExportPageTest, Stepper_HighlightsCurrentPhaseOnly) {
    EditExportPage page;
    auto activeStyle = [](QLabel* lbl) { return lbl->styleSheet().contains(QStringLiteral("border-bottom")); };

    // Locate the three stepper labels by text since they have no object names.
    QLabel* stepper_review = nullptr;
    QLabel* stepper_edit = nullptr;
    QLabel* stepper_output = nullptr;
    for (auto* lbl : page.findChildren<QLabel*>()) {
        if (lbl->text() == QStringLiteral("Review"))
            stepper_review = lbl;
        else if (lbl->text() == QStringLiteral("Edit"))
            stepper_edit = lbl;
        else if (lbl->text() == QStringLiteral("Output"))
            stepper_output = lbl;
    }
    ASSERT_NE(stepper_review, nullptr);
    ASSERT_NE(stepper_edit, nullptr);
    ASSERT_NE(stepper_output, nullptr);

    page.setPhase(EditExportPage::Phase::Review);
    EXPECT_TRUE(activeStyle(stepper_review));
    EXPECT_FALSE(activeStyle(stepper_edit));
    EXPECT_FALSE(activeStyle(stepper_output));

    page.setPhase(EditExportPage::Phase::Edit);
    EXPECT_FALSE(activeStyle(stepper_review));
    EXPECT_TRUE(activeStyle(stepper_edit));
    EXPECT_FALSE(activeStyle(stepper_output));

    page.setPhase(EditExportPage::Phase::Output);
    EXPECT_FALSE(activeStyle(stepper_review));
    EXPECT_FALSE(activeStyle(stepper_edit));
    EXPECT_TRUE(activeStyle(stepper_output));
}

// ---- Dead/placeholder controls removed ----

TEST_F(EditExportPageTest, SplitChapterButtonRemoved) {
    // Chapter export is out of scope (ADR 0022): the button must not exist.
    EditExportPage page;
    auto* btn = page.findChild<QPushButton*>(QStringLiteral("editExportSplitChapterBtn"));
    EXPECT_EQ(btn, nullptr);
    for (auto* b : page.findChildren<QPushButton*>())
        EXPECT_NE(b->text(), QStringLiteral("Split Chapter"));
}

TEST_F(EditExportPageTest, BrowseDestButtonRemoved) {
    // The save-mode model (new file beside source / overwrite in place) leaves no
    // user-choosable destination, so the permanently-disabled Browse button was
    // dead UI and is removed rather than wired up.
    EditExportPage page;
    for (auto* b : page.findChildren<QPushButton*>())
        EXPECT_NE(b->text(), QStringLiteral("Browse\xe2\x80\xa6"));
}

// ---- Honest result/error strings ----

TEST_F(EditExportPageTest, DoneResult_NeverShowsDemoFilename) {
    EditExportPage page;
    page.setPhase(EditExportPage::Phase::Done);
    auto* detail = page.findChild<QLabel*>(QStringLiteral("editExportResultDetail"));
    ASSERT_NE(detail, nullptr);
    EXPECT_FALSE(detail->text().contains(QStringLiteral("Sprint-demo.mp4")));
}

TEST_F(EditExportPageTest, FailedResult_NeverShowsHardcodedDiskFull) {
    EditExportPage page;
    page.setPhase(EditExportPage::Phase::Failed);
    auto* detail = page.findChild<QLabel*>(QStringLiteral("editExportResultDetail"));
    ASSERT_NE(detail, nullptr);
    EXPECT_FALSE(detail->text().contains(QStringLiteral("disk full")));
}

// ---- Marker pins on the timeline ----

TEST_F(EditExportPageTest, MarkerPins_OneRenderedPerMarker) {
    EditExportPage page;
    page.resize(900, 700);
    page.show();

    EditContext ctx;
    ctx.output_path = QStringLiteral("C:\\test\\recording.mkv");
    ctx.duration_seconds = 100.0;
    RecordingMarker m1;
    m1.time_ms = 25000; // 25% through a 100s recording
    RecordingMarker m2;
    m2.time_ms = 75000; // 75%
    ctx.markers = {m1, m2};
    page.setEditContext(ctx);
    page.setPhase(EditExportPage::Phase::Edit);
    SettleLayout();

    auto pins = page.findChildren<QFrame*>(QStringLiteral("editTimelineMarkerPin"));
    EXPECT_EQ(pins.size(), 2);
}

TEST_F(EditExportPageTest, MarkerPins_PositionedProportionally) {
    EditExportPage page;
    page.resize(900, 700);
    page.show();

    EditContext ctx;
    ctx.output_path = QStringLiteral("C:\\test\\recording.mkv");
    ctx.duration_seconds = 100.0;
    RecordingMarker mid;
    mid.time_ms = 50000; // 50% through
    ctx.markers = {mid};
    page.setEditContext(ctx);
    page.setPhase(EditExportPage::Phase::Edit);
    SettleLayout();

    auto pins = page.findChildren<QFrame*>(QStringLiteral("editTimelineMarkerPin"));
    ASSERT_EQ(pins.size(), 1);
    auto* pin = pins.front();
    auto* container = qobject_cast<QWidget*>(pin->parent());
    ASSERT_NE(container, nullptr);
    ASSERT_GT(container->width(), 0);
    const double fraction = static_cast<double>(pin->x()) / static_cast<double>(container->width());
    EXPECT_NEAR(fraction, 0.5, 0.1);
}

TEST_F(EditExportPageTest, MarkerPins_NoneWhenDurationUnknown) {
    // Duration 0/unknown must never crash and must simply render no pins.
    EditExportPage page;
    page.resize(900, 700);
    page.show();

    EditContext ctx;
    ctx.output_path = QStringLiteral("C:\\test\\recording.mkv");
    ctx.duration_seconds = 0.0; // unknown
    RecordingMarker m;
    m.time_ms = 1000;
    ctx.markers = {m};
    page.setEditContext(ctx);
    page.setPhase(EditExportPage::Phase::Edit);
    SettleLayout();

    auto pins = page.findChildren<QFrame*>(QStringLiteral("editTimelineMarkerPin"));
    EXPECT_TRUE(pins.isEmpty());
    SUCCEED(); // primarily: constructing/laying out this state must not crash
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
