#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPointF>
#include <QRectF>

#include "ui/widgets/PreviewSurface.h"
#include "viewmodels/RecordViewModel.h"

namespace exosnap::ui::widgets {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "preview_surface_webcam_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

// A deterministic PreviewSurface with a fixed 16:9 content rect filling the widget,
// so normalized placement maps to predictable pixels.
class PreviewSurfaceWebcamTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    void SetUp() override {
        surface_ = std::make_unique<PreviewSurface>();
        surface_->resize(800, 450);
        // 16:9 source inside a 16:9 widget → content rect == full widget (no letterbox).
        QImage frame(1600, 900, QImage::Format_RGB32);
        frame.fill(QColor(20, 24, 30));
        surface_->setLiveFrame(frame);
        QImage cam(320, 180, QImage::Format_ARGB32);
        cam.fill(QColor(200, 120, 90));
        surface_->setWebcamFrame(cam);
    }

    void sendMouse(QEvent::Type type, QPointF local, Qt::MouseButton button, Qt::MouseButtons buttons) {
        const QPointF global = surface_->mapToGlobal(local.toPoint());
        QMouseEvent ev(type, local, global, button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(surface_.get(), &ev);
    }

    void sendKey(int key) {
        QKeyEvent ev(QEvent::KeyPress, key, Qt::NoModifier);
        QCoreApplication::sendEvent(surface_.get(), &ev);
    }

    QPointF pipCenter() const {
        const QRectF n = surface_->webcamOverlayRect();
        // content rect is (0,0,800,450)
        return QPointF((n.x() + n.width() / 2.0) * 800.0, (n.y() + n.height() / 2.0) * 450.0);
    }

    std::unique_ptr<PreviewSurface> surface_;
};

// 16. Clicking the PiP selects it.
TEST_F(PreviewSurfaceWebcamTest, ClickSelectsPip) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.40, 0.40, 0.25, 0.25));
    EXPECT_FALSE(surface_->isWebcamSelected());

    bool selection_signal = false;
    QObject::connect(surface_.get(), &PreviewSurface::webcamSelectionChanged, [&](bool s) { selection_signal = s; });

    sendMouse(QEvent::MouseButtonPress, pipCenter(), Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, pipCenter(), Qt::LeftButton, Qt::NoButton);

    EXPECT_TRUE(surface_->isWebcamSelected());
    EXPECT_TRUE(selection_signal);
}

// 17. Clicking outside the PiP deselects (placement kept).
TEST_F(PreviewSurfaceWebcamTest, ClickOutsideDeselects) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.40, 0.40, 0.25, 0.25));
    surface_->setWebcamSelected(true);
    ASSERT_TRUE(surface_->isWebcamSelected());
    const QRectF before = surface_->webcamOverlayRect();

    sendMouse(QEvent::MouseButtonPress, QPointF(10, 10), Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, QPointF(10, 10), Qt::LeftButton, Qt::NoButton);

    EXPECT_FALSE(surface_->isWebcamSelected());
    EXPECT_EQ(surface_->webcamOverlayRect(), before); // placement preserved
}

// 18 + 20. Dragging updates placement live; release confirms + emits.
TEST_F(PreviewSurfaceWebcamTest, DragUpdatesAndReleaseConfirms) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.40, 0.40, 0.25, 0.25));
    surface_->setWebcamSelected(true);

    int moved_count = 0;
    QRectF moved_rect;
    QObject::connect(surface_.get(), &PreviewSurface::webcamOverlayMoved, [&](QRectF r) {
        ++moved_count;
        moved_rect = r;
    });

    const QPointF start = pipCenter();
    sendMouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, start + QPointF(40, 24), Qt::NoButton, Qt::LeftButton);

    const QRectF during = surface_->webcamOverlayRect();
    EXPECT_GT(during.x(), 0.40); // moved right
    EXPECT_GT(during.y(), 0.40); // moved down

    sendMouse(QEvent::MouseButtonRelease, start + QPointF(40, 24), Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(moved_count, 1);
    EXPECT_EQ(moved_rect, surface_->webcamOverlayRect());
}

// 19. Escape during a drag restores the pre-interaction geometry.
TEST_F(PreviewSurfaceWebcamTest, EscapeRestoresPriorPlacement) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.40, 0.40, 0.25, 0.25));
    surface_->setWebcamSelected(true);
    const QRectF before = surface_->webcamOverlayRect();

    const QPointF start = pipCenter();
    sendMouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, start + QPointF(60, 40), Qt::NoButton, Qt::LeftButton);
    ASSERT_NE(surface_->webcamOverlayRect(), before); // moved

    sendKey(Qt::Key_Escape);
    EXPECT_EQ(surface_->webcamOverlayRect(), before); // rolled back
}

