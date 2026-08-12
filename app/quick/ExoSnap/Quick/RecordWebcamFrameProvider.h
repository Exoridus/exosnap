#pragma once

#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>

#include <cstdint>

namespace exosnap::quick {

// Thread-safe, narrow image-provider endpoint for the coordinator's existing
// webcam preview callback. Main desktop preview pixels never pass through it.
class RecordWebcamFrameProvider final : public QQuickImageProvider {
  public:
    RecordWebcamFrameProvider();

    [[nodiscard]] QImage requestImage(const QString& id, QSize* size, const QSize& requested_size) override;
    [[nodiscard]] QImage latestFrame() const;
    void submitFrame(QImage frame);
    void setChromaKey(bool enabled, uint8_t red, uint8_t green, uint8_t blue, float tolerance, float softness,
                      float spill_reduction);

  private:
    struct ChromaKey {
        bool enabled = false;
        uint8_t red = 0;
        uint8_t green = 255;
        uint8_t blue = 0;
        float tolerance = 0.4f;
        float softness = 0.15f;
        float spill_reduction = 0.3f;
    };

    mutable QMutex mutex_;
    QImage frame_;
    ChromaKey chroma_;
};

} // namespace exosnap::quick
