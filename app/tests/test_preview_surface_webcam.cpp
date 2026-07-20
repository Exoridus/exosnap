#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>

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

// 33. Opacity blends the PiP toward the underlying frame; edit chrome is unaffected.
TEST_F(PreviewSurfaceWebcamTest, OpacityBlendsPipTowardBackground) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.40, 0.40, 0.25, 0.25));
    surface_->setWebcamSelected(false);

    surface_->setWebcamOpacity(1.0f);
    const QImage full = surface_->grab().toImage();
    const QPoint center = pipCenter().toPoint();
    const QColor full_color = full.pixelColor(center);

    surface_->setWebcamOpacity(0.5f);
    const QImage half = surface_->grab().toImage();
    const QColor half_color = half.pixelColor(center);

    // Background colour at the PiP centre before compositing the webcam frame: the
    // live frame fills the full 16:9 content rect in this fixture (see SetUp()).
    const QColor bg_color(20, 24, 30);

    const int full_channels[3] = {full_color.red(), full_color.green(), full_color.blue()};
    const int half_channels[3] = {half_color.red(), half_color.green(), half_color.blue()};
    const int bg_channels[3] = {bg_color.red(), bg_color.green(), bg_color.blue()};
    for (int ch = 0; ch < 3; ++ch) {
        const int expected_mid = (full_channels[ch] + bg_channels[ch]) / 2;
        EXPECT_LE(std::abs(half_channels[ch] - expected_mid), 25) << "channel " << ch;
    }
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

// SNAPSHOT-GATE: the Ready-state screenshot readback requires a live DXGI preview
// that has drawn a real frame. A surface with no DXGI preview can never satisfy that
// precondition, so it must report not-ready (the TransportDock gate stays disabled).
TEST(PreviewSurfaceSnapshotReady, NotReadyWithoutDxgiPreview) {
    EnsureApplication();
    PreviewSurface surface;
    EXPECT_FALSE(surface.isDxgiSnapshotReady());
}

// ---- Chroma key state propagation (systematic-debugging: Bug A/B) ----------
//
// Investigation trail for "chroma key has no visible effect" / "webcam
// disappears when chroma key is enabled": traced the full path — WebcamSetupPanel
// toggle -> WebcamSettings.chroma_key -> RecordPage::setWebcamSettings ->
// PreviewSurface::setWebcamChromaKey -> syncWebcamOverlayToDxgi ->
// DxgiPreviewRenderer::SetWebcamOverlayState -> RenderWebcamOverlay's
// MakeOverlayPixelConstants(overlayChroma_, ..., force_opaque=true, ...) call ->
// the shared overlay_shader.h shader. Every hop forwards the struct unchanged
// (verified by reading + the WebcamSetupPanel emission tests in
// test_webcam_setup_panel.cpp), SelectOverlayMode() lets an enabled key win over
// force_opaque (OverlayShaderConstants.ModeFollowsChromaThenForceOpaque), and the
// shader itself is proven correct on a WARP device with these exact parameters
// (OverlayShaderWarp.ChromaKeyDropsKeyColorAndKeepsTheRest /
// KeyedSpriteHonoursOpacityOnTheKeptPixels in test_overlay_shader.cpp). No defect
// found in this chain. This test closes the one real gap: PreviewSurface itself
// had no coverage locking down that it forwards chroma settings unchanged.
TEST_F(PreviewSurfaceWebcamTest, ChromaKeySettingsRoundTripUnchanged) {
    exosnap::WebcamChromaKeySettings chroma;
    chroma.enabled = true;
    chroma.color_mode = exosnap::WebcamChromaKeyColorMode::Custom;
    chroma.custom_r = 10;
    chroma.custom_g = 200;
    chroma.custom_b = 30;
    chroma.tolerance = 0.33f;
    chroma.softness = 0.22f;
    chroma.spill_reduction = 0.11f;

    surface_->setWebcamChromaKey(chroma);
    EXPECT_TRUE(surface_->webcamChromaKey() == chroma);

    surface_->setWebcamChromaKey(exosnap::WebcamChromaKeySettings{});
    EXPECT_FALSE(surface_->webcamChromaKey().enabled);
}

