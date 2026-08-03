#pragma once

#include <QImage>
#include <QString>
#include <QWidget>

#include <memory>

#include <recorder_core/edit_player_engine.h> // RawDecodedVideoFrame

namespace exosnap {
class EditPlayerRenderer;
}

namespace exosnap::ui::widgets {

// The Edit-page video player's paint surface.
//
// Two independent presentation paths coexist on purpose:
//
//  - The GPU render path (native child HWND + EditPlayerRenderer, see
//    docs/superpowers/specs/2026-08-03-editor-playback-gpu-render-design.md):
//    opt in via startGpuRendering(), then feed frames via presentFrame().
//    EditExportPage's real playback wiring uses this. The native child
//    window occludes ordinary Qt painting the same way PreviewSurface's
//    DXGI child window does, so once this path is active this widget itself
//    paints nothing.
//  - The legacy QImage/QPainter path (setFrame/clearFrame): a plain
//    already-composited-BGRA paint path, unchanged from before this design.
//    This is the ONLY path while startGpuRendering() has not been called (or
//    failed) -- notably the --visual-test render harness's standalone
//    EditPlayerSurface instance (see MainWindow::applyVisualScenario())
//    deliberately never calls startGpuRendering(), because a native child
//    HWND's D3D11-composited pixels are not something a QWidget screenshot
//    grab captures; that harness needs the QPainter path to stay
//    pixel-provable, so it keeps using setFrame(QImage) exactly as before
//    this task.
class EditPlayerSurface : public QWidget {
    Q_OBJECT
  public:
    explicit EditPlayerSurface(QWidget* parent = nullptr);
    ~EditPlayerSurface() override;

    QSize sizeHint() const override;

    // Explicit opt-in to the GPU render path -- mirrors PreviewSurface::
    // tryStartDxgiPreview's "nothing happens until a caller asks" contract
    // rather than creating a native window automatically. Idempotent (returns
    // true immediately if already started). Returns false if the native
    // window or the D3D11 device/swap chain could not be created (e.g. no
    // hardware adapter) -- the legacy QPainter path stays active in that
    // case, same as if this had never been called.
    bool startGpuRendering();

    // GPU render path: presents a decoded frame. Thread-safe: callable from
    // any thread, including the engine's own decode/seek threads (see
    // EditPlayerRenderer's threading-model doc comment) -- do not marshal
    // this onto the UI thread first, that would reintroduce the per-frame
    // UI-thread hop the GPU render path exists to remove. A no-op before
    // startGpuRendering() has succeeded.
    void presentFrame(recorder_core::RawDecodedVideoFrame frame, float hdr_peak_scale = 1.0f);
    // GPU render path: updates the playback-clock snapshot the renderer's
    // present-gate consults. See EditPlayerRenderer::SetClockUs. A no-op
    // before startGpuRendering() has succeeded.
    void updateClockUs(int64_t media_time_us) noexcept;

    // Legacy/harness path: shows an already-composited BGRA frame via plain
    // Qt painting -- see the class comment. An empty/null image falls back to
    // the placeholder.
    void setFrame(QImage frame);

    // Drops the current frame (either path) and shows the placeholder text again.
    void clearFrame();

    // Sets the message shown when no frame is present (supports '\n'). Only
    // takes visible effect immediately while no frame is currently shown --
    // never interrupts a frame already on screen.
    void setPlaceholderText(const QString& text);

    [[nodiscard]] bool hasFrame() const noexcept {
        return has_frame_;
    }

    // Test/diagnostic access to the owned renderer -- null unless
    // startGpuRendering() has been called and succeeded.
    [[nodiscard]] EditPlayerRenderer* rendererForTest() const noexcept {
        return renderer_.get();
    }

  protected:
    void resizeEvent(QResizeEvent* event) override;
    // Only paints while the GPU render path is not active (see the class
    // comment) -- when it is, the native child HWND occludes this rect and
    // this never visibly runs.
    void paintEvent(QPaintEvent* event) override;
    // Explicitly toggles the native child window's OS-level visibility --
    // same bug class (and fix) as PreviewSurface's own hideEvent/showEvent:
    // Qt's native-window hide cascade is not guaranteed to reach a manually
    // created WS_CHILD window in this app's frameless/custom-chrome window in
    // the same paint cycle as the next page's first paint, which would leave
    // a stale video frame visibly lingering over whatever is shown next.
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void applyResize();

    std::unique_ptr<EditPlayerRenderer> renderer_;
    QImage frame_; // legacy/harness path only
    QString placeholder_ = QStringLiteral("Preview unavailable");
    bool has_frame_ = false;
};

} // namespace exosnap::ui::widgets
