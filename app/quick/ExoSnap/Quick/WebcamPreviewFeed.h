#pragma once

#include <QObject>
#include <QtQmlIntegration/qqmlintegration.h>

#include <exosnap/engine/overlay_shader.h>
#include <exosnap/engine/recorder_session.h>

#include <memory>

namespace exosnap::quick {

// The camera picture a stand-alone webcam preview shows: the latest frame
// snapshot and the overlay style the recording applies to it. Viewers register
// while they are on screen, and the application keeps the camera open for them
// only while at least one is.
class WebcamPreviewFeed : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")

  public:
    using QObject::QObject;

    void setFrame(std::shared_ptr<const exosnap::engine::WebcamFrameSnapshot> frame);
    void setStyle(bool mirror, float opacity, const exosnap::engine::ChromaKeyParams& chroma);

    [[nodiscard]] const std::shared_ptr<const exosnap::engine::WebcamFrameSnapshot>& frame() const noexcept {
        return frame_;
    }
    [[nodiscard]] bool mirror() const noexcept {
        return mirror_;
    }
    [[nodiscard]] float opacity() const noexcept {
        return opacity_;
    }
    [[nodiscard]] const exosnap::engine::ChromaKeyParams& chroma() const noexcept {
        return chroma_;
    }

    // Counted, so two previews cannot close the camera on each other.
    void addViewer();
    void removeViewer();
    [[nodiscard]] bool hasViewers() const noexcept {
        return viewers_ > 0;
    }

  signals:
    void frameChanged();
    void styleChanged();
    void hasViewersChanged();

  private:
    std::shared_ptr<const exosnap::engine::WebcamFrameSnapshot> frame_;
    bool mirror_ = false;
    float opacity_ = 1.0f;
    exosnap::engine::ChromaKeyParams chroma_;
    int viewers_ = 0;
};

} // namespace exosnap::quick
