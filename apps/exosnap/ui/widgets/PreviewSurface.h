#pragma once

#include <QImage>
#include <QKeyEvent>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <memory>
#include <optional>

#include "../../services/PreviewHelpers.h"
#include <recorder_core/recorder_session.h>

class QLabel;

namespace exosnap {
class DxgiPreviewRenderer;
}

namespace exosnap::ui::widgets {

class StatusPill;

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

    void setLiveFrame(QImage frame);

    // crop_box: optional monitor-relative physical-pixel crop for Region targets.
    // Pass std::nullopt for Display and Window targets (no crop).
    bool tryStartDxgiPreview(const recorder_core::CaptureTarget& target, uint32_t frame_rate_num,
                             uint32_t frame_rate_den, std::optional<exosnap::PreviewCropBox> crop_box = std::nullopt);
    void stopDxgiPreview();
    [[nodiscard]] bool isDxgiPreviewActive() const noexcept;
    void repositionDxgiPreview();

    void setStatusText(const QString& text);
    void setTopMetaText(const QString& text);
    void setCenterTitle(const QString& text);
    void setCenterSubtitle(const QString& text);
    void setBottomLeftText(const QString& text);
    void setBottomRightText(const QString& text);
    void setFrameTone(FrameTone tone);

    StatusPill* statusPill() const noexcept;

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

    // Editing lock. When locked the PiP video stays visible but selection/drag/resize
    // and edit chrome are disabled and pointer events pass through. Set true outside
    // the Ready state (countdown/recording/paused/stopping/blocked) so preview and the
    // snapshot-based recording compositor cannot diverge.
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

    // --- Visual-harness / test accessors (no side effects) ---
    // Pixel rect of the PiP within the widget, mapped through the same content rect
    // used for hit-testing and DXGI rendering.
    [[nodiscard]] QRect webcamMappedPreviewRect() const;
    // Active handle/drag descriptor: "none" | "move" | "tl" | "tr" | "bl" | "br".
    [[nodiscard]] QString webcamActiveHandle() const;

  signals:
    void webcamOverlayMoved(QRectF rect_norm);
    void webcamSelectionChanged(bool selected);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

  private:
    enum class DragMode { None, Move, ResizeTL, ResizeTR, ResizeBL, ResizeBR };

    QRectF webcamPixelRect() const;
    QRectF displayedFrameRect() const;
    QRectF displayedFrameRectForSource(int srcW, int srcH) const;
    DragMode hitTestWebcam(QPointF pos) const;
    void applyDragFromPointer(QPointF pos, Qt::KeyboardModifiers modifiers);
    void snapOverlayRectToCurrentAspect();
    void applyDxgiPreviewResize();
    // True only when the webcam PiP can currently be selected/dragged/resized.
    [[nodiscard]] bool webcamEditingAllowed() const noexcept;
    // Push current overlay video + placement + chrome state to the DXGI renderer so
    // the live preview composites the PiP itself (the native child HWND occludes Qt).
    void syncWebcamOverlayToDxgi();

    QImage current_frame_;
    QImage webcam_frame_;
    bool webcam_enabled_ = false;
    bool aspect_ratio_locked_ = true;
    bool webcam_mirror_ = false;
    bool webcam_selected_ = false;
    bool webcam_edit_locked_ = false;
    double webcam_aspect_ratio_ = 16.0 / 9.0;
    QRectF webcam_rect_norm_{0.0, 0.0, 0.25, 0.25};

    DragMode drag_mode_ = DragMode::None;
    QPointF drag_origin_;
    QRectF drag_start_rect_;
    // Geometry captured at drag start so Escape can roll back to it.
    QRectF pre_interaction_rect_;
    bool drag_modifier_toggle_held_ = false;

    StatusPill* status_pill_ = nullptr;
    QLabel* top_meta_label_ = nullptr;
    QLabel* center_title_label_ = nullptr;
    QLabel* center_subtitle_label_ = nullptr;
    QLabel* bottom_left_label_ = nullptr;
    QLabel* bottom_right_label_ = nullptr;
    QWidget* top_row_ = nullptr;
    QWidget* center_box_ = nullptr;
    QWidget* bottom_row_ = nullptr;
    bool recording_ = false;
    FrameTone frame_tone_ = FrameTone::Ready;

    std::unique_ptr<exosnap::DxgiPreviewRenderer> dxgi_renderer_;
    bool dxgi_active_ = false;
};

} // namespace exosnap::ui::widgets