// ---- Page-visibility hide/show (Bug C: flicker navigating away from Record) --
//
// Root cause: the DXGI preview's native child HWND (DxgiPreviewRenderer's
// WS_CHILD window) is a manually created Win32 window, not a QWidget — nothing
// previously hid it explicitly when PreviewSurface (and its ancestor Record page)
// was hidden by the top-nav QStackedWidget switch. It relied entirely on Qt's
// native-window hide cascade reaching it in the same paint cycle as the next
// page's first paint, which is not guaranteed in this app's frameless/
// custom-chrome window. Fix: PreviewSurface::hideEvent/showEvent now toggle the
// child window's OS-level visibility explicitly and synchronously via the new
// DxgiPreviewRenderer::SetChildWindowVisible (a plain ShowWindow call — it never
// touches capture/render-thread lifecycle, so it cannot block or introduce jank).
// No live DXGI renderer exists in this widget-test fixture (real DXGI capture
// needs actual hardware), so this locks down the safe no-op path and confirms no
// webcam/overlay state is disturbed by a hide/show cycle.
TEST_F(PreviewSurfaceWebcamTest, HideShowCycleIsSafeAndPreservesState) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.40, 0.40, 0.25, 0.25));
    surface_->setWebcamMirror(true);
    surface_->setWebcamOpacity(0.75f);
    exosnap::WebcamChromaKeySettings chroma;
    chroma.enabled = true;
    chroma.tolerance = 0.5f;
    surface_->setWebcamChromaKey(chroma);

    surface_->hide();
    surface_->show();

    EXPECT_TRUE(surface_->isWebcamOverlayEnabled());
    EXPECT_EQ(surface_->webcamOverlayRect(), QRectF(0.40, 0.40, 0.25, 0.25));
    EXPECT_TRUE(surface_->webcamMirror());
    EXPECT_FLOAT_EQ(surface_->webcamOpacity(), 0.75f);
    EXPECT_TRUE(surface_->webcamChromaKey().enabled);
    EXPECT_FLOAT_EQ(surface_->webcamChromaKey().tolerance, 0.5f);
}

// ---- True WYSIWYG PiP (no preview-only trims) ------------------------------

// Regression: the preview used to reserve the bottom stats strip and clip the PiP
// out of it, so a bottom-edge placement rendered trimmed while the file got the
// full camera. The PiP must now paint all the way to the video edge.
TEST_F(PreviewSurfaceWebcamTest, PipDrawsFlushToBottomEdgeUnclipped) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOpacity(1.0f);
    // Flush bottom-right quarter: (0.75,0.75)-(1.0,1.0) → pixels 600..800 x 337..450.
    surface_->setWebcamOverlayRect(QRectF(0.75, 0.75, 0.25, 0.25));

    const QImage img = surface_->grab().toImage();
    // Below the old footer clip (~y=410) and below the stats-row labels (rows end
    // at y=438): this pixel used to show the background frame, never the camera.
    const QColor cam(200, 120, 90); // fixture camera fill (SetUp)
    const QColor got = img.pixelColor(QPoint(700, 444));
    EXPECT_LE(std::abs(got.red() - cam.red()), 8);
    EXPECT_LE(std::abs(got.green() - cam.green()), 8);
    EXPECT_LE(std::abs(got.blue() - cam.blue()), 8);
}

// ---- Hover cursor refresh (stationary-pointer bug) -------------------------

// Regression: the resize/move cursor was recomputed ONLY on mouse moves, so any
// hit-map change under a stationary pointer (selection toggles the corner-handle
// zones, disabling removes them) kept a stale cursor until the pointer left and
// re-entered the surface.
TEST_F(PreviewSurfaceWebcamTest, HoverCursorRefreshesOnSelectionChange) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.40, 0.40, 0.25, 0.25));

    // Hover the bottom-right corner: unselected → plain move (no handles yet).
    const QPointF corner(519.0, 291.0); // rect spans 320..520 x 180..292.5
    sendMouse(QEvent::MouseMove, corner, Qt::NoButton, Qt::NoButton);
    EXPECT_EQ(surface_->cursor().shape(), Qt::SizeAllCursor);

    // Selecting adds the corner handles — the cursor must flip WITHOUT any
    // further mouse movement.
    surface_->setWebcamSelected(true);
    EXPECT_EQ(surface_->cursor().shape(), Qt::SizeFDiagCursor);

    // Deselecting removes them again — still without a mouse move.
    surface_->setWebcamSelected(false);
    EXPECT_EQ(surface_->cursor().shape(), Qt::SizeAllCursor);
}

