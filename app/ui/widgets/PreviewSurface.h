#pragma once

#include <QImage>
#include <QKeyEvent>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../models/WebcamSettings.h"
#include "../../services/PreviewHelpers.h"
#include <recorder_core/recorder_session.h>

class QLabel;
class QPainter;
class QTimer;
class QVariantAnimation;

namespace exosnap {
class DxgiPreviewRenderer;
}

namespace exosnap::ui::widgets {

class PreviewSurface : public QWidget {
    Q_OBJECT
  public:
    enum class FrameTone { Ready, Recording, Warn, Blocked };

    explicit PreviewSurface(QWidget* parent = nullptr);
    ~PreviewSurface() override;

    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    void setRecording(bool recording);
    bool isRecording() const noexcept;

    // SUITE-PHASE-F: in-window countdown ring drawn over the preview.
    // Call with active=true and the current digit + total duration while the
    // pre-record countdown is running; call with active=false (or remaining=0)
    // to clear.  The ring is drawn directly in paintEvent, so it works both
    // when the DXGI native child is absent (QImage path) and in the Qt overlay
    // layer (the ring is composited above the DXGI surface via a layered window
    // trick: we place a translucent QWidget child over the HWND).
    // In practice the DXGI surface occludes the Qt paint; the on-screen
    // CountdownOverlayWindow handles that case. This method drives the
    // in-window preview-first appearance (no DXGI active = QImage preview).
    void setCountdownState(bool active, int remaining_seconds, int duration_seconds);

    void setLiveFrame(QImage frame);

    // crop_box: optional monitor-relative physical-pixel crop for Region targets.
    // Pass std::nullopt for Display and Window targets (no crop).
    bool tryStartDxgiPreview(const recorder_core::CaptureTarget& target, uint32_t frame_rate_num,
                             uint32_t frame_rate_den, std::optional<exosnap::PreviewCropBox> crop_box = std::nullopt);

    // Start the DXGI preview renderer with no capture of its own: frames arrive
    // exclusively through beginPushedSource (the DXGI capture hub, and the
    // engine during recording). See DxgiPreviewRenderer::StartPushedOnly.
    bool tryStartDxgiPushedPreview(const recorder_core::CaptureTarget& target, uint32_t frame_rate_num,
                                   uint32_t frame_rate_den);
    void stopDxgiPreview();
    [[nodiscard]] bool isDxgiPreviewActive() const noexcept;
    // True while the live DXGI preview has presented at least one real frame —
    // the exact condition under which requestDxgiSnapshot can succeed.
    [[nodiscard]] bool isDxgiSnapshotReady() const noexcept;
    void repositionDxgiPreview();

    // Switch the active DXGI preview to a shared source texture: the engine's
    // WYSIWYG tap during recording (raw_source_frames = false), or the DXGI
    // capture hub's raw feed (true — the renderer draws cursor + PiP itself).
    // No-op if no DXGI preview is running. Ownership of the NT handle transfers
    // to the renderer; `tap` names the display transform the renderer must
    // apply. See DxgiPreviewRenderer::BeginPushedSource.
    void beginPushedSource(void* nt_handle, uint32_t width, uint32_t height, recorder_core::PreviewTapDesc tap,
                           bool raw_source_frames = false);
    // Revert to the DXGI preview's own WGC capture. No-op if no renderer exists.
    void endPushedSource();

    // One-shot readback of whatever the DXGI preview is currently showing (the
    // fully composited, tone-mapped WYSIWYG frame — same content the renderer
    // presents). No-op (callback fires with ok=false) if no DXGI preview is
    // active. See DxgiPreviewRenderer::RequestSnapshot for the threading
    // contract: the callback fires on the render thread, not the UI thread.
    void requestDxgiSnapshot(std::function<void(bool, uint32_t, uint32_t, std::vector<uint8_t>, std::string)> callback);

