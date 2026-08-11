#include "RecordWebcamFrameProvider.h"

#include <QMutexLocker>

#include <algorithm>
#include <cmath>

namespace exosnap::quick {
namespace {

QImage applyChromaKey(QImage image, const auto& key) {
    if (!key.enabled || image.isNull())
        return image;
    image = image.convertToFormat(QImage::Format_RGBA8888);
    const float key_red = static_cast<float>(key.red) / 255.0f;
    const float key_green = static_cast<float>(key.green) / 255.0f;
    const float key_blue = static_cast<float>(key.blue) / 255.0f;
    const float key_cb = -0.169f * key_red - 0.331f * key_green + 0.5f * key_blue + 0.5f;
    const float key_cr = 0.5f * key_red - 0.419f * key_green - 0.081f * key_blue + 0.5f;
    const float key_luminance = 0.2126f * key_red + 0.7152f * key_green + 0.0722f * key_blue;
    const float key_chroma[3] = {key_red - key_luminance, key_green - key_luminance, key_blue - key_luminance};
    const float key_length_squared =
        key_chroma[0] * key_chroma[0] + key_chroma[1] * key_chroma[1] + key_chroma[2] * key_chroma[2];
    const float softness = std::max(key.softness, 0.001f);

    for (int y = 0; y < image.height(); ++y) {
        uchar* pixels = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x, pixels += 4) {
            float red = static_cast<float>(pixels[0]) / 255.0f;
            float green = static_cast<float>(pixels[1]) / 255.0f;
            float blue = static_cast<float>(pixels[2]) / 255.0f;
            const float cb = -0.169f * red - 0.331f * green + 0.5f * blue + 0.5f;
            const float cr = 0.5f * red - 0.419f * green - 0.081f * blue + 0.5f;
            const float distance = std::hypot(cb - key_cb, cr - key_cr);
            const float keyed_alpha = std::clamp((distance - key.tolerance) / softness, 0.0f, 1.0f);

            if (key.spill_reduction > 0.001f && keyed_alpha > 0.001f && key_length_squared > 0.001f) {
                const float luminance = 0.2126f * red + 0.7152f * green + 0.0722f * blue;
                const float color_chroma[3] = {red - luminance, green - luminance, blue - luminance};
                const float projection =
                    color_chroma[0] * key_chroma[0] + color_chroma[1] * key_chroma[1] + color_chroma[2] * key_chroma[2];
                if (projection > 0.0f) {
                    const float strength = key.spill_reduction * (1.0f - keyed_alpha) * projection / key_length_squared;
                    red = std::clamp(red - key_chroma[0] * strength, 0.0f, 1.0f);
                    green = std::clamp(green - key_chroma[1] * strength, 0.0f, 1.0f);
                    blue = std::clamp(blue - key_chroma[2] * strength, 0.0f, 1.0f);
                }
            }
            pixels[0] = static_cast<uchar>(std::lround(red * 255.0f));
            pixels[1] = static_cast<uchar>(std::lround(green * 255.0f));
            pixels[2] = static_cast<uchar>(std::lround(blue * 255.0f));
            pixels[3] = static_cast<uchar>(std::lround(static_cast<float>(pixels[3]) * keyed_alpha));
        }
    }
    return image;
}

} // namespace

RecordWebcamFrameProvider::RecordWebcamFrameProvider() : QQuickImageProvider(QQuickImageProvider::Image) {
}

QImage RecordWebcamFrameProvider::requestImage(const QString&, QSize* size, const QSize& requested_size) {
    QImage frame;
    ChromaKey chroma;
    {
        QMutexLocker lock(&mutex_);
        frame = frame_;
        chroma = chroma_;
    }
    if (size != nullptr)
        *size = frame.size();
    if (frame.isNull() || !requested_size.isValid() || requested_size.isEmpty())
        return applyChromaKey(std::move(frame), chroma);
    const QSize bounded_size = requested_size.boundedTo(frame.size());
    return applyChromaKey(frame.scaled(bounded_size, Qt::KeepAspectRatio, Qt::FastTransformation), chroma);
}

QImage RecordWebcamFrameProvider::latestFrame() const {
    QMutexLocker lock(&mutex_);
    return frame_;
}

void RecordWebcamFrameProvider::setChromaKey(bool enabled, uint8_t red, uint8_t green, uint8_t blue, float tolerance,
                                             float softness, float spill_reduction) {
    QMutexLocker lock(&mutex_);
    chroma_ = {enabled,
               red,
               green,
               blue,
               std::clamp(tolerance, 0.0f, 1.0f),
               std::clamp(softness, 0.0f, 1.0f),
               std::clamp(spill_reduction, 0.0f, 1.0f)};
}

void RecordWebcamFrameProvider::submitFrame(QImage frame) {
    QMutexLocker lock(&mutex_);
    frame_ = std::move(frame);
}

} // namespace exosnap::quick