TEST_F(PreviewSurfaceWebcamTest, HoverCursorClearsWhenEditingDisabled) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.40, 0.40, 0.25, 0.25));
    sendMouse(QEvent::MouseMove, pipCenter(), Qt::NoButton, Qt::NoButton);
    ASSERT_EQ(surface_->cursor().shape(), Qt::SizeAllCursor);

    surface_->setWebcamOverlayEnabled(false);
    EXPECT_EQ(surface_->cursor().shape(), Qt::ArrowCursor);
}

TEST_F(PreviewSurfaceWebcamTest, HoverCursorRefreshesAfterDragEnds) {
    surface_->setWebcamOverlayEnabled(true);
    surface_->setWebcamOverlayRect(QRectF(0.40, 0.40, 0.25, 0.25));
    surface_->setWebcamSelected(true);

    // Grab the bottom-right handle and drag so the pointer ends INSIDE the PiP
    // body (the rect grows past the pointer's release position).
    const QPointF corner(519.0, 291.0);
    sendMouse(QEvent::MouseMove, corner, Qt::NoButton, Qt::NoButton);
    ASSERT_EQ(surface_->cursor().shape(), Qt::SizeFDiagCursor);
    sendMouse(QEvent::MouseButtonPress, corner, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, corner + QPointF(120, 70), Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, corner + QPointF(120, 70), Qt::LeftButton, Qt::NoButton);

    // The release position sits on the grown rect's bottom-right corner handle;
    // the cursor must reflect the post-drag hit map immediately.
    const QRectF n = surface_->webcamOverlayRect();
    const QPointF br((n.x() + n.width()) * 800.0, (n.y() + n.height()) * 450.0);
    const QPointF released = corner + QPointF(120, 70);
    const bool on_handle = std::abs(released.x() - br.x()) < 10.0 && std::abs(released.y() - br.y()) < 10.0;
    EXPECT_EQ(surface_->cursor().shape(), on_handle ? Qt::SizeFDiagCursor : Qt::SizeAllCursor);
}

// ---- Content aspect ratio (drives the Record page's content-fit box) -------

TEST_F(PreviewSurfaceWebcamTest, ContentAspectRatioTracksLiveFrame) {
    // Fixture SetUp installed a 1600x900 frame.
    EXPECT_NEAR(surface_->contentAspectRatio(), 16.0 / 9.0, 1e-9);

    int change_count = 0;
    double last_ar = -1.0;
    QObject::connect(surface_.get(), &PreviewSurface::contentAspectRatioChanged, [&](double ar) {
        ++change_count;
        last_ar = ar;
    });

    QImage square(1000, 1000, QImage::Format_RGB32);
    square.fill(QColor(10, 10, 10));
    surface_->setLiveFrame(square);
    EXPECT_EQ(change_count, 1);
    EXPECT_NEAR(last_ar, 1.0, 1e-9);
    EXPECT_NEAR(surface_->contentAspectRatio(), 1.0, 1e-9);

    // Same dimensions again → no redundant notification.
    QImage square2(1000, 1000, QImage::Format_RGB32);
    square2.fill(QColor(30, 30, 30));
    surface_->setLiveFrame(square2);
    EXPECT_EQ(change_count, 1);

    // Source gone → unknown (0.0), notified once.
    surface_->setLiveFrame(QImage());
    EXPECT_EQ(change_count, 2);
    EXPECT_NEAR(last_ar, 0.0, 1e-9);
    EXPECT_NEAR(surface_->contentAspectRatio(), 0.0, 1e-9);
}

} // namespace
} // namespace exosnap::ui::widgets
