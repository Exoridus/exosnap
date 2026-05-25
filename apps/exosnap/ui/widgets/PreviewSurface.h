#pragma once

#include <QImage>
#include <QKeyEvent>
#include <QPointF>
#include <QRectF>
#include <QWidget>

class QLabel;

namespace exosnap::ui::widgets {

class StatusPill;

class PreviewSurface : public QWidget {
    Q_OBJECT
  public:
    explicit PreviewSurface(QWidget* parent = nullptr);

    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    void setRecording(bool recording);
    bool isRecording() const noexcept;

    // Delivers a live preview frame. Pass a null QImage to clear.
    void setLiveFrame(QImage frame);

    void setStatusText(const QString& text);
    void setTopMetaText(const QString& text);
    void setCenterTitle(const QString& text);
    void setCenterSubtitle(const QString& text);
    void setBottomLeftText(const QString& text);
    void setBottomRightText(const QString& text);

    StatusPill* statusPill() const noexcept;

    // Webcam overlay — W3
    void setWebcamFrame(QImage frame);
    void setWebcamOverlayEnabled(bool enabled);
    // rect in [0,1] normalized to the live-frame display area
    void setWebcamOverlayRect(QRectF rect_norm);
    QRectF webcamOverlayRect() const noexcept {
        return webcam_rect_norm_;
    }
    QRectF defaultWebcamOverlayRect(double camera_aspect_w_over_h = 0.0) const;
    // When true, corner-drag resize preserves the current webcam frame aspect ratio.
    // Holding Shift/Ctrl during a corner drag temporarily toggles the lock.
    void setAspectRatioLocked(bool locked);

  signals:
    // Emitted when user finishes dragging/resizing the overlay (normalized coords).
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

    QRectF webcamPixelRect() const; // webcam rect in widget pixel coords
    QRectF displayedFrameRect() const;
    DragMode hitTestWebcam(QPointF pos) const;
    void applyDragFromPointer(QPointF pos, Qt::KeyboardModifiers modifiers);
    void snapOverlayRectToCurrentAspect();

    QImage current_frame_;
    QImage webcam_frame_;
    bool webcam_enabled_ = false;
    bool aspect_ratio_locked_ = true;
    double webcam_aspect_ratio_ = 16.0 / 9.0; // updated from incoming frames
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
};

} // namespace exosnap::ui::widgets
