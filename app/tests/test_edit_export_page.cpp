#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPixmap>
#include <QPoint>
#include <QPushButton>
#include <QRect>
#include <QScrollArea>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cstdlib>

#include "models/EditTimelineModel.h"
#include "models/RecordingMarker.h"
#include "pages/EditExportPage.h"
#include "ui/theme/ExoSnapTheme.h"
#include "ui/widgets/EditDetailsRail.h"
#include "ui/widgets/EditTimeline.h"
#include "ui/widgets/ExportPanel.h"

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
// code (e.g. EditExportPage's preview_timer_) fire repeatedly.
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

// Answers the modal confirmation from outside. QDialog::exec() spins its own
// event loop, so the click has to come from a timer that fires inside it; the
// watchdog closes anything still standing so a missing button fails the
// assertions below instead of hanging the suite.
void AnswerModalDialog(const QString& button_text, bool* out_appeared) {
    auto* timer = new QTimer;
    auto* attempts = new int(0);
    timer->setInterval(10);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, button_text, out_appeared, attempts]() {
        auto finish = [timer, attempts]() {
            timer->stop();
            delete attempts;
            timer->deleteLater();
        };
        if (++*attempts > 200) { // ~2 s
            if (auto* stuck = QApplication::activeModalWidget())
                stuck->close();
            finish();
            return;
        }
        auto* modal = QApplication::activeModalWidget();
        if (modal == nullptr)
            return;
        for (auto* btn : modal->findChildren<QAbstractButton*>()) {
            if (QString(btn->text()).remove(QLatin1Char('&')) != button_text)
                continue;
            if (out_appeared != nullptr)
                *out_appeared = true;
            btn->click();
            finish();
            return;
        }
    });
    timer->start();
}

ui::widgets::ExportPanel* ExportPanelOf(EditExportPage& page) {
    return page.findChild<ui::widgets::ExportPanel*>(QStringLiteral("exportPanel"));
}

QPushButton* ExportButton(EditExportPage& page) {
    return page.findChild<QPushButton*>(QStringLiteral("editExportPrimaryBtn"));
}

// Selects a save mode on the export panel and presses the action bar's Export
// button — the whole path a user takes now that no card sits in between.
// Returns false when the panel carries no save-mode combo, so the overwrite
// cases can report that rather than assert against a control that is not there.
bool RequestExport(EditExportPage& page, const QString& mode_key) {
    auto* panel = ExportPanelOf(page);
    if (panel == nullptr)
        return false;
    auto* combo = panel->findChild<QComboBox*>(QStringLiteral("outputSaveModeCombo"));
    if (combo == nullptr)
        return false;
    const int index = combo->findData(mode_key);
    if (index < 0)
        return false;
    combo->setCurrentIndex(index);
    auto* button = ExportButton(page);
    if (button == nullptr)
        return false;
    button->click();
    return true;
}

EditContext MakeContext(double duration_seconds, std::vector<RecordingMarker> markers = {}) {
    EditContext ctx;
    ctx.output_path = QStringLiteral("C:\\test\\recording.mkv");
    ctx.duration_seconds = duration_seconds;
    ctx.markers = std::move(markers);
    return ctx;
}

// Starts a real export against a master that cannot be opened. runExport() flips
// the running flag synchronously and only clears it from the queued completion
// callback, so the caller owns a deterministic "export is running" window for as
// long as it does not pump the event loop.
void StartDoomedExport(EditExportPage& page) {
    EditContext ctx;
    ctx.output_path = QDir::temp().filePath(QStringLiteral("exosnap-edit-export-test.mkv"));
    ctx.mkv_master_path = QDir::temp().filePath(QStringLiteral("exosnap-edit-export-missing.mkv"));
    page.setEditContext(ctx);
    auto* button = ExportButton(page);
    ASSERT_NE(button, nullptr);
    button->click();
}

QString ReportTooltip(EditExportPage& page) {
    auto* icon = page.findChild<QLabel*>(QStringLiteral("editReportIcon"));
    return icon != nullptr ? icon->toolTip() : QString();
}

TEST_F(EditExportPageTest, ConstructsWithoutCrash) {
    EditExportPage page;
    page.show();
    SUCCEED();
}

// ---- One view: nothing is gated behind a step any more ----

TEST_F(EditExportPageTest, PlayerAndTimelineAreBothPresentFromTheStart) {
    EditExportPage page;
    auto* player = page.findChild<QFrame*>(QStringLiteral("editExportPlayer"));
    ASSERT_NE(player, nullptr);
    EXPECT_FALSE(player->isHidden());

    auto* timeline = page.findChild<ui::widgets::EditTimeline*>(QStringLiteral("editTimeline"));
    ASSERT_NE(timeline, nullptr);
    EXPECT_FALSE(timeline->isHidden());
}

