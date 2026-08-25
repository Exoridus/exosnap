#include "ui/brand/ShellIconRenderer.h"

#include "ui/brand/BrandMark.h"
#include "ui/brand/BrandMarkSvg.h"
#include "ui/theme/ExoSnapThemes.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QStringList>
#include <QSvgRenderer>

#include <algorithm>

namespace exosnap::ui::brand {

namespace {

using theme::ExoAccent;
using theme::ExoAppearance;
using theme::kDefaultAccentId;
using theme::kDefaultAppearanceId;
using theme::kExoAccents;
using theme::kExoAppearances;
using theme::ThemeKind;

// An unknown id resolves to the shipped default rather than to nothing. The
// renderer is downstream of a settings store and of a URL, and both can carry a
// value from a build that no longer exists; a tray icon that fails to paint is a
// worse answer than one painted in the default accent.
[[nodiscard]] const ExoAppearance& AppearanceFor(const QString& id) {
    for (const ExoAppearance& appearance : kExoAppearances) {
        if (id == QLatin1String(appearance.id))
            return appearance;
    }
    for (const ExoAppearance& appearance : kExoAppearances) {
        if (QLatin1String(kDefaultAppearanceId) == QLatin1String(appearance.id))
            return appearance;
    }
    return kExoAppearances.front();
}

[[nodiscard]] const ExoAccent& AccentFor(const QString& id) {
    for (const ExoAccent& accent : kExoAccents) {
        if (id == QLatin1String(accent.id))
            return accent;
    }
    for (const ExoAccent& accent : kExoAccents) {
        if (QLatin1String(kDefaultAccentId) == QLatin1String(accent.id))
            return accent;
    }
    return kExoAccents.front();
}

[[nodiscard]] QString KindToken(BrandMarkKind kind) {
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

[[nodiscard]] bool KindFromToken(const QString& token, BrandMarkKind& out) {
    static constexpr BrandMarkKind kKinds[] = {
        BrandMarkKind::Brand,  BrandMarkKind::Idle,  BrandMarkKind::Recording, BrandMarkKind::Processing,
        BrandMarkKind::Paused, BrandMarkKind::Saved, BrandMarkKind::Warning,   BrandMarkKind::Error,
    };
    for (const BrandMarkKind kind : kKinds) {
        if (token == KindToken(kind)) {
            out = kind;
            return true;
        }
    }
    return false;
}

[[nodiscard]] QString GlyphToken(ShellGlyph glyph) {
    switch (glyph) {
    case ShellGlyph::Pause:
        return QStringLiteral("pause");
    case ShellGlyph::Resume:
        return QStringLiteral("resume");
    case ShellGlyph::Stop:
        return QStringLiteral("stop");
    case ShellGlyph::Window:
        return QStringLiteral("window");
    case ShellGlyph::Folder:
        return QStringLiteral("folder");
    case ShellGlyph::Notifications:
        return QStringLiteral("notifications");
    case ShellGlyph::Quit:
        return QStringLiteral("quit");
    case ShellGlyph::Record:
        break;
    }
    return QStringLiteral("record");
}

[[nodiscard]] bool GlyphFromToken(const QString& token, ShellGlyph& out) {
    static constexpr ShellGlyph kGlyphs[] = {
        ShellGlyph::Record, ShellGlyph::Pause,         ShellGlyph::Resume, ShellGlyph::Stop,
        ShellGlyph::Window, ShellGlyph::Notifications, ShellGlyph::Folder, ShellGlyph::Quit,
    };
    for (const ShellGlyph glyph : kGlyphs) {
        if (token == GlyphToken(glyph)) {
            out = glyph;
            return true;
        }
    }
    return false;
}

// A shell raster below 8 px is not a mark and above 512 px is not a shell icon.
// Clamped rather than refused: the size arrives from a system metric, and a
// machine reporting something absurd should still get an icon.
inline constexpr int kMinPx = 8;
inline constexpr int kMaxPx = 512;

[[nodiscard]] int ClampPx(int px) noexcept {
    return std::clamp(px, kMinPx, kMaxPx);
}

[[nodiscard]] QString AppearanceToken(const QString& id) {
    return id.isEmpty() ? QString::fromLatin1(kDefaultAppearanceId) : id;
}

[[nodiscard]] QString AccentToken(const QString& id) {
    return id.isEmpty() ? QString::fromLatin1(kDefaultAccentId) : id;
}

[[nodiscard]] QImage NewCanvas(int px) {
    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    return image;
}

void FillDisc(QPainter& painter, double scale, double cx, double cy, double radius, const QColor& colour) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(colour);
    const double r = radius * scale;
    painter.drawEllipse(QPointF(cx * scale, cy * scale), r, r);
}

} // namespace

QString ShellIconImageUrl(const QString& image_id) {
    return QStringLiteral("image://%1/%2").arg(QLatin1String(kShellIconProviderId), image_id);
}

QColor ResolveAccent(const QString& appearance_id, const QString& accent_id) {
    const ExoAppearance& appearance = AppearanceFor(appearance_id);
    const ExoAccent& accent = AccentFor(accent_id);
    return QColor(QString::fromLatin1(appearance.kind == ThemeKind::Dark ? accent.dark : accent.light));
}

BrandMarkKind BrandMarkKindFor(ShellIconState state) noexcept {
    switch (state) {
    case ShellIconState::Recording:
        return BrandMarkKind::Recording;
    case ShellIconState::Processing:
        return BrandMarkKind::Processing;
    case ShellIconState::Paused:
        return BrandMarkKind::Paused;
    case ShellIconState::Saved:
        return BrandMarkKind::Saved;
    case ShellIconState::Error:
        return BrandMarkKind::Error;
    case ShellIconState::Idle:
        break;
    }
    // Idle shows the brand drawing, which is what makes every other state read
    // as a change to the mark rather than as a different logo.
    return BrandMarkKind::Idle;
}

BrandMarkPalette ResolvePalette(const QString& appearance_id, const QString& accent_id) {
    const ExoAppearance& appearance = AppearanceFor(appearance_id);
    BrandMarkPalette palette;
    palette.accent = ResolveAccent(appearance_id, accent_id);
    palette.recording = QColor(QString::fromLatin1(appearance.error));
    palette.caution = QColor(QString::fromLatin1(appearance.caution));
    palette.success = QColor(QString::fromLatin1(appearance.success));
    palette.outer_opacity = appearance.kind == ThemeKind::Dark ? kOuterOpacityDark : kOuterOpacityLight;
    return palette;
}

QString MarkImageId(const ShellMarkRequest& request) {
    return QStringLiteral("mark/%1/%2/%3/%4/%5/%6")
        .arg(KindToken(request.kind))
        .arg(ClampPx(request.px))
        // A static mark drops the frame, so a counter that kept moving after the
        // state settled cannot multiply its cache entries.
        .arg(BrandMarkIsAnimated(request.kind) ? request.frame : 0)
        .arg(request.standalone ? QStringLiteral("shell") : QStringLiteral("inline"),
             AppearanceToken(request.appearance_id), AccentToken(request.accent_id));
}

QString GlyphImageId(const ShellGlyphRequest& request) {
    return QStringLiteral("glyph/%1/%2/%3/%4")
        .arg(GlyphToken(request.glyph))
        .arg(ClampPx(request.px))
        .arg(AppearanceToken(request.appearance_id), AccentToken(request.accent_id));
}

bool ParseMarkImageId(const QString& id, ShellMarkRequest& out) {
    const QStringList parts = id.split(QLatin1Char('/'));
    if (parts.size() != 7 || parts.at(0) != QLatin1String("mark"))
        return false;
    ShellMarkRequest request;
    if (!KindFromToken(parts.at(1), request.kind))
        return false;
    bool ok = false;
    request.px = parts.at(2).toInt(&ok);
    if (!ok)
        return false;
    request.frame = parts.at(3).toInt(&ok);
    if (!ok)
        return false;
    if (parts.at(4) != QLatin1String("shell") && parts.at(4) != QLatin1String("inline"))
        return false;
    request.standalone = parts.at(4) == QLatin1String("shell");
    request.appearance_id = parts.at(5);
    request.accent_id = parts.at(6);
    out = request;
    return true;
}

bool ParseGlyphImageId(const QString& id, ShellGlyphRequest& out) {
    const QStringList parts = id.split(QLatin1Char('/'));
    if (parts.size() != 5 || parts.at(0) != QLatin1String("glyph"))
        return false;
    ShellGlyphRequest request;
    if (!GlyphFromToken(parts.at(1), request.glyph))
        return false;
    bool ok = false;
    request.px = parts.at(2).toInt(&ok);
    if (!ok)
        return false;
    request.appearance_id = parts.at(3);
    request.accent_id = parts.at(4);
    out = request;
    return true;
}

QImage RenderMark(const ShellMarkRequest& request) {
    const int px = ClampPx(request.px);
    const OpticalProfile& profile = OpticalProfileFor(px);

    const QByteArray svg = ThemedBrandMarkSvg(request.kind, request.frame,
                                              ResolvePalette(request.appearance_id, request.accent_id), profile);
    QSvgRenderer renderer(svg);
    if (!renderer.isValid())
        return {};

    QImage image = NewCanvas(px);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // The margin is reserved by shrinking the target rectangle rather than by
    // insetting the drawing: the asset is a square viewBox, so a smaller target
    // is the same mark with room around it, and no coordinate inside the asset
    // has to be touched to get one.
    const double content = request.standalone ? kStandaloneContentScale * profile.content_scale : 1.0;
    const double side = px * std::min(content, 1.0);
    const double origin = (px - side) / 2.0;
    renderer.render(&painter, QRectF(origin, origin, side, side));
    painter.end();
    return image;
}

QImage RenderGlyph(const ShellGlyphRequest& request) {
    const int px = ClampPx(request.px);
    const double scale = static_cast<double>(px) / kGrid;

    QImage image = NewCanvas(px);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // The transport glyphs carry the same semantics the marks do: record and
    // stop are the recording colour -- one before there is a recording, one
    // ending it -- pause is caution, and resume is the accent, because it is the
    // only one of the four that is an ordinary affirmative action.
    QColor colour;
    switch (request.glyph) {
    case ShellGlyph::Record:
    case ShellGlyph::Stop:
        colour = ResolvePalette(request.appearance_id, request.accent_id).recording;
        break;
    case ShellGlyph::Pause:
        colour = ResolvePalette(request.appearance_id, request.accent_id).caution;
        break;
    case ShellGlyph::Resume:
    // The four that are not transport. They say what a row is ABOUT rather than
    // what it does to a recording, so they carry the accent and no semantic.
    case ShellGlyph::Window:
    case ShellGlyph::Folder:
    case ShellGlyph::Notifications:
    case ShellGlyph::Quit:
        colour = ResolveAccent(request.appearance_id, request.accent_id);
        break;
    }

    // Outlines are stroked at a weight that survives the same 16 px the marks
    // do; the transport shapes below override this with a fill.
    QPen outline(colour);
    outline.setWidthF(kGlyphStroke * scale);
    outline.setCapStyle(Qt::RoundCap);
    outline.setJoinStyle(Qt::RoundJoin);

    painter.setPen(Qt::NoPen);
    painter.setBrush(colour);

    switch (request.glyph) {
    case ShellGlyph::Record:
        FillDisc(painter, scale, kCenter, kCenter, kGlyphDiscRadius, colour);
        break;
    case ShellGlyph::Stop:
        painter.drawRect(QRectF((kCenter - kGlyphSquareHalf) * scale, (kCenter - kGlyphSquareHalf) * scale,
                                2.0 * kGlyphSquareHalf * scale, 2.0 * kGlyphSquareHalf * scale));
        break;
    case ShellGlyph::Pause:
        for (const double direction : {-1.0, 1.0}) {
            const double x = kCenter + direction * (kGlyphBarGap / 2.0 + kGlyphBarWidth / 2.0);
            painter.drawRect(QRectF((x - kGlyphBarWidth / 2.0) * scale, (kCenter - kGlyphBarHeight / 2.0) * scale,
                                    kGlyphBarWidth * scale, kGlyphBarHeight * scale));
        }
        break;
    case ShellGlyph::Resume: {
        QPainterPath path;
        path.moveTo(kGlyphTriangleBackX * scale, (kCenter - kGlyphTriangleHalfHeight) * scale);
        path.lineTo(kGlyphTriangleBackX * scale, (kCenter + kGlyphTriangleHalfHeight) * scale);
        path.lineTo(kGlyphTriangleTipX * scale, kCenter * scale);
        path.closeSubpath();
        painter.drawPath(path);
        break;
    }

    case ShellGlyph::Window: {
        painter.setPen(outline);
        painter.setBrush(Qt::NoBrush);
        const QRectF frame((kCenter - kGlyphWindowHalfWidth) * scale, (kCenter - kGlyphWindowHalfHeight) * scale,
                           2.0 * kGlyphWindowHalfWidth * scale, 2.0 * kGlyphWindowHalfHeight * scale);
        painter.drawRoundedRect(frame, kGlyphWindowCorner * scale, kGlyphWindowCorner * scale);
        // The title band, which is what makes it a window rather than a card.
        painter.drawLine(QPointF((kCenter - kGlyphWindowHalfWidth) * scale, kGlyphWindowBandY * scale),
                         QPointF((kCenter + kGlyphWindowHalfWidth) * scale, kGlyphWindowBandY * scale));
        break;
    }

    case ShellGlyph::Folder: {
        painter.setPen(outline);
        painter.setBrush(Qt::NoBrush);
        QPainterPath path;
        const double left = kCenter - kGlyphFolderHalfWidth;
        const double right = kCenter + kGlyphFolderHalfWidth;
        path.moveTo(left * scale, kGlyphFolderBottomY * scale);
        path.lineTo(left * scale, kGlyphFolderTopY * scale);
        path.lineTo((left + kGlyphFolderTabWidth) * scale, kGlyphFolderTopY * scale);
        path.lineTo((left + kGlyphFolderTabWidth + 1.8) * scale, kGlyphFolderBodyY * scale);
        path.lineTo(right * scale, kGlyphFolderBodyY * scale);
        path.lineTo(right * scale, kGlyphFolderBottomY * scale);
        path.closeSubpath();
        painter.drawPath(path);
        break;
    }

    case ShellGlyph::Notifications: {
        painter.setPen(outline);
        painter.setBrush(Qt::NoBrush);
        QPainterPath path;
        const double left = kCenter - kGlyphBellHalfWidth;
        const double right = kCenter + kGlyphBellHalfWidth;
        // A dome that meets its lip, rather than a closed shape: at 16 px a bell
        // drawn as one outline loses the lip and reads as a balloon.
        path.moveTo(left * scale, kGlyphBellLipY * scale);
        path.lineTo(left * scale, (kGlyphBellTopY + 5.6) * scale);
        path.quadTo(kCenter * scale, (kGlyphBellTopY - 2.6) * scale, right * scale, (kGlyphBellTopY + 5.6) * scale);
        path.lineTo(right * scale, kGlyphBellLipY * scale);
        path.closeSubpath();
        painter.drawPath(path);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        FillDisc(painter, scale, kCenter, kGlyphBellClapperY, kGlyphBellClapperRadius, colour);
        break;
    }

    case ShellGlyph::Quit: {
        painter.setPen(outline);
        painter.setBrush(Qt::NoBrush);
        // The power mark: an arc with a gap at the top and a stem through it. The
        // gap is what separates it from the aperture, which is three closed
        // circles and is the last thing Quit should look like.
        const QRectF ring((kCenter - kGlyphPowerRadius) * scale, (kCenter - kGlyphPowerRadius) * scale,
                          2.0 * kGlyphPowerRadius * scale, 2.0 * kGlyphPowerRadius * scale);
        const int start = static_cast<int>((90.0 + kGlyphPowerGapDegrees / 2.0) * 16.0);
        const int span = static_cast<int>((360.0 - kGlyphPowerGapDegrees) * 16.0);
        painter.drawArc(ring, start, span);
        painter.drawLine(QPointF(kCenter * scale, kGlyphPowerStemTopY * scale),
                         QPointF(kCenter * scale, kGlyphPowerStemBottomY * scale));
        break;
    }
    }

    painter.end();
    return image;
}

QImage ShellIconCache::mark(const ShellMarkRequest& request) {
    const QString key = MarkImageId(request);
    QMutexLocker locker(&mutex_);
    auto it = images_.constFind(key);
    if (it != images_.constEnd())
        return it.value();
    // Rendered while the lock is held. The alternative -- render outside and
    // insert afterwards -- lets two loader threads paint the same key twice, and
    // the paint is measured in microseconds.
    const QImage image = RenderMark(request);
    images_.insert(key, image);
    return image;
}

QImage ShellIconCache::glyph(const ShellGlyphRequest& request) {
    const QString key = GlyphImageId(request);
    QMutexLocker locker(&mutex_);
    auto it = images_.constFind(key);
    if (it != images_.constEnd())
        return it.value();
    const QImage image = RenderGlyph(request);
    images_.insert(key, image);
    return image;
}

void ShellIconCache::clear() {
    QMutexLocker locker(&mutex_);
    images_.clear();
}

int ShellIconCache::sizeForTest() {
    QMutexLocker locker(&mutex_);
    return static_cast<int>(images_.size());
}

} // namespace exosnap::ui::brand
