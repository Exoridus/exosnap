#include "RecordWebcamFrameProvider.h"

#include <QMutexLocker>

#include <algorithm>
#include <cmath>

namespace exosnap::quick {
namespace {

QImage applyChromaKey(QImage image, const auto& key) {
    if (!key.enabled || image.isNull())
        return image;
    // convertToFormat() on a matching format still costs a full copy plus its
    // allocation; the webcam delivery timer runs this at 30 Hz.
    if (image.format() != QImage::Format_RGBA8888)
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
    // The alpha ramp is a function of the chroma distance, so the square root
    // cannot simply be dropped — but it is only NEEDED between the two clamps.
    // Below `tolerance` the pixel is fully keyed, at or above `tolerance +
    // softness` it is fully opaque, and on a real key almost every pixel is in
    // one of those two bands. Both are decided on the squared distance against
    // the squared bounds; only the ramp itself still calls std::hypot, so the
    // values inside it stay bit-identical to the previous implementation rather
    // than merely close. Both bounds are non-negative (tolerance is clamped to
    // [0,1] and softness to at least 0.001), so squaring preserves the ordering.
    const float keyed_bound = key.tolerance;
    const float opaque_bound = key.tolerance + softness;
    const float keyed_bound_squared = keyed_bound * keyed_bound;
    const float opaque_bound_squared = opaque_bound * opaque_bound;

    for (int y = 0; y < image.height(); ++y) {
        uchar* pixels = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x, pixels += 4) {
            float red = static_cast<float>(pixels[0]) / 255.0f;
            float green = static_cast<float>(pixels[1]) / 255.0f;
            float blue = static_cast<float>(pixels[2]) / 255.0f;
            const float cb = -0.169f * red - 0.331f * green + 0.5f * blue + 0.5f;
            const float cr = 0.5f * red - 0.419f * green - 0.081f * blue + 0.5f;
            const float delta_cb = cb - key_cb;
            const float delta_cr = cr - key_cr;
            const float distance_squared = delta_cb * delta_cb + delta_cr * delta_cr;
            // The clamp is kept on the ramp branch: the squared comparison and
            // std::hypot can disagree by an ulp exactly on a bound, and the old
            // code guaranteed [0, 1] unconditionally.
            const float keyed_alpha =
                distance_squared <= keyed_bound_squared ? 0.0f
                : distance_squared >= opaque_bound_squared
                    ? 1.0f
                    : std::clamp((std::hypot(delta_cb, delta_cr) - key.tolerance) / softness, 0.0f, 1.0f);

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