TEST_F(EditExportPageTest, StepperAndPhasePanelsAreGone) {
    // The surface is one view: no stepper, no per-phase panels, no secondary
    // action button.
    EditExportPage page;
    for (const QString& name : {QStringLiteral("editExportStepper"), QStringLiteral("editExportReviewPanel"),
                                QStringLiteral("editExportOutputPanel"), QStringLiteral("editExportExportingPanel"),
                                QStringLiteral("editExportResultPanel"), QStringLiteral("editExportSecondaryBtn")}) {
        EXPECT_EQ(page.findChild<QWidget*>(name), nullptr) << name.toStdString();
    }
    for (auto* lbl : page.findChildren<QLabel*>()) {
        EXPECT_NE(lbl->text(), QStringLiteral("Review"));
        EXPECT_NE(lbl->text(), QStringLiteral("Output"));
    }
}

TEST_F(EditExportPageTest, ExportButtonLivesInTheBottomActionBar) {
    // The single action sits bottom-right, like the Record page's transport
    // actions — not in the top mode bar. No ellipsis: it starts the export
    // rather than opening anything.
    EditExportPage page;
    auto* primary = ExportButton(page);
    ASSERT_NE(primary, nullptr);
    ASSERT_NE(primary->parentWidget(), nullptr);
    EXPECT_EQ(primary->parentWidget()->objectName(), QStringLiteral("editExportActionBar"));
    EXPECT_EQ(primary->text(), QStringLiteral("Export"));
    EXPECT_FALSE(primary->isHidden());
}

// ---- Export settings live in the rail, not in a card over the view ----

TEST_F(EditExportPageTest, ExportPanelIsEmbeddedInTheRailUnderTheDetailsCard) {
    EditExportPage page;
    auto* panel = ExportPanelOf(page);
    ASSERT_NE(panel, nullptr);
    auto* rail = page.findChild<ui::widgets::EditDetailsRail*>(QStringLiteral("editDetailsRail"));
    ASSERT_NE(rail, nullptr);

    // Same column, and below the details card in it.
    ASSERT_EQ(panel->parentWidget(), rail->parentWidget());
    EXPECT_GT(panel->y(), rail->y());
    // Visible from the start: nothing has to be opened to reach the settings.
    EXPECT_TRUE(panel->isVisibleTo(&page));
}

TEST_F(EditExportPageTest, NoExportCardOverlayIsLeftOnTheSurface) {
    // The floating export card is gone; the only overlay-shaped thing that may
    // sit over the view is nothing at all.
    EditExportPage page;
    EXPECT_EQ(page.findChild<QWidget*>(QStringLiteral("exportOverlay")), nullptr);
    EXPECT_EQ(page.findChild<QWidget*>(QStringLiteral("exportOverlayCard")), nullptr);
}

TEST_F(EditExportPageTest, ExportButtonStartsTheExportDirectly) {
    // No intermediate card: one click on the action bar runs the export against
    // the panel's current settings (and fails here for want of a master, which
    // is what proves runExport() was entered).
    EditExportPage page;
    auto* panel = ExportPanelOf(page);
    ASSERT_NE(panel, nullptr);
    ASSERT_EQ(panel->state(), ui::widgets::ExportPanel::State::Options);

    ExportButton(page)->click();
    EXPECT_NE(panel->state(), ui::widgets::ExportPanel::State::Options);
}

TEST_F(EditExportPageTest, ExportButtonIsOutOfReachWhileARunIsInFlight) {
    // A second click would reach runExport(), whose join() on the previous
    // thread blocks the UI thread.
    EditExportPage page;
    StartDoomedExport(page);
    ASSERT_TRUE(page.isExportRunning());
    EXPECT_FALSE(ExportButton(page)->isEnabled());

    WaitMs(50);
    SettleLayout();
    ASSERT_FALSE(page.isExportRunning());
    EXPECT_TRUE(ExportButton(page)->isEnabled());
}

