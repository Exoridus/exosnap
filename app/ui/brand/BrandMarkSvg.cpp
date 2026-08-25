#include "ui/brand/BrandMarkSvg.h"

#include <QFile>
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>

#include <algorithm>

namespace exosnap::ui::brand {

namespace {

// Numbers go back into the SVG the way the generator writes them: short enough
// not to bloat the document, precise enough that a scaled stroke does not land
// on a visibly different weight.
[[nodiscard]] QString Number(double value) {
    return QString::number(value, 'g', 6);
}

[[nodiscard]] QString Stem(BrandMarkKind kind) {
    switch (kind) {
    case BrandMarkKind::Brand:
        return QStringLiteral("brand");
    case BrandMarkKind::Recording:
        return QStringLiteral("recording");
    case BrandMarkKind::Processing:
        return QStringLiteral("processing");
    case BrandMarkKind::Paused:
        return QStringLiteral("paused");
    case BrandMarkKind::Saved:
        return QStringLiteral("saved");
    case BrandMarkKind::Warning:
        return QStringLiteral("warning");
    case BrandMarkKind::Error:
        return QStringLiteral("error");
    case BrandMarkKind::Idle:
        break;
    }
    return QStringLiteral("idle");
}

// Every stroke in the suite, scaled. The dash pattern beside it is deliberately
// untouched: lengthening the dashes with the stroke would close the gaps the
// processing arc is made of.
void ScaleStrokes(QString& svg, double scale) {
    if (qFuzzyCompare(scale, 1.0))
        return;
    static const QRegularExpression pattern(QStringLiteral("stroke-width=\"([0-9.]+)\""));
    QString out;
    out.reserve(svg.size());
    qsizetype cursor = 0;
    QRegularExpressionMatchIterator it = pattern.globalMatch(svg);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += QStringView(svg).mid(cursor, match.capturedStart() - cursor);
        out += QStringLiteral("stroke-width=\"%1\"").arg(Number(match.captured(1).toDouble() * scale));
        cursor = match.capturedEnd();
    }
    out += QStringView(svg).mid(cursor);
    svg = out;
}

// The upright bars of the pause and processing glyphs are fills, not strokes, so
// the stroke correction never reaches them. They are widened about their own
// centre -- the height is left alone, because what a small raster loses is the
// narrow dimension.
void WidenBars(QString& svg, double scale) {
    if (qFuzzyCompare(scale, 1.0))
        return;
    static const QRegularExpression pattern(
        QStringLiteral("<rect x=\"([0-9.-]+)\" y=\"([0-9.-]+)\" width=\"([0-9.]+)\" height=\"([0-9.]+)\""
                       " rx=\"([0-9.]+)\" ry=\"([0-9.]+)\""));
    QString out;
    out.reserve(svg.size());
    qsizetype cursor = 0;
    QRegularExpressionMatchIterator it = pattern.globalMatch(svg);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const double x = match.captured(1).toDouble();
        const double width = match.captured(3).toDouble();
        const double widened = width * scale;
        const double radius = match.captured(5).toDouble() * scale;
        out += QStringView(svg).mid(cursor, match.capturedStart() - cursor);
        out += QStringLiteral("<rect x=\"%1\" y=\"%2\" width=\"%3\" height=\"%4\" rx=\"%5\" ry=\"%6\"")
                   .arg(Number(x + (width - widened) / 2.0), match.captured(2), Number(widened), match.captured(4),
                        Number(radius), Number(radius));
        cursor = match.capturedEnd();
    }
    out += QStringView(svg).mid(cursor);
    svg = out;
}

} // namespace

bool BrandMarkIsAnimated(BrandMarkKind kind) noexcept {
    return kind == BrandMarkKind::Recording || kind == BrandMarkKind::Processing;
}

QString BrandMarkAssetPath(BrandMarkKind kind, int frame) {
    const QString stem = Stem(kind);
    if (!BrandMarkIsAnimated(kind))
        return QStringLiteral(":/brand/marks/%1.svg").arg(stem);
    // Wrapped rather than clamped or refused: the frame arrives from a counter
    // that indexes a beat, and a counter that ran past the end is still asking
    // for a real frame of it.
    const int index = ((frame % kBrandMarkFrameCount) + kBrandMarkFrameCount) % kBrandMarkFrameCount;
    return QStringLiteral(":/brand/marks/%1-f%2.svg").arg(stem).arg(index);
}

QByteArray ThemedBrandMarkSvg(BrandMarkKind kind, int frame, const BrandMarkPalette& palette,
                              const OpticalProfile& profile) {
    QFile file(BrandMarkAssetPath(kind, frame));
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QString svg = QString::fromUtf8(file.readAll());

    svg.replace(QLatin1String(kReferenceAccent), palette.accent.name(QColor::HexRgb));
    svg.replace(QLatin1String(kReferenceRecording), palette.recording.name(QColor::HexRgb));
    svg.replace(QLatin1String(kReferenceCaution), palette.caution.name(QColor::HexRgb));
    svg.replace(QLatin1String(kReferenceSuccess), palette.success.name(QColor::HexRgb));

    const double opacity = std::clamp(palette.outer_opacity * profile.outer_opacity_scale, 0.0, 1.0);
    svg.replace(QStringLiteral("opacity=\"%1\"").arg(Number(kReferenceOuterOpacity)),
                QStringLiteral("opacity=\"%1\"").arg(Number(opacity)));

    ScaleStrokes(svg, profile.stroke_scale);
    WidenBars(svg, profile.stroke_scale);

    return svg.toUtf8();
}

} // namespace exosnap::ui::brand