    void setTopMetaText(const QString& text);
    // Help text shown under the branded empty-state placeholder (no live preview).
    // Empty => the default "No source selected — choose one to preview" prompt.
    void setPlaceholderHint(const QString& text);
    void setBottomLeftText(const QString& text);
    void setBottomRightText(const QString& text);
    void setFrameTone(FrameTone tone);

    void setWebcamFrame(QImage frame);
    void setWebcamOverlayEnabled(bool enabled);
    void setWebcamOverlayRect(QRectF rect_norm);
    QRectF webcamOverlayRect() const noexcept {
        return webcam_rect_norm_;
    }
    QRectF defaultWebcamOverlayRect(double camera_aspect_w_over_h = 0.0) const;
    void setAspectRatioLocked(bool locked);

    // Real horizontal mirror of the webcam image (no vertical flip). Applied to the
    // on-screen PiP (Qt paint and DXGI overlay). The recording compositor mirrors
    // independently via WebcamConfig::mirror; both read the same persisted flag.
    void setWebcamMirror(bool mirror);
    [[nodiscard]] bool webcamMirror() const noexcept {
        return webcam_mirror_;
    }

    // PiP compositing opacity (0 = fully transparent, 1 = fully opaque). Applied to
    // both the Qt paint path and the DXGI overlay so the record-page preview stays
    // WYSIWYG with the recording compositor. Never applied to the edit chrome.
    void setWebcamOpacity(float opacity);
    [[nodiscard]] float webcamOpacity() const noexcept {
        return webcam_opacity_;
    }

    // Chroma key for the PiP. Only the DXGI overlay applies it — it runs the same
    // shader as the recording compositor. The Qt fallback paint path draws the raw
    // camera image; it is only reached before a capture target has a live preview.
    void setWebcamChromaKey(const WebcamChromaKeySettings& chroma);
    [[nodiscard]] const WebcamChromaKeySettings& webcamChromaKey() const noexcept {
        return webcam_chroma_;
    }

    // Editing lock. When locked the PiP video stays visible but selection/drag/resize
    // and edit chrome are disabled and pointer events pass through. RecordPage
    // keeps this unlocked in states whose overlay edits are live-applied to the
    // running session.
    void setWebcamEditLocked(bool locked);
    [[nodiscard]] bool isWebcamEditLocked() const noexcept {
        return webcam_edit_locked_;
    }

    // Selection (shows edit chrome). Deselecting keeps the confirmed placement.
    void setWebcamSelected(bool selected);
    [[nodiscard]] bool isWebcamSelected() const noexcept {
        return webcam_selected_;
    }

    [[nodiscard]] bool isWebcamOverlayEnabled() const noexcept {
        return webcam_enabled_;
    }

    // End any in-progress drag/resize and release pointer/keyboard capture without
    // committing a geometry change beyond what is already applied. Used on page/target
    // changes so no transient capture or stale interaction survives.
    void cancelWebcamInteraction();

    // Aspect ratio (width / height) of the source currently displayed in the
    // preview: the live DXGI source dimensions while the DXGI preview runs,
    // otherwise the current QImage frame. Returns 0.0 when no source is known
    // (empty state). Drives the Record page's content-fit preview box.
    [[nodiscard]] double contentAspectRatio() const;

    // --- Webcam magnifier (hover-to-enlarge) ---
    // A small icon appears on hover over the PiP; clicking it animates the PiP into
    // a larger floating view centred in the preview (scale/fade, ~180ms). Clicking
    // the icon again (now a restore glyph) or clicking outside the floating view
    // animates it back down. Preview-only affordance: it never changes
    // webcam_rect_norm_ (the confirmed, WYSIWYG placement) or persists across
    // restarts — purely a transient "peek at it bigger" convenience.
    [[nodiscard]] bool isWebcamEnlarged() const noexcept {
        return webcam_enlarged_;
    }