TEST_F(EditExportPageTest, DetailsRailReceivesTheRecordingFacts) {
    EditExportPage page;
    page.setRecordingInfo(QStringLiteral("C:\\test\\recording.mkv"), QStringLiteral("00:04:18"),
                          QStringLiteral("612 MB"), QStringLiteral("2560 x 1440"), QStringLiteral("60 fps CFR"),
                          QStringLiteral("AV1"), QStringLiteral("Opus"), QStringLiteral("MKV"));

    ASSERT_NE(page.findChild<QWidget*>(QStringLiteral("editDetailsRail")), nullptr);

    auto* dur = page.findChild<QLabel*>(QStringLiteral("factDurationValue"));
    if (dur == nullptr)
        GTEST_SKIP() << "details rail carries no fact rows in this build";
    EXPECT_EQ(dur->text(), QStringLiteral("00:04:18"));

    auto* sz = page.findChild<QLabel*>(QStringLiteral("factSizeValue"));
    ASSERT_NE(sz, nullptr);
    EXPECT_EQ(sz->text(), QStringLiteral("612 MB"));

    auto* vid = page.findChild<QLabel*>(QStringLiteral("factVideoValue"));
    ASSERT_NE(vid, nullptr);
    EXPECT_EQ(vid->text(), QStringLiteral("AV1"));

    auto* container = page.findChild<QLabel*>(QStringLiteral("factContainerValue"));
    ASSERT_NE(container, nullptr);
    EXPECT_EQ(container->text(), QStringLiteral("MKV"));
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

// ---- Overwrite confirmation ----

// "Overwrite original" replaces the user's only copy of the recording, so it
// must not run off a single click. Declining leaves the export unstarted.
TEST_F(EditExportPageTest, OverwriteExport_Declined_DoesNotStart) {
    EditExportPage page;

    bool asked = false;
    AnswerModalDialog(QStringLiteral("Keep original"), &asked);
    if (!RequestExport(page, QStringLiteral("overwrite")))
        GTEST_SKIP() << "export card carries no save-mode combo in this build";

    EXPECT_TRUE(asked) << "overwriting the original must be confirmed first";
    EXPECT_FALSE(page.isExportRunning()) << "declining must leave the export unstarted";
    EXPECT_EQ(ExportPanelOf(page)->state(), ui::widgets::ExportPanel::State::Options);
}

// Confirming actually proceeds. The export then fails for want of an edit
// master (none is set in this fixture) -- reaching the card's Failed state is
// precisely what proves runExport() was entered rather than skipped.
TEST_F(EditExportPageTest, OverwriteExport_Confirmed_Starts) {
    EditExportPage page;

    bool asked = false;
    AnswerModalDialog(QStringLiteral("Overwrite"), &asked);
    if (!RequestExport(page, QStringLiteral("overwrite")))
        GTEST_SKIP() << "export card carries no save-mode combo in this build";

    EXPECT_TRUE(asked);
    auto* panel = ExportPanelOf(page);
    ASSERT_NE(panel, nullptr);
    EXPECT_NE(panel->state(), ui::widgets::ExportPanel::State::Options) << "confirming must start the export";
}

// Writing a new file destroys nothing, so it must not interrupt the user with
// a question at all.
TEST_F(EditExportPageTest, NewFileExport_StartsWithoutAsking) {
    EditExportPage page;

    bool asked = false;
    AnswerModalDialog(QStringLiteral("Keep original"), &asked);
    if (!RequestExport(page, QStringLiteral("new")))
        GTEST_SKIP() << "export card carries no save-mode combo in this build";

    EXPECT_FALSE(asked) << "a new-file export replaces nothing and must not prompt";
    auto* panel = ExportPanelOf(page);
    ASSERT_NE(panel, nullptr);
    EXPECT_NE(panel->state(), ui::widgets::ExportPanel::State::Options) << "the export must start straight away";
}

// ---- Export execution drives the card ----

TEST_F(EditExportPageTest, ExportRunsUntilItsCompletionCallbackLands) {
    EditExportPage page;
    ASSERT_FALSE(page.isExportRunning());

    StartDoomedExport(page);
    EXPECT_TRUE(page.isExportRunning()) << "the running flag must be set before the worker is joined";

    // The completion callback is a queued invoke: pumping the loop lets it land.
    WaitMs(50);
    SettleLayout();
    EXPECT_FALSE(page.isExportRunning());

    auto* panel = ExportPanelOf(page);
    ASSERT_NE(panel, nullptr);
    EXPECT_EQ(panel->state(), ui::widgets::ExportPanel::State::Failed);
}

// ---- Dead/placeholder controls removed ----

TEST_F(EditExportPageTest, TimelineHasNoButtonRowAboveIt) {
    // Trim is direct manipulation on the timeline: the Trim / Add Marker button
    // row (and its duration readout) above the strip is gone.
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
    SettleLayout();

    page.setPreviewPlaying(true);
    ASSERT_TRUE(page.isPreviewPlaying());

    // Let the preview timer run past the 1-second duration.
    WaitMs(1200);

    EXPECT_FALSE(page.isPreviewPlaying());
    EXPECT_EQ(page.previewPositionMs(), 1000);
}

TEST_F(EditExportPageTest, ScrubWhilePausedStaysPaused) {
    EditExportPage page;
    page.resize(900, 700);
    page.show();
    page.setEditContext(MakeContext(100.0));
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

// ---- Post-flight report (header icon + tooltip) ----

TEST_F(EditExportPageTest, ReportUsesEmDashForMissingValuesNotUnavailable) {
    // With no diagnostics snapshot and no drift measurement, every empty value in
    // the post-recording report reads as the unified em dash — never a stray
    // "unavailable" string or an en dash.
    EditExportPage page;
    EditContext ctx;
    ctx.output_path = QStringLiteral("C:\\test\\recording.mkv");
    ctx.duration_seconds = 100.0;
    ctx.av_drift_available = false; // no drift data
    page.setEditContext(ctx);

    const QString em_dash = QString::fromUtf8("\xe2\x80\x94");
    const QString en_dash = QString::fromUtf8("\xe2\x80\x93");
    const QStringList lines = ReportTooltip(page).split(QLatin1Char('\n'));
    ASSERT_EQ(lines.size(), 3);

    for (const QString& line : lines) {
        EXPECT_TRUE(line.contains(em_dash)) << line.toStdString();
        EXPECT_FALSE(line.contains(en_dash)) << line.toStdString();
        EXPECT_FALSE(line.contains(QStringLiteral("unavailable"))) << line.toStdString();
    }
}

// Regression: a healthy 144 Hz display recorded at 60 fps CFR deliberately
// discards a large fraction of source frames via coalescing/pacing to hit the
// target rate -- none of that is a real drop. The report must count only
// encoder-backpressure drops, not the coalesced/CFR-pacing categories.
TEST_F(EditExportPageTest, ReportCountsOnlyRealDrops_NotCoalescedPacing) {
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

    // 3 / (6000 + 3) * 100 rounds to 0.0%; a coalesced-inclusive total (4103
    // dropped of 10103) would have shown roughly 40%.
    EXPECT_TRUE(ReportTooltip(page).contains(QStringLiteral("Frame drops: 0.0%"))) << ReportTooltip(page).toStdString();
}

// The mirror image of the test above: a frame whose GPU conversion failed is picture
// the recording lost, and it used to be filed under benign CFR pacing. The report
// then read 0.0% while the Diagnostics capture card flagged the same session.
TEST_F(EditExportPageTest, ReportCountsProcessingFailuresAsRealDrops) {
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

    // 100 / (900 + 100) = 10.0%. Before the fix this read 0.0%.
    EXPECT_TRUE(ReportTooltip(page).contains(QStringLiteral("Frame drops: 10.0%")))
        << ReportTooltip(page).toStdString();
}

TEST_F(EditExportPageTest, ReportTooltipCarriesAllThreeValues) {
    EditExportPage page;
    EditContext ctx;
    ctx.output_path = QStringLiteral("C:\\test\\recording.mkv");
    ctx.duration_seconds = 100.0;
    ctx.av_drift_available = true;
    ctx.peak_av_drift_ms = 12.0;
    ctx.completed_snapshot.valid = true;
    ctx.completed_snapshot.health = recorder_core::PipelineHealth::Good;
    page.setEditContext(ctx);

    const QString tooltip = ReportTooltip(page);
    EXPECT_TRUE(tooltip.contains(QStringLiteral("Frame drops:"))) << tooltip.toStdString();
    EXPECT_TRUE(tooltip.contains(QStringLiteral("Peak A/V drift:"))) << tooltip.toStdString();
    EXPECT_TRUE(tooltip.contains(QStringLiteral("12"))) << tooltip.toStdString();
    EXPECT_TRUE(tooltip.contains(QStringLiteral("Pipeline health: Good"))) << tooltip.toStdString();
}

// A hover-only tooltip would swallow a genuine finding, so the icon itself
// carries the severity: quiet for Good, coloured plus a word for the rest.
TEST_F(EditExportPageTest, ReportIconIsQuietForAHealthyPipeline) {
    EditExportPage page;
    EditContext ctx;
    ctx.output_path = QStringLiteral("C:\\test\\recording.mkv");
    ctx.completed_snapshot.valid = true;
    ctx.completed_snapshot.health = recorder_core::PipelineHealth::Good;
    page.setEditContext(ctx);

    auto* label = page.findChild<QLabel*>(QStringLiteral("editReportLabel"));
    ASSERT_NE(label, nullptr);
    EXPECT_TRUE(label->text().isEmpty());
    ASSERT_NE(page.findChild<QLabel*>(QStringLiteral("editReportIcon")), nullptr);
}

TEST_F(EditExportPageTest, ReportIconCallsOutAWarningPipeline) {
    EditExportPage page;
    EditContext ctx;
    ctx.output_path = QStringLiteral("C:\\test\\recording.mkv");
    ctx.completed_snapshot.valid = true;
    ctx.completed_snapshot.health = recorder_core::PipelineHealth::Warning;
    page.setEditContext(ctx);

    auto* label = page.findChild<QLabel*>(QStringLiteral("editReportLabel"));
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->text(), QStringLiteral("Warning"));
    EXPECT_TRUE(label->styleSheet().contains(QString::fromUtf8(ui::theme::ActiveTheme().caution), Qt::CaseInsensitive))
        << label->styleSheet().toStdString();
}

TEST_F(EditExportPageTest, ReportIconCallsOutACriticalPipeline) {
    EditExportPage page;
    EditContext ctx;
    ctx.output_path = QStringLiteral("C:\\test\\recording.mkv");
    ctx.completed_snapshot.valid = true;
    ctx.completed_snapshot.health = recorder_core::PipelineHealth::Critical;
    page.setEditContext(ctx);

    auto* label = page.findChild<QLabel*>(QStringLiteral("editReportLabel"));
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->text(), QStringLiteral("Critical"));
    EXPECT_TRUE(label->styleSheet().contains(QString::fromUtf8(ui::theme::ActiveTheme().error), Qt::CaseInsensitive))
        << label->styleSheet().toStdString();
}