// 21 + 26. Disabling the webcam clears edit state (selection + active drag).
TEST_F(PreviewSurfaceWebcamTest, DisablingClearsEditState) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.40, 0.40, 0.25, 0.25));
    surface_->setWebcamSelected(true);
    ASSERT_TRUE(surface_->isWebcamSelected());

    surface_->setWebcamOverlayEnabled(false);
    EXPECT_FALSE(surface_->isWebcamSelected());
    EXPECT_EQ(surface_->webcamActiveHandle(), QStringLiteral("none"));
}

// 22 + 23. Locking editing prevents selection (programmatic and via click).
TEST_F(PreviewSurfaceWebcamTest, LockPreventsSelection) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.40, 0.40, 0.25, 0.25));
    surface_->setWebcamEditLocked(true);

    surface_->setWebcamSelected(true);
    EXPECT_FALSE(surface_->isWebcamSelected());

    sendMouse(QEvent::MouseButtonPress, pipCenter(), Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, pipCenter(), Qt::LeftButton, Qt::NoButton);
    EXPECT_FALSE(surface_->isWebcamSelected());
}

// 24. Locking while selected drops the selection.
TEST_F(PreviewSurfaceWebcamTest, LockDeselectsActiveSelection) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamSelected(true);
    ASSERT_TRUE(surface_->isWebcamSelected());

    surface_->setWebcamEditLocked(true);
    EXPECT_FALSE(surface_->isWebcamSelected());
}

TEST_F(PreviewSurfaceWebcamTest, UnlockRestoresSelectionForLiveEditableStates) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.40, 0.40, 0.25, 0.25));
    surface_->setWebcamEditLocked(true);
    surface_->setWebcamEditLocked(false);

    sendMouse(QEvent::MouseButtonPress, pipCenter(), Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, pipCenter(), Qt::LeftButton, Qt::NoButton);
    EXPECT_TRUE(surface_->isWebcamSelected());
}

TEST(PreviewSurfaceWebcamPolicyTest, RecordingAndPausedStatesAreEditable) {
    EXPECT_TRUE(exosnap::IsWebcamOverlayEditable(exosnap::UiRecordingState::Ready));
    EXPECT_TRUE(exosnap::IsWebcamOverlayEditable(exosnap::UiRecordingState::Countdown));
    EXPECT_TRUE(exosnap::IsWebcamOverlayEditable(exosnap::UiRecordingState::Recording));
    EXPECT_TRUE(exosnap::IsWebcamOverlayEditable(exosnap::UiRecordingState::Paused));
    EXPECT_FALSE(exosnap::IsWebcamOverlayEditable(exosnap::UiRecordingState::Stopping));
    EXPECT_FALSE(exosnap::IsWebcamOverlayEditable(exosnap::UiRecordingState::Completed));
    EXPECT_FALSE(exosnap::IsWebcamOverlayEditable(exosnap::UiRecordingState::Failed));
}

// 25. Cancelling interaction releases the active drag/pointer capture.
TEST_F(PreviewSurfaceWebcamTest, CancelInteractionReleasesDrag) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.40, 0.40, 0.25, 0.25));
    surface_->setWebcamSelected(true);
    sendMouse(QEvent::MouseButtonPress, pipCenter(), Qt::LeftButton, Qt::LeftButton);
    ASSERT_NE(surface_->webcamActiveHandle(), QStringLiteral("none"));

    surface_->cancelWebcamInteraction();
    EXPECT_EQ(surface_->webcamActiveHandle(), QStringLiteral("none"));
}

// 29 + 32. Mirror is a pure transform: it flips the flag without changing placement.
TEST_F(PreviewSurfaceWebcamTest, MirrorDoesNotChangePlacement) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.40, 0.40, 0.25, 0.25));
    const QRectF before = surface_->webcamOverlayRect();

    EXPECT_FALSE(surface_->webcamMirror());
    surface_->setWebcamMirror(true);
    EXPECT_TRUE(surface_->webcamMirror());
    EXPECT_EQ(surface_->webcamOverlayRect(), before);
}

// Default placement sits in the bottom-right with a safe margin.
TEST_F(PreviewSurfaceWebcamTest, DefaultPlacementBottomRight) {
    const QRectF def = surface_->defaultWebcamOverlayRect(16.0 / 9.0);
    EXPECT_GT(def.x(), 0.5);
    EXPECT_GT(def.y(), 0.5);
    EXPECT_LE(def.x() + def.width(), 1.0 + 1e-4);
    EXPECT_LE(def.y() + def.height(), 1.0 + 1e-4);
}