    // --- Visual-harness / test accessors (no side effects) ---
    // Pixel rect of the PiP within the widget, mapped through the same content rect
    // used for hit-testing and DXGI rendering.
    [[nodiscard]] QRect webcamMappedPreviewRect() const;
    // Active handle/drag descriptor: "none" | "move" | "tl" | "tr" | "bl" | "br".
    [[nodiscard]] QString webcamActiveHandle() const;
    // True while the pointer hovers the (non-enlarged) PiP — gates whether the
    // magnifier hotspot can be active at all.
    [[nodiscard]] bool isWebcamHovered() const noexcept {
        return webcam_hovered_;
    }
    // Current 0 (normal) .. 1 (fully enlarged) animation progress.
    [[nodiscard]] double webcamMagnifyProgress() const noexcept {
        return magnify_progress_;
    }
    // Pixel rect of the magnifier/restore cursor hotspot for the current state,
    // mapped to widget coordinates. Empty when there is no active hotspot.
    [[nodiscard]] QRect webcamMagnifierIconMappedRect() const;

  signals:
    void webcamOverlayMoved(QRectF rect_norm);
    void webcamSelectionChanged(bool selected);
    // Emitted once per DXGI preview run when the renderer presents its first real
    // frame — the moment isDxgiSnapshotReady() flips true. Marshaled to this thread.
    void dxgiFirstFrameRendered();
    // Emitted whenever contentAspectRatio() changes (target switch, region change,
    // live DXGI source resize, or the source going away — then 0.0).
    void contentAspectRatioChanged(double aspect_w_over_h);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    // BUG-C: proactively toggle the DXGI child HWND's OS-level visibility in
    // lockstep with this widget's own show/hide, instead of relying on Qt's
    // native-window hide cascade (see PreviewSurface.cpp for why that cascade is
    // not trustworthy timing-wise in this app's frameless/custom-chrome window).
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    enum class DragMode { None, Move, ResizeTL, ResizeTR, ResizeBL, ResizeBR };

    QRectF webcamPixelRect() const;
    QRectF displayedFrameRect() const;
    QRectF displayedFrameRectForSource(int srcW, int srcH) const;
    DragMode hitTestWebcam(QPointF pos) const;
    // Target rect (widget pixels) the PiP animates into when enlarged: centred in
    // the preview panel, sized to a comfortable fraction of it, aspect-preserving.
    QRectF webcamEnlargedTargetRect() const;
    // Magnifier/restore cursor hotspot rect, anchored to the top-right corner of
    // `pip_rect` (either the normal PiP or the current lerped/enlarged rect). Empty
    // if `pip_rect` is too small to host the hotspot without dominating it.
    QRectF magnifierIconRect(const QRectF& pip_rect) const;
    void ensureMagnifyAnimation();
    // Starts (or reverses) the enlarge/restore animation toward `enlarged`. No-op if
    // already at that target. Entering enlarged mode cancels any in-progress
    // drag/selection first.
    void setWebcamEnlarged(bool enlarged);
    void applyDragFromPointer(QPointF pos, Qt::KeyboardModifiers modifiers);
    void snapOverlayRectToCurrentAspect();
    void applyDxgiPreviewResize();
    // True only when the webcam PiP can currently be selected/dragged/resized.
    [[nodiscard]] bool webcamEditingAllowed() const noexcept;
    // Push current overlay video + placement + chrome state to the DXGI renderer so
    // the live preview composites the PiP itself (the native child HWND occludes Qt).
    void syncWebcamOverlayToDxgi();
    // Rasterize the meta/stats rows (top_row_ / bottom_row_) and push them to the
    // DXGI renderer as OSD sprites drawn ON TOP of the frame and the PiP — the
    // native child HWND occludes the Qt-painted labels, so during a live preview
    // the renderer composites them itself. Keeps the footer-above-PiP z-order
    // identical in the Qt paint path (child widgets paint above paintEvent) and
    // the DXGI path (OSD sprites drawn last).
    void syncOsdToDxgi();
    // While magnify_progress_ > 0: pushes a full-panel dim scrim (OSD sprite slot
    // 2, alpha scaled by magnify_progress_) and re-points the DXGI webcam overlay
    // at the interpolated (normal -> enlarged) rect instead of webcam_rect_norm_ --
    // preview-only, never persisted. At progress == 0, clears the scrim and
    // restores the normal placement via syncWebcamOverlayToDxgi().
    void syncEnlargedWebcamToDxgi();
    // Re-evaluate the hover cursor for the last known pointer position. Called on
    // every event that can change the hit map under a stationary pointer
    // (selection/lock/enable changes, placement updates, drag end), not just on
    // mouse moves — a stale resize/move cursor otherwise persists until the
    // pointer leaves and re-enters the surface.
    void applyHoverCursor();
    // Emit contentAspectRatioChanged if the displayed source aspect changed.
    void notifyAspectRatioMaybeChanged();
    // Branded capture-safe fallback drawn in the preview when there is no live
    // frame and no DXGI preview (no source / source unavailable). Mirrors the
    // title-bar lockup: aperture mark + two-tone "exosnap".
    void paintBrandPlaceholder(QPainter& painter, const QRectF& frame_rect);