// ---- Discarding edits ----

TEST_F(EditExportPageTest, FreshClipHasNothingToDiscard) {
    EditExportPage page;
    page.resize(900, 700);
    page.show();
    page.setEditContext(MakeContext(100.0));
    SettleLayout();
    EXPECT_FALSE(page.hasUnsavedEdits());
}

TEST_F(EditExportPageTest, TrimRangeCountsAsAnUnsavedEdit) {
    EditExportPage page;
    page.resize(900, 700);
    page.show();
    page.setEditContext(MakeContext(100.0));
    SettleLayout();

    page.setTrimRangeMs(10000, 90000);
    EXPECT_TRUE(page.hasUnsavedEdits());
}

TEST_F(EditExportPageTest, MarkersCountAsAnUnsavedEdit) {
    EditExportPage page;
    RecordingMarker m;
    m.time_ms = 25000;
    page.setEditContext(MakeContext(100.0, {m}));
    EXPECT_TRUE(page.hasUnsavedEdits());
}

TEST_F(EditExportPageTest, BackButton_WithEdits_KeepEditingCancelsTheClose) {
    EditExportPage page;
    page.resize(900, 700);
    page.show();
    page.setEditContext(MakeContext(100.0));
    SettleLayout();
    page.setTrimRangeMs(10000, 90000);
    ASSERT_TRUE(page.hasUnsavedEdits());

    int signal_count = 0;
    QObject::connect(&page, &EditExportPage::backRequested, &page, [&signal_count]() { ++signal_count; });

    bool asked = false;
    AnswerModalDialog(QStringLiteral("Keep editing"), &asked);
    auto* back_btn = page.findChild<QPushButton*>(QStringLiteral("editExportBackBtn"));
    ASSERT_NE(back_btn, nullptr);
    back_btn->click();

    EXPECT_TRUE(asked) << "closing with a trim set must ask first";
    EXPECT_EQ(signal_count, 0) << "keeping the edit must not close the surface";
}

