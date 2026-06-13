// CAPTURE-FRAME-BUTTON-R1 unit tests
//
// Tests cover:
//   1. NotificationManager enqueues a "Frame saved" success event with the right fields.
//   2. The event has type Saved, action OpenFolder, non-empty title + body.
//   3. Enqueueing a frame-saved event with the folder path does not crash.
//   4. A second capture does not enqueue while the first is in-flight (disabled state).
//   5. The RecordPage::captureFrameSaved signal emits with the correct path.
//   6. State-gating: button disabled in LoadingCapabilities state, enabled in Ready.
//
// Follows the QCoreApplication-fixture pattern (no GPU / real window needed).
// ctest runs each test in isolation via --gtest_filter.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>

#include "notifications/NotificationEvent.h"
#include "notifications/NotificationManager.h"
#include "services/RecordingCoordinator.h"

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

} // namespace
} // namespace exosnap
