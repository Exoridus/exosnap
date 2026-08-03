// Lifecycle/resize coverage for EditPlayerSurface's GPU render path
// (2026-08-03 editor-playback-gpu-render design, Task 4). Drives the real
// EditPlayerRenderer against a real hardware D3D11 device -- same live-
// hardware precedent as test_preview_surface_webcam.cpp's
// PreviewSurfaceWebcamDxgiLiveTest (DxgiPreviewRenderer::InitD3D11()
// hardcodes D3D_DRIVER_TYPE_HARDWARE); GTEST_SKIP covers headless/GPU-less
// runners. These link the real recorder_core and so run the real
// EditFrameGpuConverter shaders, but they deliberately do NOT assert per-pixel
// correctness -- that is test_edit_frame_gpu_converter.cpp's job, where the
// output is pinned against the CPU reference converters on a WARP device.
// What these assert is that the pipeline runs and its lifecycle holds: native
// child HWND created, synthetic RawDecodedVideoFrames presented without
// crashing, the present-gate's clock behaves, resize reaches the real Win32
// window, Shutdown() tears down cleanly.
#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>

#include "services/EditPlayerRenderer.h"
#include "ui/widgets/EditPlayerSurface.h"

#include <recorder_core/edit_player_engine.h>

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace exosnap::ui::widgets {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "edit_player_surface_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

// Builds a small, deterministic, self-contained Yuv420P8 frame: Y/U/V planes
// live in one shared buffer referenced by backing_frame, so the frame stays
// valid for as long as any copy of it is alive -- including across the async
// hand-off to EditPlayerRenderer's dedicated render thread, which is exactly
// the ownership contract RawDecodedVideoFrame::backing_frame documents.
recorder_core::RawDecodedVideoFrame MakeSyntheticFrame(int64_t pts_us, uint32_t w, uint32_t h, uint8_t y_fill) {
    const uint32_t cw = w / 2;
    const uint32_t ch = h / 2;
    auto buffer = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w) * h + static_cast<size_t>(cw) * ch * 2);
    std::memset(buffer->data(), y_fill, static_cast<size_t>(w) * h);
    std::memset(buffer->data() + static_cast<size_t>(w) * h, 128, static_cast<size_t>(cw) * ch * 2);

    recorder_core::RawDecodedVideoFrame frame;
    frame.pts_us = pts_us;
    frame.width = w;
    frame.height = h;
    frame.format = recorder_core::DecodedPixelFormat::Yuv420P8;
    frame.y_stride_bytes = w;
    frame.u_stride_bytes = cw;
    frame.v_stride_bytes = cw;
    frame.y_plane = buffer->data();
    frame.u_plane = buffer->data() + static_cast<size_t>(w) * h;
    frame.v_plane = buffer->data() + static_cast<size_t>(w) * h + static_cast<size_t>(cw) * ch;
    frame.matrix = recorder_core::MatrixCoefficients::Bt709;
    frame.range = recorder_core::ColorRange::Limited;
    frame.backing_frame = std::move(buffer);
    return frame;
}

// Pumps a plain busy-wait (the render thread is a std::jthread independent of
// the Qt event loop, so no QCoreApplication::processEvents() is needed to
// make progress) until `predicate` is true or timeout_ms elapses.
template <typename Predicate> bool WaitUntil(Predicate&& predicate, int timeout_ms = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.hasExpired(timeout_ms))
            return false;
        Sleep(2);
    }
    return true;
}

// Real-hardware fixture: creates the surface, shows it off-screen (a real
// swap chain needs a genuinely realized top-level window, not merely a
// native HWND -- same WA_DontShowOnScreen trick
// PreviewSurfaceWebcamDxgiLiveTest uses) and opts into the GPU render path.
// GTEST_SKIP covers a headless/no-hardware-adapter CI runner.
class EditPlayerSurfaceGpuTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    void SetUp() override {
        surface_ = std::make_unique<EditPlayerSurface>();
        surface_->resize(400, 300);
        surface_->setAttribute(Qt::WA_DontShowOnScreen, true);
        surface_->show();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

        if (!surface_->startGpuRendering())
            GTEST_SKIP() << "No hardware D3D11 adapter in this environment (headless CI runner).";
    }

    std::unique_ptr<EditPlayerSurface> surface_;
};

