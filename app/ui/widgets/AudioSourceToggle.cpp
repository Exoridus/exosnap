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
#include <cmath>

namespace exosnap::ui::widgets {
namespace {

constexpr int kDiameter = 42;
// v10: meter strip removed — the toggle is a pure 42×42 circle.
constexpr int kTotalHeight = kDiameter; // 42

// dBFS zone break points and their corresponding visual fill fractions.
// Piecewise-linear mapping gives the caution/error zones more visual real estate:
//   Mint  : -60 → -9  dBFS maps to 0.00 → 0.70  (70 % of height)
//   Amber :  -9 → -3  dBFS maps to 0.70 → 0.85  (15 % of height)
//   Coral :  -3 →  0  dBFS maps to 0.85 → 1.00  (15 % of height)
constexpr qreal kFloorDb = -60.0;
constexpr qreal kAmberDb = -9.0;
constexpr qreal kCoralDb = -3.0;
constexpr qreal kMintEnd = 0.70;
constexpr qreal kAmberEnd = 0.90;

// Lucide-style 24x24 stroke paths, matching the hybrid design icon set.
QByteArray iconPathFor(const QString& key) {
    if (key == QLatin1String("system"))
        return QByteArrayLiteral("M11 5L6 9H2v6h4l5 4V5zM15.5 8.5a5 5 0 0 1 0 7M18.5 5.5a9 9 0 0 1 0 13");
    if (key == QLatin1String("mic"))
        return QByteArrayLiteral("M12 3a3 3 0 0 0-3 3v5a3 3 0 0 0 6 0V6a3 3 0 0 0-3-3zM5 11a7 7 0 0 0 14 0M12 18v3");
    if (key == QLatin1String("webcam"))
        // A round lens on a stand: a webcam, not the photo camera that marks the
        // single-frame capture action elsewhere in the same dock.
        return QByteArrayLiteral("M12 17a7 7 0 1 0 0-14 7 7 0 0 0 0 14zM12 12.5a2.5 2.5 0 1 0 0-5 2.5 2.5 0 0 0 0 5z"
                                 "M12 17v3M7 20h10");
    if (key == QLatin1String("app"))
        // Window with a title bar and its control dots — reads as an application
        // rather than a bare frame.
        return QByteArrayLiteral("M4 5h16a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1z"
                                 "M3 9h18M7 7h.01M10 7h.01");
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

    const QRectF circle = QRectF(1.0, 1.0, kDiameter - 2.0, kDiameter - 2.0);

    const auto& t = exosnap::ui::theme::ActiveTheme();
    const QColor accent(QString::fromUtf8(t.ac));
    const QColor ink(QString::fromUtf8(t.ink));

    QColor fill, border, icon_color;
    if (on_) {
        // suite-record.jsx:108-111 SourceToggle on-state:
        //   background var(--ac-dim) = accent @ acDim(0.14); border var(--ac-b2) = accent @ acB2(0.60).
        fill = accent;
        fill.setAlphaF(0.14f);
        border = accent;
        border.setAlphaF(0.60f);
        icon_color = accent;
    } else {
        fill = QColor::fromRgba(qRgba(ink.red(), ink.green(), ink.blue(), static_cast<int>(0.05 * 255)));
        border = QColor(Qt::transparent);
        icon_color = QColor(QString::fromUtf8(t.mut));
    }

    painter.setPen(border.alpha() > 0 ? QPen(border, 1.0) : Qt::NoPen);
    painter.setBrush(fill);
    painter.drawEllipse(circle);

    // Meter fill: vertical fill from bottom, clipped to the circle shape.
    // Level is mapped from linear amplitude → dBFS → visual fill fraction so
    // that normal speech (~-18 dBFS) fills roughly 70% of the circle height.
    // Zone thresholds correspond to -9 dBFS (amber) and -3 dBFS (coral).
    if (meter_active_ && meter_level_ > 0.0f) {
        // meter_level_ is pre-mapped by dockLevel() as (dBFS + 60) / 60 → undo to get real dBFS.
        const qreal db = static_cast<qreal>(meter_level_) * 60.0 - 60.0;
        if (db < kFloorDb)
            return; // below noise gate — paint nothing

        // Piecewise-linear dBFS → visual fill fraction.
        qreal level;
        if (db <= kAmberDb)
            level = (db - kFloorDb) / (kAmberDb - kFloorDb) * kMintEnd;
        else if (db <= kCoralDb)
            level = kMintEnd + (db - kAmberDb) / (kCoralDb - kAmberDb) * (kAmberEnd - kMintEnd);
        else
            level = kAmberEnd + (db - kCoralDb) / (0.0 - kCoralDb) * (1.0 - kAmberEnd);

        QPainterPath clip_path;
        clip_path.addEllipse(circle);
        painter.setClipPath(clip_path);
        painter.setPen(Qt::NoPen);

        const qreal bottom = circle.bottom();

        if (!on_) {
            // OFF state: flat muted-grey fill (signal present but not captured).
            const qreal total_height = circle.height() * level;
            QColor grey(QString::fromUtf8(t.mut));
            grey.setAlphaF(0.30f);
            painter.setBrush(grey);
            painter.drawRect(QRectF(circle.left(), bottom - total_height, circle.width(), total_height));
        } else {
            const QColor caution_raw(QString::fromUtf8(t.caution));
            const QColor error_raw(QString::fromUtf8(t.error));

            // Mint zone: 0 → min(level, kMintEnd)
            {
                const qreal zone_top = std::min(level, kMintEnd);
                const qreal h = circle.height() * zone_top;
                QColor c(accent);
                c.setAlphaF(0.30f);
                painter.setBrush(c);
                painter.drawRect(QRectF(circle.left(), bottom - h, circle.width(), h));
            }
            // Amber zone: kMintEnd → min(level, kAmberEnd)
            if (level > kMintEnd) {
                const qreal zone_top = std::min(level, kAmberEnd);
                const qreal y_bottom = bottom - circle.height() * kMintEnd;
                const qreal h = circle.height() * (zone_top - kMintEnd);
                QColor c(caution_raw);
                c.setAlphaF(0.30f);
                painter.setBrush(c);
                painter.drawRect(QRectF(circle.left(), y_bottom - h, circle.width(), h));
            }
            // Coral zone: kAmberEnd → level
            if (level > kAmberEnd) {
                const qreal y_bottom = bottom - circle.height() * kAmberEnd;
                const qreal h = circle.height() * (level - kAmberEnd);
                QColor c(error_raw);
                c.setAlphaF(0.30f);
                painter.setBrush(c);
                painter.drawRect(QRectF(circle.left(), y_bottom - h, circle.width(), h));
            }
        }

        painter.setClipping(false);
    }

    // DESIGN-FIDELITY: glyph is round(size*0.45)=19px (hybrid-shared.jsx). For the 40px
    // circle that is inset (40-19)/2/40 = 0.2625 (was 0.27 → 18.4px).
    const QRectF icon_rect = circle.adjusted(circle.width() * 0.2625, circle.height() * 0.2625,
                                             -circle.width() * 0.2625, -circle.height() * 0.2625);
    paintIcon(painter, icon_key_, icon_rect, icon_color);
}

} // namespace exosnap::ui::widgets
