#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFrame>
#include <QLabel>
#include <QMetaMethod>
#include <QMouseEvent>
#include <QPoint>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include <cstdlib>

#include "models/EditTimelineModel.h"
#include "models/RecordingMarker.h"
#include "pages/EditExportPage.h"
#include "ui/widgets/EditTimeline.h"

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

// Pumps a real Qt event loop for `ms` wall-clock milliseconds -- unlike
// SettleLayout's processEvents() loop, this actually lets QTimer-driven
// code (e.g. EditExportPage's 33 ms preview_timer_) fire repeatedly.
void WaitMs(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Synthesize mouse events directly (the test binaries link gtest, not Qt Test).
void SendMouse(QWidget* w, QEvent::Type type, const QPoint& pos) {
    const Qt::MouseButton button = Qt::LeftButton;
    const Qt::MouseButtons buttons = (type == QEvent::MouseButtonRelease) ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent ev(type, QPointF(pos), QPointF(w->mapToGlobal(pos)), button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &ev);
}

EditContext MakeContext(double duration_seconds, std::vector<RecordingMarker> markers = {}) {
    EditContext ctx;
    ctx.output_path = QStringLiteral("C:\\test\\recording.mkv");
    ctx.duration_seconds = duration_seconds;
    ctx.markers = std::move(markers);
    return ctx;
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

TEST_F(EditExportPageTest, DetailsCardValuesAreRightAligned) {
    // Design-suite Details card: mono values right-aligned against the keys.
    EditExportPage page;
    auto* dur = page.findChild<QLabel*>(QStringLiteral("editFactDuration"));
    ASSERT_NE(dur, nullptr);
    EXPECT_TRUE(dur->alignment().testFlag(Qt::AlignRight));
}

TEST_F(EditExportPageTest, SaveButtonLivesInTheBottomActionBar) {
    // The primary action sits bottom-right, like the Record page's transport
    // actions — not in the top mode bar.
    EditExportPage page;
    auto* primary = page.findChild<QPushButton*>(QStringLiteral("editExportPrimaryBtn"));
    ASSERT_NE(primary, nullptr);
    ASSERT_NE(primary->parentWidget(), nullptr);
    EXPECT_EQ(primary->parentWidget()->objectName(), QStringLiteral("editExportActionBar"));

    page.setPhase(EditExportPage::Phase::Output);
    EXPECT_FALSE(primary->isHidden());
    EXPECT_EQ(primary->text(), QStringLiteral("Save && export"));
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

TEST_F(EditExportPageTest, TimelineHasNoButtonRowAboveIt) {
    // Trim is direct manipulation on the timeline now: the Trim / Add Marker
    // button row (and its duration readout) above the strip is gone.
    EditExportPage page;
    for (auto* b : page.findChildren<QPushButton*>()) {
        EXPECT_NE(b->text(), QStringLiteral("Trim"));
        EXPECT_NE(b->text(), QStringLiteral("Add Marker"));
    }
    EXPECT_EQ(page.findChild<QWidget*>(QStringLiteral("editExportEditControls")), nullptr);
}

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

// ---- Markers on the timeline ----

TEST_F(EditExportPageTest, MarkersFlowFromContextToTheTimeline) {
    EditExportPage page;
    page.resize(900, 700);
    page.show();

    RecordingMarker m1;
    m1.time_ms = 25000; // 25% through a 100s recording
    RecordingMarker m2;
    m2.time_ms = 75000; // 75%
    page.setEditContext(MakeContext(100.0, {m1, m2}));
    page.setPhase(EditExportPage::Phase::Edit);
    SettleLayout();

    auto* timeline = page.findChild<ui::widgets::EditTimeline*>(QStringLiteral("editTimeline"));
    ASSERT_NE(timeline, nullptr);
    ASSERT_EQ(timeline->markers().size(), 2u);
    EXPECT_EQ(timeline->markers()[0].time_ms, 25000u);
    EXPECT_EQ(timeline->markers()[1].time_ms, 75000u);
    EXPECT_EQ(timeline->durationMs(), 100000);
}

TEST_F(EditExportPageTest, TimelineMapsTimeProportionallyToPixels) {
    EditExportPage page;
    page.resize(900, 700);
    page.show();

    page.setEditContext(MakeContext(100.0));
    page.setPhase(EditExportPage::Phase::Edit);
    SettleLayout();

    auto* timeline = page.findChild<ui::widgets::EditTimeline*>(QStringLiteral("editTimeline"));
    ASSERT_NE(timeline, nullptr);
    ASSERT_GT(timeline->width(), 0);
    const int span = timeline->xForMs(100000) - timeline->xForMs(0);
    ASSERT_GT(span, 0);
    const double fraction = static_cast<double>(timeline->xForMs(50000) - timeline->xForMs(0)) / span;
    EXPECT_NEAR(fraction, 0.5, 0.02);
}

TEST_F(EditExportPageTest, UnknownDurationRendersAnInertTimeline) {
    // Duration 0/unknown must never crash; the timeline stays inert (no trim
    // edits, no playback).
    EditExportPage page;
    page.resize(900, 700);
    page.show();

    RecordingMarker m;
    m.time_ms = 1000;
    page.setEditContext(MakeContext(0.0, {m}));
    page.setPhase(EditExportPage::Phase::Edit);
    SettleLayout();

    page.setPreviewPlaying(true);
    EXPECT_FALSE(page.isPreviewPlaying());

    auto* timeline = page.findChild<ui::widgets::EditTimeline*>(QStringLiteral("editTimeline"));
    ASSERT_NE(timeline, nullptr);
    EXPECT_EQ(timeline->durationMs(), 0);
    SUCCEED(); // primarily: constructing/laying out this state must not crash
}

// ---- Trim handles (direct manipulation) ----

TEST_F(EditExportPageTest, TrimHandlesConstrainEachOtherWhileDragging) {
    ui::widgets::EditTimeline timeline;
    timeline.resize(610, timeline.height());
    timeline.show();
    SettleLayout();
    timeline.setDurationMs(100000);
    timeline.setTrimRangeMs(0, 50000);

    // Grab the start handle and drag it far past the end handle: it must stop
    // at the minimum gap before the end handle, never cross it.
    const int track_y = 48; // inside the track band
    SendMouse(&timeline, QEvent::MouseButtonPress, QPoint(timeline.xForMs(0), track_y));
    SendMouse(&timeline, QEvent::MouseMove, QPoint(timeline.xForMs(80000), track_y));
    SendMouse(&timeline, QEvent::MouseButtonRelease, QPoint(timeline.xForMs(80000), track_y));

    EXPECT_EQ(timeline.trimStartMs(), 50000 - kMinTrimGapMs);
    EXPECT_EQ(timeline.trimEndMs(), 50000);
}

TEST_F(EditExportPageTest, TrimHandleDragEmitsReleaseSignalOnce) {
    ui::widgets::EditTimeline timeline;
    timeline.resize(610, timeline.height());
    timeline.show();
    SettleLayout();
    timeline.setDurationMs(100000);

    int released = 0;
    qint64 released_end = -1;
    QObject::connect(&timeline, &ui::widgets::EditTimeline::trimHandleReleased, &timeline, [&](qint64, qint64 end_ms) {
        ++released;
        released_end = end_ms;
    });

    // Drag the end handle inward to ~60%.
    const int track_y = 48;
    SendMouse(&timeline, QEvent::MouseButtonPress, QPoint(timeline.xForMs(100000), track_y));
    SendMouse(&timeline, QEvent::MouseMove, QPoint(timeline.xForMs(60000), track_y));
    SendMouse(&timeline, QEvent::MouseButtonRelease, QPoint(timeline.xForMs(60000), track_y));

    EXPECT_EQ(released, 1);
    EXPECT_NEAR(static_cast<double>(released_end), 60000.0, 500.0);
    EXPECT_TRUE(timeline.isTrimmed());
}

// ---- Scrubbing pause/resume semantics ----

TEST_F(EditExportPageTest, ScrubPausesAndResumesOnlyIfPreviouslyPlaying) {
    EditExportPage page;
    page.resize(900, 700);
    page.show();
    page.setEditContext(MakeContext(100.0));
    page.setPhase(EditExportPage::Phase::Edit);
    SettleLayout();

    auto* timeline = page.findChild<ui::widgets::EditTimeline*>(QStringLiteral("editTimeline"));
    ASSERT_NE(timeline, nullptr);
    ASSERT_GT(timeline->width(), 0);

    page.setPreviewPlaying(true);
    ASSERT_TRUE(page.isPreviewPlaying());

    // Press mid-track: the preview pauses for the duration of the drag.
    const QPoint mid(timeline->xForMs(50000), 48);
    SendMouse(timeline, QEvent::MouseButtonPress, mid);
    EXPECT_FALSE(page.isPreviewPlaying());

    const QPoint later(timeline->xForMs(70000), 48);
    SendMouse(timeline, QEvent::MouseMove, later);
    EXPECT_FALSE(page.isPreviewPlaying());
    EXPECT_NEAR(static_cast<double>(page.previewPositionMs()), 70000.0, 500.0);

    // Release: the preview was playing before the scrub, so it resumes.
    SendMouse(timeline, QEvent::MouseButtonRelease, later);
    EXPECT_TRUE(page.isPreviewPlaying());
}

TEST_F(EditExportPageTest, PreviewStopsAtEndOfClipWithNoAudioStream) {
    EditExportPage page;
    page.resize(900, 700);
    page.show();
    page.setEditContext(MakeContext(1.0)); // 1-second clip: reaches the end in a couple of ticks
    page.setPhase(EditExportPage::Phase::Edit);
    SettleLayout();

    page.setPreviewPlaying(true);
    ASSERT_TRUE(page.isPreviewPlaying());

    // Let the preview timer (33 ms) run past the 1-second duration.
    WaitMs(1200);

    EXPECT_FALSE(page.isPreviewPlaying());
    EXPECT_EQ(page.previewPositionMs(), 1000);
}

TEST_F(EditExportPageTest, ScrubWhilePausedStaysPaused) {
    EditExportPage page;
    page.resize(900, 700);
    page.show();
    page.setEditContext(MakeContext(100.0));
    page.setPhase(EditExportPage::Phase::Edit);
    SettleLayout();

    auto* timeline = page.findChild<ui::widgets::EditTimeline*>(QStringLiteral("editTimeline"));
    ASSERT_NE(timeline, nullptr);
    ASSERT_GT(timeline->width(), 0);
    ASSERT_FALSE(page.isPreviewPlaying());

    const QPoint mid(timeline->xForMs(40000), 48);
    SendMouse(timeline, QEvent::MouseButtonPress, mid);
    SendMouse(timeline, QEvent::MouseButtonRelease, mid);
    EXPECT_FALSE(page.isPreviewPlaying()) << "paused before the scrub => stays paused after release";
    EXPECT_NEAR(static_cast<double>(page.previewPositionMs()), 40000.0, 500.0);
}

// ---- Play button centering (P9) ----

TEST_F(EditExportPageTest, PlayButtonIsCenteredOverThePlayerSurface) {
    // The play/pause toggle floats centered in the video rectangle, not pinned
    // to its top edge — it shares the player frame with the surface and lands
    // near the frame's vertical center.
    EditExportPage page;
    page.resize(1000, 800);
    page.setEditContext(MakeContext(100.0));
    page.setPhase(EditExportPage::Phase::Review);
    page.show();
    // updatePlayerHeight() re-sizes the frame from a resize-event filter after
    // the first layout pass; pump a real event loop so the grid re-lays out
    // against the frame's final height before asserting geometry.
    SettleLayout();
    WaitMs(50);
    SettleLayout();

    auto* frame = page.findChild<QFrame*>(QStringLiteral("editExportPlayer"));
    ASSERT_NE(frame, nullptr);
    auto* play_btn = page.findChild<QPushButton*>(QStringLiteral("editExportPlayPauseBtn"));
    ASSERT_NE(play_btn, nullptr);

    // The button is overlaid inside the player frame (not stacked above it).
    EXPECT_EQ(play_btn->parentWidget(), frame);

    ASSERT_GT(frame->height(), 120);
    const int frame_center_y = frame->rect().center().y();
    const int btn_center_y = play_btn->geometry().center().y(); // in frame coords
    // Tight tolerance: Qt::AlignCenter puts the button within rounding distance
    // of the frame center; the pre-fix bug offset it by ~54 px, so 10 px still
    // cleanly separates "centered" from the failure mode.
    EXPECT_LE(std::abs(btn_center_y - frame_center_y), 10)
        << "play button center " << btn_center_y << " should sit near the frame center " << frame_center_y << " [frame "
        << frame->width() << "x" << frame->height() << ", btn geo " << play_btn->geometry().x() << ","
        << play_btn->geometry().y() << " parent=" << play_btn->parentWidget()->objectName().toStdString() << "]";
}

// ---- Unified empty-value copy in the post-recording report (P10) ----

TEST_F(EditExportPageTest, ReviewReportUsesEmDashForMissingValuesNotUnavailable) {
    // With no diagnostics snapshot and no drift measurement, every empty value in
    // the post-recording report reads as the unified em dash — never a stray
    // "unavailable" string or an en dash.
    EditExportPage page;
    EditContext ctx;
    ctx.output_path = QStringLiteral("C:\\test\\recording.mkv");
    ctx.duration_seconds = 100.0;
    ctx.av_drift_available = false; // no drift data
    page.setEditContext(ctx);
    page.setPhase(EditExportPage::Phase::Review);

    const QString em_dash = QString::fromUtf8("\xe2\x80\x94");
    const QString en_dash = QString::fromUtf8("\xe2\x80\x93");

    QLabel* drops = nullptr;
    QLabel* drift = nullptr;
    QLabel* health = nullptr;
    for (auto* lbl : page.findChildren<QLabel*>()) {
        if (lbl->text().startsWith(QStringLiteral("Frame drops:")))
            drops = lbl;
        else if (lbl->text().startsWith(QStringLiteral("Peak A/V drift:")))
            drift = lbl;
        else if (lbl->text().startsWith(QStringLiteral("Pipeline health:")))
            health = lbl;
    }
    ASSERT_NE(drops, nullptr);
    ASSERT_NE(drift, nullptr);
    ASSERT_NE(health, nullptr);

    for (QLabel* lbl : {drops, drift, health}) {
        EXPECT_TRUE(lbl->text().contains(em_dash)) << lbl->text().toStdString();
        EXPECT_FALSE(lbl->text().contains(en_dash)) << lbl->text().toStdString();
        EXPECT_FALSE(lbl->text().contains(QStringLiteral("unavailable"))) << lbl->text().toStdString();
    }
}

// Regression: a healthy 144 Hz display recorded at 60 fps CFR deliberately
// discards a large fraction of source frames via coalescing/pacing to hit the
// target rate -- none of that is a real drop. The report must count only
// encoder-backpressure drops, not the coalesced/CFR-pacing categories.
TEST_F(EditExportPageTest, ReviewReportCountsOnlyRealDrops_NotCoalescedPacing) {
    EditExportPage page;
    EditContext ctx;
    ctx.output_path = QStringLiteral("C:\\test\\recording.mkv");
    ctx.duration_seconds = 100.0;
    ctx.completed_snapshot.valid = true;
    ctx.completed_snapshot.capture.frames_emitted = 6000;           // 100 s @ 60 fps
    ctx.completed_snapshot.capture.frames_dropped_coalesced = 4000; // benign pacing
    ctx.completed_snapshot.capture.frames_dropped_cfr = 100;        // benign
    ctx.completed_snapshot.capture.frames_dropped_backpressure = 3; // the only real drops
    page.setEditContext(ctx);
    page.setPhase(EditExportPage::Phase::Review);

    QLabel* drops = nullptr;
    for (auto* lbl : page.findChildren<QLabel*>()) {
        if (lbl->text().startsWith(QStringLiteral("Frame drops:")))
            drops = lbl;
    }
    ASSERT_NE(drops, nullptr);
    // 3 / (6000 + 3) * 100 rounds to 0.0%; a coalesced-inclusive total (4103
    // dropped of 10103) would have shown roughly 40%.
    EXPECT_EQ(drops->text(), QStringLiteral("Frame drops: 0.0%")) << drops->text().toStdString();
}

// The mirror image of the test above: a frame whose GPU conversion failed is picture
// the recording lost, and it used to be filed under benign CFR pacing. The review
// panel then read 0.0% while the Diagnostics capture card flagged the same session.
TEST_F(EditExportPageTest, ReviewReportCountsProcessingFailuresAsRealDrops) {
    EditExportPage page;
    EditContext ctx;
    ctx.output_path = QStringLiteral("C:\\test\\recording.mkv");
    ctx.duration_seconds = 15.0;
    ctx.completed_snapshot.valid = true;
    ctx.completed_snapshot.capture.frames_emitted = 900;
    ctx.completed_snapshot.capture.frames_dropped_coalesced = 4000;         // benign
    ctx.completed_snapshot.capture.frames_dropped_cfr = 50;                 // benign start-up pacing
    ctx.completed_snapshot.capture.frames_dropped_processing_failure = 100; // real: picture lost
    page.setEditContext(ctx);
    page.setPhase(EditExportPage::Phase::Review);

    QLabel* drops = nullptr;
    for (auto* lbl : page.findChildren<QLabel*>()) {
        if (lbl->text().startsWith(QStringLiteral("Frame drops:")))
            drops = lbl;
    }
    ASSERT_NE(drops, nullptr);
    // 100 / (900 + 100) = 10.0%. Before the fix this read 0.0%.
    EXPECT_EQ(drops->text(), QStringLiteral("Frame drops: 10.0%")) << drops->text().toStdString();
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