    QImage current_frame_;
    QString placeholder_hint_;
    QImage webcam_frame_;
    bool webcam_enabled_ = false;
    bool aspect_ratio_locked_ = true;
    bool webcam_mirror_ = false;
    float webcam_opacity_ = 1.0f;
    WebcamChromaKeySettings webcam_chroma_{};
    bool webcam_selected_ = false;
    bool webcam_edit_locked_ = false;
    double webcam_aspect_ratio_ = 16.0 / 9.0;
    QRectF webcam_rect_norm_{0.0, 0.0, 0.25, 0.25};

    // --- Webcam magnifier (hover-to-enlarge) ---
    bool webcam_hovered_ = false;  // pointer over the (non-enlarged) PiP
    bool webcam_enlarged_ = false; // logical target state (animation may still be mid-flight)
    QVariantAnimation* magnify_animation_ = nullptr;
    double magnify_progress_ = 0.0; // 0 = normal placement, 1 = fully enlarged

    DragMode drag_mode_ = DragMode::None;
    QPointF drag_origin_;
    QRectF drag_start_rect_;
    // Geometry captured at drag start so Escape can roll back to it.
    QRectF pre_interaction_rect_;
    bool drag_modifier_toggle_held_ = false;

    // Last pointer position over the surface (widget coordinates), tracked so
    // applyHoverCursor() can refresh the cursor without a fresh mouse move.
    QPointF hover_pos_;
    bool hover_pos_valid_ = false;

    // Last aspect ratio reported through contentAspectRatioChanged.
    double last_notified_aspect_ = 0.0;
    // Polls the live DXGI source dimensions (they change asynchronously on the
    // render thread, e.g. when a captured window is resized). Runs only while
    // the DXGI preview is active.
    QTimer* aspect_poll_timer_ = nullptr;

    QLabel* top_meta_label_ = nullptr;
    QLabel* bottom_left_label_ = nullptr;
    QLabel* bottom_right_label_ = nullptr;
    QWidget* top_row_ = nullptr;
    QWidget* bottom_row_ = nullptr;
    bool recording_ = false;
    FrameTone frame_tone_ = FrameTone::Ready;

    // In-window countdown ring state (SUITE-PHASE-F).
    bool countdown_active_ = false;
    int countdown_remaining_ = 0;
    int countdown_duration_ = 0;

    // Full overlay texts; the labels render an elided variant fitted to the
    // current width and the rows hide entirely below a minimum width (VR-009).
    QString top_meta_full_;
    QString bottom_left_full_;
    QString bottom_right_full_;
    void applyOverlayTextElision();

    std::unique_ptr<exosnap::DxgiPreviewRenderer> dxgi_renderer_;
    bool dxgi_active_ = false;
};

} // namespace exosnap::ui::widgets