TEST_F(EditExportPageTest, BackButton_WithEdits_DiscardClosesTheSurface) {
    EditExportPage page;
    page.resize(900, 700);
    page.show();
    page.setEditContext(MakeContext(100.0));
    SettleLayout();
    page.setTrimRangeMs(10000, 90000);
    ASSERT_TRUE(page.hasUnsavedEdits());

    int signal_count = 0;
    QObject::connect(&page, &EditExportPage::backRequested, &page, [&signal_count]() { ++signal_count; });

    bool asked = false;
    AnswerModalDialog(QStringLiteral("Discard"), &asked);
    auto* back_btn = page.findChild<QPushButton*>(QStringLiteral("editExportBackBtn"));
    ASSERT_NE(back_btn, nullptr);
    back_btn->click();

    EXPECT_TRUE(asked);
    EXPECT_EQ(signal_count, 1);
}

// ---- Row stack (video row + one row per audio track) ----

TEST_F(EditExportPageTest, TimelineHeightFollowsTheAudioTrackCount) {
    ui::widgets::EditTimeline timeline;
    timeline.resize(800, timeline.height());
    SettleLayout();

    const int video_only = timeline.height();
    EXPECT_GT(video_only, 0);

    timeline.setAudioTrackLabels({QStringLiteral("System")});
    SettleLayout();
    const int one_track = timeline.height();
    EXPECT_EQ(one_track, video_only + timeline.audioRowHeight() + 2 /* row gap */);

    timeline.setAudioTrackLabels({QStringLiteral("System"), QStringLiteral("Microphone")});
    SettleLayout();
    EXPECT_GT(timeline.height(), one_track);

    // A clip with no audio gets no audio rows at all.
    timeline.setAudioTrackLabels({});
    SettleLayout();
    EXPECT_EQ(timeline.height(), video_only);
}

