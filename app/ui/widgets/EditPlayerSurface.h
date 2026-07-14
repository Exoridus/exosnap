#pragma once

#include <QImage>
#include <QString>
#include <QWidget>

namespace exosnap::ui::widgets {

// The Edit-page video player's paint surface. Modeled directly on
// CameraPreview: shows the current decoded frame letterboxed inside a
// rounded dark panel, or a placeholder message when there is no frame yet
// (before the first decode, or a decode failure -- see EditPlayerSession's
// Open() contract).
class EditPlayerSurface : public QWidget {
    Q_OBJECT
  public:
    explicit EditPlayerSurface(QWidget* parent = nullptr);

    QSize sizeHint() const override;

    // Shows a decoded frame. An empty/null image falls back to the placeholder.
    void setFrame(QImage frame);

    // Drops the current frame and shows the placeholder text again.
    void clearFrame();

    // Sets the message shown when no frame is present (supports '\n').
    void setPlaceholderText(const QString& text);

    [[nodiscard]] bool hasFrame() const noexcept {
        return !frame_.isNull();
    }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QImage frame_;
    QString placeholder_ = QStringLiteral("Preview unavailable");
};

} // namespace exosnap::ui::widgets
