#pragma once

#include <QImage>
#include <QKeyEvent>
#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <memory>

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

    bool tryStartDxgiPreview(const recorder_core::CaptureTarget& target, uint32_t frame_rate_num,
                             uint32_t frame_rate_den);
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

  signals:
    void webcamOverlayMoved(QRectF rect_norm);

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

    QImage current_frame_;
    QImage webcam_frame_;
    bool webcam_enabled_ = false;
    bool aspect_ratio_locked_ = true;
    double webcam_aspect_ratio_ = 16.0 / 9.0;
    QRectF webcam_rect_norm_{0.0, 0.0, 0.25, 0.25};

    DragMode drag_mode_ = DragMode::None;
    QPointF drag_origin_;
    QRectF drag_start_rect_;
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
