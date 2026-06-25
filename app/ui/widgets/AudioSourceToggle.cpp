#include "AudioSourceToggle.h"

#include "../theme/ExoSnapTheme.h"

#include <QByteArray>
#include <QColor>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <QSvgRenderer>

#include <algorithm>

namespace exosnap::ui::widgets {
namespace {

constexpr int kDiameter = 42;
// v10: meter strip removed — the toggle is a pure 42×42 circle.
constexpr int kTotalHeight = kDiameter; // 42

// Lucide-style 24x24 stroke paths, matching the hybrid design icon set.
QByteArray iconPathFor(const QString& key) {
    if (key == QLatin1String("system"))
        return QByteArrayLiteral("M11 5L6 9H2v6h4l5 4V5zM15.5 8.5a5 5 0 0 1 0 7M18.5 5.5a9 9 0 0 1 0 13");
    if (key == QLatin1String("mic"))
        return QByteArrayLiteral("M12 3a3 3 0 0 0-3 3v5a3 3 0 0 0 6 0V6a3 3 0 0 0-3-3zM5 11a7 7 0 0 0 14 0M12 18v3");
    if (key == QLatin1String("webcam"))
        // Camera body with lens circle — unambiguously a camera, not a person/profile.
        return QByteArrayLiteral(
            "M14.5 4h-5L7 7H4a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V9a2 2 0 0 0-2-2h-3l-2.5-3z"
            "M12 13a3 3 0 1 0 0-6 3 3 0 0 0 0 6z");
    if (key == QLatin1String("app"))
        // Application window with title-bar separator — communicates selected-app audio,
        // distinct from the 4-square grid that read as a layout/menu icon.
        return QByteArrayLiteral("M3 3h18a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2zM3 9h18");
    return {};
}

void paintIcon(QPainter& painter, const QString& key, const QRectF& bounds, const QColor& color) {
    const QByteArray path = iconPathFor(key);
    if (path.isEmpty())
        return;
    QByteArray svg;
    svg.reserve(256);
    svg.append("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='");
    svg.append(color.name(QColor::HexRgb).toUtf8());
    svg.append("' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'><path d='");
    svg.append(path);
    svg.append("'/></svg>");
    QSvgRenderer renderer(svg);
    renderer.render(&painter, bounds);
}

} // namespace

AudioSourceToggle::AudioSourceToggle(const QString& icon_key, const QString& source_key, QWidget* parent)
    : QAbstractButton(parent), icon_key_(icon_key), source_key_(source_key) {
    setObjectName(QStringLiteral("audioSourceToggle"));
    setProperty("sourceKey", source_key_);
    setProperty("toggleOn", false);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(kDiameter, kTotalHeight);
}

void AudioSourceToggle::setOn(bool on) {
    if (on_ == on)
        return;
    on_ = on;
    setProperty("toggleOn", on_);
    update();
}

void AudioSourceToggle::setInteractive(bool interactive) {
    interactive_ = interactive;
    setEnabled(interactive); // disabled => no clicked() signal
    setCursor(interactive ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
}

void AudioSourceToggle::setMeterLevel(float level01) {
    const float clamped = std::clamp(level01, 0.0f, 1.0f);
    if (qFuzzyCompare(meter_level_, clamped))
        return;
    meter_level_ = clamped;
    update();
}

void AudioSourceToggle::setMeterActive(bool active) {
    if (meter_active_ == active)
        return;
    meter_active_ = active;
    update();
}

QSize AudioSourceToggle::sizeHint() const {
    return {kDiameter, kTotalHeight};
}

QSize AudioSourceToggle::minimumSizeHint() const {
    return {kDiameter, kTotalHeight};
}

void AudioSourceToggle::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Explicit circle rect: always kDiameter×kDiameter regardless of total height.
    const QRectF circle = QRectF(1.0, 1.0, kDiameter - 2.0, kDiameter - 2.0);

    // Derive accent colour from the active theme.
    const auto& t = exosnap::ui::theme::ActiveTheme();
    const QColor accent(QString::fromUtf8(t.ac));

    QColor fill;
    QColor border;
    QColor icon;
    const QColor ink(QString::fromUtf8(t.ink));
    if (on_) {
        fill = accent;
        fill.setAlphaF(0.14f); // matches kAccentDim
        border = accent;
        border.setAlphaF(0.42f); // matches kAccentLine
        icon = accent;
    } else {
        // Derive from ink so the pill is visible on light themes
        // (ink ≈ white on dark → same as the old hard-white; ink ≈ dark on light → visible on paper).
        fill = QColor::fromRgba(qRgba(ink.red(), ink.green(), ink.blue(), static_cast<int>(0.05 * 255)));
        border = QColor(Qt::transparent);
        icon = QColor(QString::fromUtf8(t.mut));
    }

    painter.setPen(border.alpha() > 0 ? QPen(border, 1.0) : Qt::NoPen);
    painter.setBrush(fill);
    painter.drawEllipse(circle);

    // Meter fill: vertical fill from bottom, clipped to the circle shape.
    // Shown when meter is active and level > 0, regardless of on_ state.
    // OFF-state: flat grey fill (shows signal present but source not captured).
    // ON-state: zoned fill — mint 0-0.75, caution/amber 0.75-0.95, error/coral 0.95-1.0.
    if (meter_active_ && meter_level_ > 0.0f) {
        // Clip to the button circle so the fill never bleeds outside the border.
        QPainterPath clip_path;
        clip_path.addEllipse(circle);
        painter.setClipPath(clip_path);
        painter.setPen(Qt::NoPen);

        const qreal level = static_cast<qreal>(meter_level_);
        const qreal bottom = circle.bottom();

        if (!on_) {
            // OFF state: flat muted-grey fill.
            const qreal total_height = circle.height() * level;
            QColor grey(QString::fromUtf8(t.mut));
            grey.setAlphaF(0.30f);
            const QRectF fill_rect(circle.left(), bottom - total_height, circle.width(), total_height);
            painter.setBrush(grey);
            painter.drawRect(fill_rect);
        } else {
            // ON state: zoned fill bands stacked from bottom.
            // Zone thresholds (as fraction of full circle height):
            constexpr qreal kYellowThresh = 0.75; // mint below, caution above
            constexpr qreal kRedThresh = 0.95;    // caution below, error above

            const QColor caution_raw(QString::fromUtf8(t.caution));
            const QColor error_raw(QString::fromUtf8(t.error));

            // Mint zone: level 0 → min(level, 0.75)
            if (level > 0.0) {
                const qreal zone_top = std::min(level, kYellowThresh);
                const qreal zone_height = circle.height() * zone_top;
                const QRectF band(circle.left(), bottom - zone_height, circle.width(), zone_height);
                QColor c(accent);
                c.setAlphaF(0.30f);
                painter.setBrush(c);
                painter.drawRect(band);
            }
            // Caution/amber zone: level 0.75 → min(level, 0.95)
            if (level > kYellowThresh) {
                const qreal zone_bottom_frac = kYellowThresh;
                const qreal zone_top_frac = std::min(level, kRedThresh);
                const qreal zone_bottom_y = bottom - circle.height() * zone_bottom_frac;
                const qreal zone_height = circle.height() * (zone_top_frac - zone_bottom_frac);
                const QRectF band(circle.left(), zone_bottom_y - zone_height, circle.width(), zone_height);
                QColor c(caution_raw);
                c.setAlphaF(0.30f);
                painter.setBrush(c);
                painter.drawRect(band);
            }
            // Error/coral zone: level 0.95 → level
            if (level > kRedThresh) {
                const qreal zone_bottom_frac = kRedThresh;
                const qreal zone_top_frac = level;
                const qreal zone_bottom_y = bottom - circle.height() * zone_bottom_frac;
                const qreal zone_height = circle.height() * (zone_top_frac - zone_bottom_frac);
                const QRectF band(circle.left(), zone_bottom_y - zone_height, circle.width(), zone_height);
                QColor c(error_raw);
                c.setAlphaF(0.30f);
                painter.setBrush(c);
                painter.drawRect(band);
            }
        }

        // Restore unrestricted clip so the icon is not clipped.
        painter.setClipping(false);
    }

    const QRectF icon_rect =
        circle.adjusted(circle.width() * 0.27, circle.height() * 0.27, -circle.width() * 0.27, -circle.height() * 0.27);
    paintIcon(painter, icon_key_, icon_rect, icon);
}

} // namespace exosnap::ui::widgets