TEST_F(EditExportPageTest, AudioRowsShareTheirHeightBudget) {
    // Three tracks must not push the player out at the 700 px minimum window
    // height, so the rows share a fixed budget instead of each claiming 20 px.
    ui::widgets::EditTimeline timeline;
    timeline.resize(800, timeline.height());

    timeline.setAudioTrackLabels({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")});
    const int three = timeline.height();

    timeline.setAudioTrackLabels({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D"),
                                  QStringLiteral("E"), QStringLiteral("F")});
    EXPECT_LT(timeline.audioRowHeight(), 20);
    // Six tracks are allowed to be taller than three, but not six times a row.
    EXPECT_LT(timeline.height(), three + 6 * 20);
}

TEST_F(EditExportPageTest, TimeMappingStillAgreesWithMultipleRows) {
    // xForMs()/msForX() map against the whole row stack; adding audio rows
    // changes its height but must not move a timestamp horizontally.
    ui::widgets::EditTimeline timeline;
    timeline.resize(800, timeline.height());
    timeline.setDurationMs(100000);
    SettleLayout();

    const int x_before = timeline.xForMs(50000);
    timeline.setAudioTrackLabels({QStringLiteral("System"), QStringLiteral("Microphone")});
    SettleLayout();
    EXPECT_EQ(timeline.xForMs(50000), x_before);

    for (const qint64 ms : {qint64{0}, qint64{25000}, qint64{50000}, qint64{100000}}) {
        const int x = timeline.xForMs(ms);
        // Round-trip within one pixel's worth of clip time.
        const qint64 per_px = 100000 / std::max(timeline.width(), 1);
        EXPECT_NEAR(static_cast<double>(timeline.msForX(x)), static_cast<double>(ms), static_cast<double>(per_px + 1));
    }
}

// ---- Thumbnail strip ----

TEST_F(EditExportPageTest, ThumbnailStripFillsTheVideoRowAndFollowsTheWidth) {
    ui::widgets::EditTimeline timeline;
    timeline.resize(800, timeline.height());
    timeline.show();
    SettleLayout();
    timeline.setDurationMs(100000);

    timeline.setThumbnailFixture(-1);
    const int wide = timeline.thumbnailCount();
    EXPECT_GT(wide, 0);

    // A narrower track holds fewer tiles: the strip is laid out on the width,
    // not on a fixed tile count.
    timeline.resize(400, timeline.height());
    SettleLayout();
    EXPECT_LT(timeline.thumbnailCount(), wide);
    EXPECT_GT(timeline.thumbnailCount(), 0);
}

TEST_F(EditExportPageTest, APartlyDecodedStripDrawsOnlyTheTilesItHas) {
    ui::widgets::EditTimeline timeline;
    timeline.resize(800, timeline.height());
    timeline.setDurationMs(100000);
    SettleLayout();

    timeline.setThumbnailFixture(-1);
    const int full = timeline.thumbnailCount();
    ASSERT_GT(full, 4);

    timeline.setThumbnailFixture(4);
    EXPECT_EQ(timeline.thumbnailCount(), 4);

    // Nothing decoded yet is an empty row, not a placeholder pattern.
    timeline.setThumbnailFixture(0);
    EXPECT_EQ(timeline.thumbnailCount(), 0);
}

// ---- Loading hint ----

// The quiet zone above the row stack, where the loading hint and the drag time
// label both paint. Grabbed as pixels rather than asserted by string/color, so
// the test does not depend on the exact wording or the theme's dim tone.
QImage GrabLabelZone(ui::widgets::EditTimeline& timeline) {
    return timeline.grab(QRect(0, 0, timeline.width(), 22)).toImage();
}

TEST_F(EditExportPageTest, LoadingHintAppearsOnlyWhileTilesAreStillPending) {
    ui::widgets::EditTimeline timeline;
    timeline.resize(800, timeline.height());
    timeline.show();
    SettleLayout();
    timeline.setDurationMs(100000);

    timeline.setThumbnailFixture(-1);
    const int full = timeline.thumbnailCount();
    ASSERT_GT(full, 1);

    // Short of capacity: the same test the real decode path uses
    // (thumbnails_.size() < the row's expected tile count).
    timeline.setThumbnailFixture(full - 1);
    const QImage pending = GrabLabelZone(timeline);

    // Filled back up: quiet again.
    timeline.setThumbnailFixture(-1);
    const QImage complete = GrabLabelZone(timeline);

    EXPECT_NE(pending, complete) << "the hint must paint something while tiles are still pending";
}

TEST_F(EditExportPageTest, LoadingHintNeverAppearsWithoutAFixtureOrClip) {
    ui::widgets::EditTimeline idle_timeline;
    idle_timeline.resize(800, idle_timeline.height());
    idle_timeline.show();
    SettleLayout();
    idle_timeline.setDurationMs(100000);
    // Never given a fixture or a clip: a recording whose clip never opened
    // must not show a hint for a decode that will never resolve.
    const QImage idle = GrabLabelZone(idle_timeline);

    ui::widgets::EditTimeline full_timeline;
    full_timeline.resize(800, full_timeline.height());
    full_timeline.show();
    SettleLayout();
    full_timeline.setDurationMs(100000);
    full_timeline.setThumbnailFixture(-1); // filled to capacity: also quiet
    const QImage full = GrabLabelZone(full_timeline);

    EXPECT_EQ(idle, full) << "no fixture/clip must read the same as a fully decoded strip, not as pending";
}

TEST_F(EditExportPageTest, LoadingHintIsSuppressedWhileDraggingAHandle) {
    ui::widgets::EditTimeline timeline;
    timeline.resize(610, timeline.height());
    timeline.show();
    SettleLayout();
    timeline.setDurationMs(100000);

    timeline.setThumbnailFixture(-1);
    const int full = timeline.thumbnailCount();
    ASSERT_GT(full, 1);
    timeline.setThumbnailFixture(full - 1); // pending -- the hint would show at rest
    const QImage pending = GrabLabelZone(timeline);

    const int track_y = 48;
    SendMouse(&timeline, QEvent::MouseButtonPress, QPoint(timeline.xForMs(0), track_y));
    SendMouse(&timeline, QEvent::MouseMove, QPoint(timeline.xForMs(20000), track_y));
    const QImage dragging = GrabLabelZone(timeline);
    SendMouse(&timeline, QEvent::MouseButtonRelease, QPoint(timeline.xForMs(20000), track_y));

    EXPECT_NE(dragging, pending) << "the drag time label must take over the zone, not share it with the hint";
}

TEST_F(EditExportPageTest, TimelineFixtureFlowsThroughThePage) {
    EditExportPage page;
    page.resize(900, 700);
    page.show();
    page.setEditContext(MakeContext(100.0));
    SettleLayout();

    auto* timeline = page.findChild<ui::widgets::EditTimeline*>(QStringLiteral("editTimeline"));
    ASSERT_NE(timeline, nullptr);
    const int video_only = timeline->height();

    page.setTimelineFixture({QStringLiteral("System"), QStringLiteral("Microphone")}, -1);
    SettleLayout();

    EXPECT_EQ(timeline->audioTrackLabels().size(), 2);
    EXPECT_GT(timeline->height(), video_only);
    EXPECT_GT(timeline->thumbnailCount(), 0);
}

TEST_F(EditExportPageTest, AClipWithoutAMasterPathLeavesTheStripEmpty) {
    // No master (the legacy toast path) means nothing to decode; the video row
    // stays blank rather than showing anything invented.
    EditExportPage page;
    page.resize(900, 700);
    page.show();
    page.setEditContext(MakeContext(100.0));
    SettleLayout();

    auto* timeline = page.findChild<ui::widgets::EditTimeline*>(QStringLiteral("editTimeline"));
    ASSERT_NE(timeline, nullptr);
    EXPECT_EQ(timeline->thumbnailCount(), 0);
    EXPECT_TRUE(timeline->audioTrackLabels().isEmpty());
}

// ---- Responsive layout ----

// The rail is the one column that must never be dropped: it carries the export
// panel. It narrows instead, so a tight window does not crush the player.
TEST_F(EditExportPageTest, RailNarrowsWithTheWindowButNeverDisappears) {
    EditExportPage page;
    page.show();

    page.resize(1400, 800);
    SettleLayout();
    auto* rail = page.findChild<QScrollArea*>(QStringLiteral("editExportRail"));
    ASSERT_NE(rail, nullptr);
    const int wide = rail->width();

    page.resize(1000, 800);
    SettleLayout();
    const int medium = rail->width();

    // 820 px is what a 860 px window (the enforced minimum) leaves the page
    // after the edit overlay's 20 px margin band on each side.
    page.resize(820, 660);
    SettleLayout();
    const int narrow = rail->width();

    EXPECT_GT(wide, medium);
    EXPECT_GT(medium, narrow);
    EXPECT_GT(narrow, 0);
    EXPECT_TRUE(rail->isVisibleTo(&page)) << "hiding the rail would hide the export controls with it";
}

// At the enforced minimum window the player must keep the larger share of the
// width, and still have real height left under the timeline.
TEST_F(EditExportPageTest, PlayerKeepsTheBulkOfTheWidthAtTheMinimumWindowSize) {
    EditExportPage page;
    page.show();
    page.resize(820, 660);
    SettleLayout();
    WaitMs(50);
    SettleLayout();

    auto* rail = page.findChild<QScrollArea*>(QStringLiteral("editExportRail"));
    ASSERT_NE(rail, nullptr);
    auto* player = page.findChild<QFrame*>(QStringLiteral("editExportPlayer"));
    ASSERT_NE(player, nullptr);

    EXPECT_GT(page.width() - rail->width(), rail->width() * 2);
    EXPECT_GE(player->height(), 180);
}

// A hidden overlay page gets no resizeEvent, so a window resized while the edit
// surface was dismissed would otherwise re-open at the stale rail width.
TEST_F(EditExportPageTest, ShowEventReappliesTheResponsiveLayout) {
    EditExportPage page;
    page.show();
    page.resize(1400, 800);
    SettleLayout();
    auto* rail = page.findChild<QScrollArea*>(QStringLiteral("editExportRail"));
    ASSERT_NE(rail, nullptr);
    const int wide = rail->width();

    page.hide();
    // Resizing while hidden delivers no resizeEvent to the page.
    page.resize(820, 660);
    EXPECT_EQ(rail->width(), wide) << "precondition: the stale width survives the hidden resize";

    page.show();
    SettleLayout();
    EXPECT_LT(rail->width(), wide) << "showEvent must re-run the responsive layout";
}

// The details card and the export panel together outgrow the column at the
// minimum window height, so the rail scrolls rather than clipping the result
// actions out of reach.
TEST_F(EditExportPageTest, RailScrollsInsteadOfClippingTheExportPanel) {
    EditExportPage page;
    page.show();
    page.resize(820, 660);
    SettleLayout();

    auto* rail = page.findChild<QScrollArea*>(QStringLiteral("editExportRail"));
    ASSERT_NE(rail, nullptr);
    auto* panel = ExportPanelOf(page);
    ASSERT_NE(panel, nullptr);

    panel->showDone(QStringLiteral("C:\\Videos\\clip_edit.mkv"));
    SettleLayout();

    ASSERT_NE(rail->widget(), nullptr);
    EXPECT_GE(rail->widget()->height(), panel->y() + panel->height())
        << "the scrolled column must be tall enough to hold the whole panel";
    auto* reveal = panel->findChild<QPushButton*>(QStringLiteral("exportRevealBtn"));
    ASSERT_NE(reveal, nullptr);
    EXPECT_GT(reveal->height(), 0);
}

// Scrolling is only an acceptable answer to the short column if the thing the
// user needs to see is scrolled to. A result reported below the fold that the
// user has to go looking for is not a report.
TEST_F(EditExportPageTest, AReportedResultIsScrolledIntoViewAtTheMinimumWindowSize) {
    EditExportPage page;
    page.show();
    page.resize(820, 660);
    SettleLayout();

    auto* rail = page.findChild<QScrollArea*>(QStringLiteral("editExportRail"));
    ASSERT_NE(rail, nullptr);
    auto* panel = ExportPanelOf(page);
    ASSERT_NE(panel, nullptr);

    StartDoomedExport(page);
    WaitMs(80);
    SettleLayout();
    WaitMs(20);
    SettleLayout();
    ASSERT_EQ(panel->state(), ui::widgets::ExportPanel::State::Failed);

    auto* retry = panel->findChild<QPushButton*>(QStringLiteral("exportRetryBtn"));
    ASSERT_NE(retry, nullptr);
    const QRect in_viewport(retry->mapTo(rail->viewport(), QPoint(0, 0)), retry->size());
    EXPECT_TRUE(rail->viewport()->rect().contains(in_viewport))
        << "Retry at " << in_viewport.top() << ".." << in_viewport.bottom() << " in a viewport of "
        << rail->viewport()->height() << " px";
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
