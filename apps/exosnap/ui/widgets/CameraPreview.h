#pragma once

#include <QImage>
#include <QString>
#include <QWidget>

namespace exosnap::ui::widgets {

// Lightweight camera-only preview surface for the Webcam setup page.
//
// Shows the live webcam frame letterboxed inside a rounded dark panel. This is
// deliberately NOT a composition/target preview: it has no overlay rect, no
// drag handles, and no recording-target chrome. When there is no frame it
// paints a centered placeholder message (empty / unavailable states).
class CameraPreview : public QWidget {
    Q_OBJECT
  public:
    explicit CameraPreview(QWidget* parent = nullptr);

    QSize sizeHint() const override;

    // Show a live frame. An empty/null image falls back to the placeholder.
    void setFrame(QImage frame);

    // Drop the current frame and show the placeholder text again.
    void clearFrame();

    // Set the message shown when no frame is present (supports '\n').
    void setPlaceholderText(const QString& text);

    [[nodiscard]] bool hasFrame() const noexcept {
        return !frame_.isNull();
    }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QImage frame_;
    QString placeholder_ = QStringLiteral("Camera preview");
};

} // namespace exosnap::ui::widgets
