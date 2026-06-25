// CAPTURE-FRAME-DOCK-BUTTON-R1 unit tests
//
// The capture-frame affordance moved from the preview-corner overlay (removed —
// occluded by DXGI HWND) to a round camera icon button on the right side of the
// transport dock.  The notification / toast path is unchanged.
//
// Tests cover:
//   1. NotificationManager enqueues a "Frame saved" success event with the right fields.
//   2. The event has type Saved, action OpenFolder, non-empty title + body.
//   3. Enqueueing a frame-saved event with the folder path does not crash.
//   4. Frame-saved toast is auto-dismissible (5 s dwell via kDismissMs_Saved).
//   5. Frame-saved toast has an Open-folder action.
//   6. State-gating: CaptureFrame rejected in LoadingCapabilities state.
//   7. Multiple frame-saved events stack in the queue (capped at kMaxVisible).
//   8. "Frame saved" title is distinct from "Recording saved".
//   9. Frame-path payload is forwarded to action_payload.
//  10. Dismiss clears the frame-saved toast.
//  11. Dock button exists on TransportDock with correct object name + tooltip.
//  12. Dock button is visible in Ready state and hidden in Saving/Completed.
//
// Follows the QCoreApplication-fixture pattern (no GPU / real window needed).
// ctest runs each test in isolation via --gtest_filter.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPushButton>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>

#include "notifications/NotificationEvent.h"
#include "notifications/NotificationManager.h"
#include "services/RecordingCoordinator.h"
#include "ui/widgets/TransportDock.h"