// Out-of-bounds placement is sanitized to a valid in-frame rect.
TEST_F(PreviewSurfaceWebcamTest, OverlayRectIsSanitized) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(2.0, 2.0, 5.0, 5.0));
    const QRectF n = surface_->webcamOverlayRect();
    EXPECT_GE(n.x(), 0.0);
    EXPECT_GE(n.y(), 0.0);
    EXPECT_LE(n.x() + n.width(), 1.0 + 1e-4);
    EXPECT_LE(n.y() + n.height(), 1.0 + 1e-4);
}

// The mapped preview rect never escapes the content frame (800x450 here).
TEST_F(PreviewSurfaceWebcamTest, MappedPreviewRectInsideContent) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.85, 0.85, 0.25, 0.25));
    const QRect r = surface_->webcamMappedPreviewRect();
    EXPECT_GE(r.left(), 0);
    EXPECT_GE(r.top(), 0);
    EXPECT_LE(r.right(), 800);
    EXPECT_LE(r.bottom(), 450);
}

// ---- Overlay text elision (VR-009) ----------------------------------------

TEST_F(PreviewSurfaceWebcamTest, OverlayTextFitsAtComfortableWidth) {
    auto* right = surface_->findChild<QLabel*>(QStringLiteral("previewBottomRightLabel"));
    ASSERT_NE(right, nullptr);

    const QString text = QStringLiteral("Native · 60 fps CFR · AV1 · Opus · WebM");
    surface_->setBottomRightText(text);
    EXPECT_TRUE(right->isVisibleTo(surface_.get()));
    EXPECT_EQ(right->text(), text); // no elision needed at 800 px
}

TEST_F(PreviewSurfaceWebcamTest, OverlayTextElidesInsteadOfClipping) {
    auto* right = surface_->findChild<QLabel*>(QStringLiteral("previewBottomRightLabel"));
    auto* left = surface_->findChild<QLabel*>(QStringLiteral("previewBottomLeftLabel"));
    ASSERT_NE(right, nullptr);
    ASSERT_NE(left, nullptr);

    surface_->resize(360, 203);
    // Long enough to require elision at 360 px regardless of the test font.
    const QString long_right =
        QStringLiteral("Native · 60 fps CFR · AV1 · Opus · WebM · SIZE 51.0 MB · BITRATE 5.2 Mb/s · DRIFT 2 ms");
    surface_->setBottomLeftText(QStringLiteral("FRAME 16.67 ms · BITRATE 5.2 Mb/s · DROP 0 · DRIFT 2 ms"));
    surface_->setBottomRightText(long_right);

    EXPECT_TRUE(right->isVisibleTo(surface_.get()));
    EXPECT_TRUE(right->text().endsWith(QChar(0x2026))); // "…"
    const int row_width = 360 - 32;
    EXPECT_LE(right->fontMetrics().horizontalAdvance(right->text()), row_width);
    // The left text yields to the right text; combined they never exceed the row.
    const int combined =
        left->fontMetrics().horizontalAdvance(left->text()) + right->fontMetrics().horizontalAdvance(right->text());
    EXPECT_LE(combined, row_width);
}

TEST_F(PreviewSurfaceWebcamTest, OverlayRowsHideBelowMinimumWidth) {
    auto* right = surface_->findChild<QLabel*>(QStringLiteral("previewBottomRightLabel"));
    auto* meta = surface_->findChild<QLabel*>(QStringLiteral("previewTopMetaLabel"));
    ASSERT_NE(right, nullptr);
    ASSERT_NE(meta, nullptr);

    surface_->resize(180, 120);
    surface_->setBottomRightText(QStringLiteral("Native · 60 fps CFR · AV1 · Opus · WebM"));
    surface_->setTopMetaText(QStringLiteral("VISUAL TEST TARGET"));

    EXPECT_FALSE(right->isVisibleTo(surface_.get()));
    EXPECT_FALSE(meta->isVisibleTo(surface_.get()));

    // Growing back restores the texts (no stale hidden state).
    surface_->resize(800, 450);
    surface_->setBottomRightText(QStringLiteral("Native · 60 fps CFR · AV1 · Opus · WebM"));
    EXPECT_TRUE(right->isVisibleTo(surface_.get()));
}

} // namespace
} // namespace exosnap::ui::widgets
