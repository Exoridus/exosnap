#include "EditVisualMedia.h"

#include <QColor>
#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

namespace exosnap::visual_tests {
namespace {

// A small integer hash, so a scene's details vary with the seed and the
// segment without any state.
std::uint32_t Mix(std::uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

double Unit(std::uint32_t seed, std::uint32_t salt) {
    return static_cast<double>(Mix(seed * 0x9e3779b9U + salt) & 0xffffU) / 65535.0;
}

class SyntheticAudio final : public engine::WaveformAudioDecoder {
  public:
    explicit SyntheticAudio(std::uint32_t seed) : seed_(seed) {
    }

    bool Decode(std::int64_t first_sample, std::uint32_t frame_count, std::vector<float>& output,
                std::string& error) override {
        error.clear();
        output.assign(static_cast<std::size_t>(frame_count) * 2, 0.0f);
        constexpr double kRate = 48000.0;
        constexpr double kSyllableSeconds = 0.32;
        const double base_hz = 140.0 + 90.0 * Unit(seed_, 1);
        for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
            const double t = static_cast<double>(first_sample + frame) / kRate;
            if (t < 0)
                continue;
            const auto syllable = static_cast<std::uint32_t>(t / kSyllableSeconds);
            // About a quarter of the syllables are pauses; the rest vary in level
            // the way speech over a game soundtrack does.
            const double loudness = Unit(seed_, 100 + syllable);
            const double level = loudness < 0.25 ? 0.04 : 0.25 + 0.7 * loudness;
            const double phase = std::fmod(t, kSyllableSeconds) / kSyllableSeconds;
            const double window = std::pow(std::sin(std::numbers::pi * phase), 2.0);
            const double carrier = 0.65 * std::sin(2 * std::numbers::pi * base_hz * t) +
                                   0.25 * std::sin(2 * std::numbers::pi * base_hz * 2.7 * t) +
                                   0.10 * std::sin(2 * std::numbers::pi * 61.0 * t);
            const double sample = level * window * carrier;
            output[static_cast<std::size_t>(frame) * 2] = static_cast<float>(sample);
            output[static_cast<std::size_t>(frame) * 2 + 1] = static_cast<float>(sample * (0.82 + 0.18 * loudness));
        }
        return true;
    }

  private:
    std::uint32_t seed_;
};

QColor Hsl(double hue, double saturation, double lightness) {
    return QColor::fromHslF(static_cast<float>(hue), static_cast<float>(saturation), static_cast<float>(lightness));
}

} // namespace

std::uint32_t EditSourceSeed(const std::uint8_t* identity, std::size_t size) {
    std::uint32_t seed = 2166136261U;
    for (std::size_t index = 0; index < size; ++index)
        seed = (seed ^ identity[index]) * 16777619U;
    return seed;
}

QImage PaintEditSourceFrame(std::uint32_t seed, int width, int height, std::int64_t time_us) {
    QImage image(std::max(2, width), std::max(2, height), QImage::Format_RGB32);
    const double w = image.width();
    const double h = image.height();
    const double t = static_cast<double>(time_us) / 1'000'000.0;
    const double hue = Unit(seed, 2);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    // Sky.
    QLinearGradient sky(0, 0, 0, h * 0.62);
    sky.setColorAt(0.0, Hsl(std::fmod(hue + 0.58, 1.0), 0.55, 0.22));
    sky.setColorAt(1.0, Hsl(std::fmod(hue + 0.05, 1.0), 0.65, 0.58));
    painter.fillRect(QRectF(0, 0, w, h * 0.62), sky);

    // A sun that crosses the frame over a minute.
    const double sun_x = std::fmod(0.15 + t / 60.0, 1.0) * w;
    painter.setPen(Qt::NoPen);
    painter.setBrush(Hsl(std::fmod(hue + 0.12, 1.0), 0.9, 0.78));
    painter.drawEllipse(QPointF(sun_x, h * 0.24), h * 0.07, h * 0.07);

    // Two mountain ranges, the nearer one drifting with the camera.
    for (int range = 0; range < 2; ++range) {
        QPainterPath ridge;
        const double base = h * (0.62 - 0.02 * range);
        const double drift = range == 1 ? std::fmod(t * 18.0, w) : 0.0;
        ridge.moveTo(0, base);
        for (int peak = -1; peak <= 9; ++peak) {
            const double x = peak * w / 7.0 - drift;
            const double peak_height = h * (0.12 + 0.16 * Unit(seed, 10 + range * 20 + ((peak + 70) % 11)));
            ridge.lineTo(x + w / 14.0, base - peak_height);
            ridge.lineTo(x + w / 7.0, base);
        }
        ridge.lineTo(w, h);
        ridge.lineTo(0, h);
        ridge.closeSubpath();
        painter.setBrush(Hsl(std::fmod(hue + 0.45 + 0.04 * range, 1.0), 0.28, 0.20 + 0.08 * range));
        painter.drawPath(ridge);
    }

    // Ground with lane markings that scroll.
    QLinearGradient ground(0, h * 0.62, 0, h);
    ground.setColorAt(0.0, Hsl(std::fmod(hue + 0.30, 1.0), 0.35, 0.30));
    ground.setColorAt(1.0, Hsl(std::fmod(hue + 0.30, 1.0), 0.40, 0.12));
    painter.fillRect(QRectF(0, h * 0.62, w, h * 0.38), ground);
    painter.setBrush(QColor(235, 235, 225, 180));
    const double lane = w / 6.0;
    for (int mark = -1; mark < 8; ++mark) {
        const double x = mark * lane - std::fmod(t * 240.0, lane);
        painter.drawRect(QRectF(x, h * 0.86, lane * 0.45, h * 0.012));
    }

    // The subject: a vehicle that weaves across the lane.
    const double subject_x = w * (0.5 + 0.18 * std::sin(t * 0.9 + hue * 6.0));
    painter.setBrush(Hsl(std::fmod(hue + 0.95, 1.0), 0.75, 0.52));
    painter.drawRoundedRect(QRectF(subject_x - w * 0.07, h * 0.70, w * 0.14, h * 0.09), h * 0.015, h * 0.015);
    painter.setBrush(QColor(20, 22, 28));
    painter.drawEllipse(QPointF(subject_x - w * 0.045, h * 0.795), h * 0.022, h * 0.022);
    painter.drawEllipse(QPointF(subject_x + w * 0.045, h * 0.795), h * 0.022, h * 0.022);

    // A game HUD: health bar, minimap and a score, the elements a recording of
    // a game actually carries.
    const double unit = h / 40.0;
    painter.setBrush(QColor(0, 0, 0, 120));
    painter.drawRoundedRect(QRectF(unit, unit, w * 0.24, unit * 1.6), unit * 0.4, unit * 0.4);
    painter.setBrush(QColor(88, 214, 141));
    painter.drawRoundedRect(QRectF(unit * 1.3, unit * 1.3, w * 0.24 * (0.55 + 0.4 * Unit(seed, 3)) - unit * 0.6, unit),
                            unit * 0.3, unit * 0.3);
    painter.setBrush(QColor(0, 0, 0, 140));
    const double map = h * 0.2;
    painter.drawRoundedRect(QRectF(w - map - unit, unit, map, map), unit * 0.5, unit * 0.5);
    painter.setBrush(QColor(255, 255, 255, 200));
    painter.drawEllipse(
        QPointF(w - map / 2 - unit + map * 0.3 * std::sin(t * 0.4), unit + map / 2 + map * 0.3 * std::cos(t * 0.4)),
        unit * 0.35, unit * 0.35);
    if (h >= 120) {
        QFont font(QStringLiteral("IBM Plex Mono"));
        font.setPixelSize(static_cast<int>(unit * 1.4));
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(QColor(255, 255, 255, 220));
        painter.drawText(
            QRectF(unit, unit * 3.2, w * 0.4, unit * 2),
            QStringLiteral("SCORE %1").arg(1200 + static_cast<int>(t * 37.0) + static_cast<int>(seed % 997)));
    }
    painter.end();
    return image;
}

std::unique_ptr<engine::WaveformAudioDecoder> MakeEditSourceAudio(std::uint32_t seed) {
    return std::make_unique<SyntheticAudio>(seed);
}

} // namespace exosnap::visual_tests