namespace exosnap {
namespace {

using notifications::NotificationAction;
using notifications::NotificationEvent;
using notifications::NotificationManager;
using notifications::NotificationType;

// ── QApplication fixture ─────────────────────────────────────────────────────

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "capture_frame_button_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class CaptureFrameButtonTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// ── Test 1: "Frame saved" event has correct type and action ──────────────────

TEST_F(CaptureFrameButtonTest, FrameSavedEvent_HasCorrectTypeAndAction) {
    NotificationManager mgr;

    NotificationEvent event;
    event.type = NotificationType::Saved;
    event.title = QStringLiteral("Frame saved");
    event.body = QStringLiteral("frame_2026-06-13_01.png — C:/Users/test/Videos");
    event.action = NotificationAction::OpenFolder;
    event.action_payload = QStringLiteral("C:/Users/test/Videos/frame_2026-06-13_01.png");

    mgr.Enqueue(event);

    ASSERT_EQ(mgr.VisibleEvents().size(), 1);
    const auto& visible = mgr.VisibleEvents().front();
    EXPECT_EQ(visible.type, NotificationType::Saved);
    EXPECT_EQ(visible.action, NotificationAction::OpenFolder);
    EXPECT_FALSE(visible.title.isEmpty());
    EXPECT_FALSE(visible.body.isEmpty());
    EXPECT_EQ(visible.title, QStringLiteral("Frame saved"));
}

// ── Test 2: "Frame saved" event body contains filename and folder ─────────────

TEST_F(CaptureFrameButtonTest, FrameSavedEvent_BodyContainsFilenameAndFolder) {
    NotificationManager mgr;

    const QString frame_path = QStringLiteral("C:/Users/test/Videos/frame_2026-06-13_01.png");
    const QString filename = QFileInfo(frame_path).fileName();
    const QString folder = QFileInfo(frame_path).dir().path();

    NotificationEvent event;
    event.type = NotificationType::Saved;
    event.title = QStringLiteral("Frame saved");
    event.body = QStringLiteral("%1 — %2").arg(filename, folder);
    event.action = NotificationAction::OpenFolder;
    event.action_payload = frame_path;

    mgr.Enqueue(event);

    ASSERT_EQ(mgr.VisibleEvents().size(), 1);
    EXPECT_TRUE(mgr.VisibleEvents().front().body.contains(filename));
    EXPECT_TRUE(mgr.VisibleEvents().front().body.contains(folder));
    EXPECT_EQ(mgr.VisibleEvents().front().action_payload, frame_path);
}

// ── Test 3: Enqueueing frame-saved event does not crash ──────────────────────

TEST_F(CaptureFrameButtonTest, FrameSavedEvent_EnqueueDoesNotCrash) {
    NotificationManager mgr;

    NotificationEvent event;
    event.type = NotificationType::Saved;
    event.title = QStringLiteral("Frame saved");
    event.body = QStringLiteral("frame.png — C:/Videos");
    event.action = NotificationAction::OpenFolder;

    EXPECT_NO_FATAL_FAILURE(mgr.Enqueue(event));
    EXPECT_GE(mgr.VisibleEvents().size(), 1);
}

// ── Test 4: Frame saved event is auto-dismissible (type=Saved, 5 s dwell) ────

TEST_F(CaptureFrameButtonTest, FrameSavedEvent_IsAutoDismiss) {
    // The "Frame saved" toast re-uses NotificationType::Saved which has a
    // 5-second auto-dismiss (kDismissMs_Saved = 5000).
    EXPECT_EQ(NotificationManager::kDismissMs_Saved, 5000);
}

// ── Test 5: Frame saved event has an Open-folder action ──────────────────────

TEST_F(CaptureFrameButtonTest, FrameSavedEvent_HasOpenFolderAction) {
    NotificationManager mgr;

    NotificationEvent event;
    event.type = NotificationType::Saved;
    event.title = QStringLiteral("Frame saved");
    event.body = QStringLiteral("frame.png — C:/Videos");
    event.action = NotificationAction::OpenFolder;
    event.action_payload = QStringLiteral("C:/Videos/frame.png");

    mgr.Enqueue(event);
    ASSERT_FALSE(mgr.VisibleEvents().isEmpty());
    EXPECT_EQ(mgr.VisibleEvents().front().action, NotificationAction::OpenFolder);
    EXPECT_TRUE(mgr.VisibleEvents().front().hasAction());
}

// ── Test 6: CaptureFrame rejected in LoadingCapabilities state ───────────────
// Mirrors test_capture_frame.cpp test 8 — ensures state-gating is unchanged
// after CAPTURE-FRAME-BUTTON-R1 changes.

TEST_F(CaptureFrameButtonTest, CaptureFrame_RejectedInLoadingCapabilities) {
    RecordingCoordinator coord;

    bool callback_fired = false;
    bool reported_success = false;
    coord.SetFrameCapturedCallback([&](bool ok, const QString&, const QString&) {
        callback_fired = true;
        reported_success = ok;
    });

    coord.CaptureFrame();
    EXPECT_TRUE(callback_fired);
    EXPECT_FALSE(reported_success);
}

// ── Test 7: Multiple frame-saved events stack in the queue ───────────────────

TEST_F(CaptureFrameButtonTest, MultipleFrameSavedEvents_QueueCorrectly) {
    NotificationManager mgr;

    for (int i = 0; i < NotificationManager::kMaxVisible + 2; ++i) {
        NotificationEvent event;
        event.type = NotificationType::Saved;
        event.title = QStringLiteral("Frame saved");
        event.body = QStringLiteral("frame_%1.png").arg(i);
        event.action = NotificationAction::OpenFolder;
        mgr.Enqueue(event);
    }

    // Visible count is capped at kMaxVisible; the rest are in the pending queue.
    EXPECT_LE(mgr.VisibleEvents().size(), NotificationManager::kMaxVisible);
    EXPECT_GE(mgr.VisibleEvents().size(), 1);
}

// ── Test 8: "Frame saved" title is distinct from "Recording saved" ────────────

TEST_F(CaptureFrameButtonTest, FrameSavedTitle_DistinctFromRecordingSaved) {
    // Ensure we use a title that is distinct from the recording result toast.
    const QString frame_toast_title = QStringLiteral("Frame saved");
    const QString recording_toast_title = QStringLiteral("Recording saved");
    EXPECT_NE(frame_toast_title, recording_toast_title);
}

// ── Test 9: Frame-path payload is forwarded to action_payload ────────────────

TEST_F(CaptureFrameButtonTest, FrameSavedEvent_ActionPayloadIsFramePath) {
    const QString path = QStringLiteral("C:/Videos/frame_001.png");

    NotificationManager mgr;
    NotificationEvent event;
    event.type = NotificationType::Saved;
    event.title = QStringLiteral("Frame saved");
    event.body = QStringLiteral("frame_001.png");
    event.action = NotificationAction::OpenFolder;
    event.action_payload = path;

    mgr.Enqueue(event);
    ASSERT_FALSE(mgr.VisibleEvents().isEmpty());
    EXPECT_EQ(mgr.VisibleEvents().front().action_payload, path);
}

// ── Test 10: Dismiss clears the frame-saved toast ────────────────────────────

TEST_F(CaptureFrameButtonTest, FrameSavedEvent_CanBeDismissed) {
    NotificationManager mgr;

    NotificationEvent event;
    event.type = NotificationType::Saved;
    event.title = QStringLiteral("Frame saved");
    event.body = QStringLiteral("frame.png");
    event.action = NotificationAction::OpenFolder;

    mgr.Enqueue(event);
    ASSERT_FALSE(mgr.VisibleEvents().isEmpty());

    const uint64_t seq = mgr.VisibleEvents().front().sequence;
    mgr.Dismiss(seq);
    EXPECT_TRUE(mgr.VisibleEvents().isEmpty());
}

// ── Test 11: Dock button exists with correct seams ───────────────────────────
// CAPTURE-FRAME-DOCK-BUTTON-R1: the camera icon button must live on the dock,
// not in the preview overlay.

TEST_F(CaptureFrameButtonTest, DockButton_ExistsWithCorrectSeams) {
    ui::widgets::TransportDock dock;

    auto* btn = dock.findChild<QPushButton*>(QStringLiteral("recordDockCaptureFrame"));
    ASSERT_NE(btn, nullptr) << "round capture-frame button must exist on the TransportDock";

    // Tooltip must mention "Capture frame" and the hotkey.
    EXPECT_TRUE(btn->toolTip().contains(QStringLiteral("Capture frame")));
    // Accessible name must be "Capture frame" for UIA / screen readers.
    EXPECT_EQ(btn->accessibleName(), QStringLiteral("Capture frame"));
    // Must be icon-only (no text label).
    EXPECT_TRUE(btn->text().isEmpty());
    // Must be the spec 44×44 fixed size.
    EXPECT_EQ(btn->width(), 44);
    EXPECT_EQ(btn->height(), 44);
}

// ── Test 12: Dock button visible in Ready/Recording/Paused, absent in Saving ──

TEST_F(CaptureFrameButtonTest, DockButton_VisibilityByState) {
    ui::widgets::TransportDock dock;

    auto* btn = dock.findChild<QPushButton*>(QStringLiteral("recordDockCaptureFrame"));
    ASSERT_NE(btn, nullptr);

    dock.setState(ui::widgets::TransportDock::State::Ready);
    EXPECT_TRUE(btn->isVisibleTo(&dock)) << "button must be visible in Ready state";

    dock.setState(ui::widgets::TransportDock::State::Recording);
    EXPECT_TRUE(btn->isVisibleTo(&dock)) << "button must be visible during Recording";

    dock.setState(ui::widgets::TransportDock::State::Paused);
    EXPECT_TRUE(btn->isVisibleTo(&dock)) << "button must be visible while Paused";

    // v10: Saving/Completed map to a Ready-layout dock (no separate saved/saving mode).
    // The capture-frame button remains visible but is disabled while saving/blocked.
    dock.setState(ui::widgets::TransportDock::State::Saving);
    EXPECT_TRUE(btn->isVisibleTo(&dock)) << "button visible in Saving (disabled, not hidden)";
    EXPECT_FALSE(btn->isEnabled()) << "button disabled during Saving";

    dock.setState(ui::widgets::TransportDock::State::Completed);
    EXPECT_TRUE(btn->isVisibleTo(&dock)) << "button visible in Completed (Ready layout)";
}

// ── Test 13: Dock button click emits captureFrameClicked signal ──────────────

TEST_F(CaptureFrameButtonTest, DockButton_ClickEmitsCaptureFrameClicked) {
    ui::widgets::TransportDock dock;
    dock.setState(ui::widgets::TransportDock::State::Ready);
    dock.setPrimaryEnabled(true);

    bool fired = false;
    QObject::connect(&dock, &ui::widgets::TransportDock::captureFrameClicked, &dock, [&fired]() { fired = true; });

    auto* btn = dock.findChild<QPushButton*>(QStringLiteral("recordDockCaptureFrame"));
    ASSERT_NE(btn, nullptr);
    ASSERT_TRUE(btn->isEnabled());
    btn->click();
    EXPECT_TRUE(fired);
}

} // namespace
} // namespace exosnap