TEST_F(EditPlayerSurfaceGpuTest, PresentingSyntheticFramesDrawsWithoutCrashing) {
    exosnap::EditPlayerRenderer* renderer = surface_->rendererForTest();
    ASSERT_NE(renderer, nullptr);
    EXPECT_FALSE(renderer->HasPresentedFrame()); // placeholder only so far, no real frame yet

    // No clock wired (matches the default "no clock" contract: a negative
    // value never drops a frame), so every one of these presents and draws.
    for (int i = 0; i < 5; ++i) {
        surface_->presentFrame(MakeSyntheticFrame(i * 16'000, 16, 16, static_cast<uint8_t>(16 * i)));
    }

    ASSERT_TRUE(WaitUntil([&] { return renderer->HasPresentedFrame(); }))
        << "render thread never drew a presented frame";

    // Resize while frames are flowing must not crash (real HWND + swap chain
    // resize, driven by the widget's own resizeEvent -> applyResize()).
    surface_->resize(800, 450);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    surface_->presentFrame(MakeSyntheticFrame(6 * 16'000, 16, 16, 200));
    Sleep(50); // brief settle so the render thread has a chance to process the post-resize frame

    // clearFrame()/setPlaceholderText() must not crash while the renderer is live.
    surface_->clearFrame();
    surface_->setPlaceholderText(QStringLiteral("No clip open"));

    // Shutdown (via the surface's destructor, below) must tear down cleanly:
    // stop+join the render thread, destroy the child HWND. Reaching the end
    // of this test without hanging or crashing IS the assertion.
}

// Regression guard for the present-gate going stale (final review, 2026-08-03).
//
// The gate drops a frame whose pts_us is behind the last clock value the
// renderer was told about. That is correct DURING playback, but the clock
// snapshot is only refreshed from the playback tick -- so after a pause (or an
// end-of-clip pause at `total`) the renderer keeps holding the position
// playback stopped at, and every subsequent backward scrub/trim-handle frame
// arrives "in the past" and is dropped before any GPU work. The picture then
// stays frozen at the paused position.
//
// EditPlayerSession resets its OWN clock to -1 on Pause() (and SeekTo() pauses
// first); the fix is that EditExportPage republishes that reset through
// updateClockUs(). This test drives exactly that sequence at the surface level:
// a stale positive clock must drop the frame, and republishing the session's
// post-pause -1 must let the very same frame through.
TEST_F(EditPlayerSurfaceGpuTest, StaleClockDropsFrameUntilTheResetIsPropagated) {
    exosnap::EditPlayerRenderer* renderer = surface_->rendererForTest();
    ASSERT_NE(renderer, nullptr);
    ASSERT_FALSE(renderer->HasPresentedFrame());

    // "Played to 10s, then paused" -- the clock the renderer was last told.
    surface_->updateClockUs(10'000'000);

    // "Scrubbed back to 3s": the seek delivers a frame well behind that clock.
    surface_->presentFrame(MakeSyntheticFrame(3'000'000, 16, 16, 90));
    Sleep(200); // generous settle: the render thread wakes on the hand-off either way
    EXPECT_FALSE(renderer->HasPresentedFrame())
        << "a frame behind a non-negative clock must be dropped by the present-gate";

    // The fix: EditPlayerSession::Pause() already reset its own clock to -1;
    // propagating that snapshot reopens the gate.
    surface_->updateClockUs(-1);
    surface_->presentFrame(MakeSyntheticFrame(3'000'000, 16, 16, 90));

    EXPECT_TRUE(WaitUntil([&] { return renderer->HasPresentedFrame(); }))
        << "after the clock reset the scrub frame must be presented, not dropped";
}

TEST_F(EditPlayerSurfaceGpuTest, ShutdownTearsDownCleanly) {
    exosnap::EditPlayerRenderer* renderer = surface_->rendererForTest();
    ASSERT_NE(renderer, nullptr);
    HWND child = renderer->ChildHwndForTest();
    ASSERT_NE(child, nullptr);
    EXPECT_TRUE(IsWindow(child));

    surface_.reset(); // destroys EditPlayerSurface -> EditPlayerRenderer -> Shutdown()

    // The child HWND itself is destroyed synchronously inside Shutdown();
    // IsWindow() on a stale handle reliably reports false rather than
    // crashing, so this is a safe post-destruction check.
    EXPECT_FALSE(IsWindow(child));
}

TEST_F(EditPlayerSurfaceGpuTest, ResizeUpdatesRealChildHwndGeometry) {
    exosnap::EditPlayerRenderer* renderer = surface_->rendererForTest();
    ASSERT_NE(renderer, nullptr);
    HWND child = renderer->ChildHwndForTest();
    ASSERT_NE(child, nullptr);

    RECT before{};
    ASSERT_TRUE(GetClientRect(child, &before));
    EXPECT_GT(before.right, 0);
    EXPECT_GT(before.bottom, 0);

    surface_->resize(800, 450);

    RECT after{};
    ASSERT_TRUE(GetClientRect(child, &after));
    // Relational, not exact-pixel: devicePixelRatioF() may differ per runner,
    // so only the direction of the change is asserted.
    EXPECT_GT(after.right, before.right);
    EXPECT_GT(after.bottom, before.bottom);

    // Shrinking must reach the HWND too, not just grow.
    surface_->resize(200, 150);
    RECT shrunk{};
    ASSERT_TRUE(GetClientRect(child, &shrunk));
    EXPECT_LT(shrunk.right, after.right);
    EXPECT_LT(shrunk.bottom, after.bottom);
}

// No hardware/no-op path: startGpuRendering() itself isn't exercised (the
// fixture above already covers success), but presentFrame()/clearFrame()/
// setPlaceholderText()/resize on a surface that never called it must all
// stay safe no-ops -- this is the ordinary (non-fixture) QPainter-fallback
// path every existing EditExportPage test already runs through.
TEST(EditPlayerSurfaceFallbackTest, MethodsAreSafeNoOpsWithoutGpuRendering) {
    EnsureApplication();
    EditPlayerSurface surface;
    surface.resize(300, 200);
    EXPECT_FALSE(surface.hasFrame());
    surface.presentFrame(MakeSyntheticFrame(0, 8, 8, 1));
    surface.clearFrame();
    surface.setPlaceholderText(QStringLiteral("Preview unavailable"));
    surface.resize(150, 100);
    SUCCEED();
}

} // namespace
} // namespace exosnap::ui::widgets
