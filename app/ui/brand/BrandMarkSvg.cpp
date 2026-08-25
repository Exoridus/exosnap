#include "ui/brand/BrandMarkSvg.h"

#include <QFile>
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>
#include <QStringList>

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
    case BrandMarkKind::Wordmark:
        return QStringLiteral("wordmark");
    case BrandMarkKind::Idle:
        break;
    }
    return QStringLiteral("idle");
}

// The two RINGS, scaled. Nothing else in the drawing is touched, and that is the
// whole point: a correction that reached the check, the cross, the warning glyph
// and the transport bars as well thickened them against a ring that had not
// moved, and the aperture's void closed up. The rings are what a small raster
// loses; the glyph inside one is what it has least room for.
//
// The dash pattern is likewise untouched: lengthening the dashes with the stroke
// would close the gaps the processing arc is made of.
void ScaleRingStrokes(QString& svg, double scale) {
    if (qFuzzyCompare(scale, 1.0))
        return;
    static const QRegularExpression pattern(QStringLiteral("(<circle\\b[^>]*?stroke-width=\")([0-9.]+)(\")"));
    QString out;
    out.reserve(svg.size());
    qsizetype cursor = 0;
    QRegularExpressionMatchIterator it = pattern.globalMatch(svg);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += QStringView(svg).mid(cursor, match.capturedStart() - cursor);
        out += match.captured(1) + Number(match.captured(2).toDouble() * scale) + match.captured(3);
        cursor = match.capturedEnd();
    }
    out += QStringView(svg).mid(cursor);
    svg = out;
}

} // namespace

bool BrandMarkIsAnimated(BrandMarkKind kind) noexcept {
    return kind == BrandMarkKind::Recording || kind == BrandMarkKind::Processing;
}

int BrandMarkFrameCount(BrandMarkKind kind) noexcept {
    switch (kind) {
    case BrandMarkKind::Recording:
        return kRecordingMarkFrameCount;
    case BrandMarkKind::Processing:
        return kProcessingMarkFrameCount;
    default:
        break;
    }
    return 1;
}

QString BrandMarkAssetPath(BrandMarkKind kind, int frame) {
    const QString stem = Stem(kind);
    if (!BrandMarkIsAnimated(kind))
        return QStringLiteral(":/brand/marks/%1.svg").arg(stem);
    // Wrapped rather than clamped or refused: the frame arrives from a counter
    // that indexes a beat, and a counter that ran past the end is still asking
    // for a real frame of it.
    const int count = BrandMarkFrameCount(kind);
    const int index = ((frame % count) + count) % count;
    return QStringLiteral(":/brand/marks/%1-f%2.svg").arg(stem).arg(index);
}

QSizeF BrandMarkViewBox(BrandMarkKind kind, int frame) {
    QFile file(BrandMarkAssetPath(kind, frame));
    if (!file.open(QIODevice::ReadOnly))
        return {1.0, 1.0};
    // The attribute rather than QSvgRenderer: the box is asked for on a QML
    // binding and by the layout that sizes the wordmark, and parsing a whole
    // document to read four numbers off its root element is the wrong order of
    // cost.
    static const QRegularExpression view_box(QStringLiteral("viewBox=\"([^\"]*)\""));
    const QRegularExpressionMatch match = view_box.match(QString::fromUtf8(file.readAll()));
    if (!match.hasMatch())
        return {1.0, 1.0};
    const QStringList parts = match.captured(1).split(QRegularExpression(QStringLiteral("[ ,]+")), Qt::SkipEmptyParts);
    if (parts.size() != 4)
        return {1.0, 1.0};
    const QSizeF box(parts.at(2).toDouble(), parts.at(3).toDouble());
    return box.width() > 0.0 && box.height() > 0.0 ? box : QSizeF{1.0, 1.0};
}

double BrandMarkAspect(BrandMarkKind kind, int frame) {
    const QSizeF box = BrandMarkViewBox(kind, frame);
    return box.width() / box.height();
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
    svg.replace(QLatin1String(kReferenceInk), palette.ink.name(QColor::HexRgb));

    const double opacity = std::clamp(palette.outer_opacity * profile.outer_opacity_scale, 0.0, 1.0);
    svg.replace(QStringLiteral("opacity=\"%1\"").arg(Number(kReferenceOuterOpacity)),
                QStringLiteral("opacity=\"%1\"").arg(Number(opacity)));

    ScaleRingStrokes(svg, profile.ring_stroke_scale);

    return svg.toUtf8();
}

} // namespace exosnap::ui::brand
