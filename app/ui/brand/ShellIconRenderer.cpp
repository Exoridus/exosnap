#include "ui/brand/ShellIconRenderer.h"

#include "models/RecordingPulse.h"
#include "ui/brand/BrandMark.h"
#include "ui/theme/ExoSnapThemes.h"

#include <QPainter>
#include <QPainterPath>
#include <QStringList>

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

[[nodiscard]] QString IconStateToken(ShellIconState state) {
    switch (state) {
    case ShellIconState::Recording:
        return QStringLiteral("recording");
    case ShellIconState::Paused:
        return QStringLiteral("paused");
    case ShellIconState::Saved:
        return QStringLiteral("saved");
    case ShellIconState::Idle:
        break;
    }
    return QStringLiteral("idle");
}

[[nodiscard]] bool IconStateFromToken(const QString& token, ShellIconState& out) {
    if (token == QLatin1String("idle")) {
        out = ShellIconState::Idle;
        return true;
    }
    if (token == QLatin1String("recording")) {
        out = ShellIconState::Recording;
        return true;
    }
    if (token == QLatin1String("paused")) {
        out = ShellIconState::Paused;
        return true;
    }
    if (token == QLatin1String("saved")) {
        out = ShellIconState::Saved;
        return true;
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
    case ShellGlyph::Record:
        break;
    }
    return QStringLiteral("record");
}

[[nodiscard]] bool GlyphFromToken(const QString& token, ShellGlyph& out) {
    if (token == QLatin1String("record")) {
        out = ShellGlyph::Record;
        return true;
    }
    if (token == QLatin1String("pause")) {
        out = ShellGlyph::Pause;
        return true;
    }
    if (token == QLatin1String("resume")) {
        out = ShellGlyph::Resume;
        return true;
    }
    if (token == QLatin1String("stop")) {
        out = ShellGlyph::Stop;
        return true;
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

// Stroke semantics are the SVG's: the stroke is centred on the path, so a ring
// of radius r and width w spans r - w/2 to r + w/2.
void StrokeRing(QPainter& painter, double scale, double radius, double width, const QColor& colour) {
    QPen pen(colour);
    pen.setWidthF(width * scale);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const double r = radius * scale;
    painter.drawEllipse(QPointF(kCenter * scale, kCenter * scale), r, r);
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

QColor ResolveSemantic(ShellIconState state, const QString& appearance_id, const QString& accent_id) {
    const ExoAppearance& appearance = AppearanceFor(appearance_id);
    switch (state) {
    case ShellIconState::Recording:
        return QColor(QString::fromLatin1(appearance.error));
    case ShellIconState::Paused:
        return QColor(QString::fromLatin1(appearance.caution));
    case ShellIconState::Saved:
        return QColor(QString::fromLatin1(appearance.success));
    case ShellIconState::Idle:
        break;
    }
    // Idle is not a state, it is the brand: the mark carries the accent in both
    // rings, which is what makes a recording read as a change rather than as a
    // different logo.
    return ResolveAccent(appearance_id, accent_id);
}

double PulseOpacity(ShellIconState state, int pulse_frame) noexcept {
    if (state != ShellIconState::Recording)
        return 1.0;
    // 0.40 at the trough to 1.00 at the peak. The floor is not zero: an inner
    // ring that fades out entirely reads as the mark breaking rather than
    // breathing, and the outer ring is deliberately never modulated at all.
    constexpr double kFloor = 0.40;
    return kFloor + (1.0 - kFloor) * RecordingPulseIntensity(pulse_frame);
}

QString MarkImageId(const ShellMarkRequest& request) {
    return QStringLiteral("mark/%1/%2/%3/%4/%5")
        .arg(IconStateToken(request.state))
        .arg(ClampPx(request.px))
        .arg(request.state == ShellIconState::Recording ? request.pulse_frame : 0)
        .arg(AppearanceToken(request.appearance_id), AccentToken(request.accent_id));
}

QString GlyphImageId(const ShellGlyphRequest& request) {
    return QStringLiteral("glyph/%1/%2/%3/%4")
        .arg(GlyphToken(request.glyph))
        .arg(ClampPx(request.px))
        .arg(AppearanceToken(request.appearance_id), AccentToken(request.accent_id));
}

bool ParseMarkImageId(const QString& id, ShellMarkRequest& out) {
    const QStringList parts = id.split(QLatin1Char('/'));
    if (parts.size() != 6 || parts.at(0) != QLatin1String("mark"))
        return false;
    ShellMarkRequest request;
    if (!IconStateFromToken(parts.at(1), request.state))
        return false;
    bool ok = false;
    request.px = parts.at(2).toInt(&ok);
    if (!ok)
        return false;
    request.pulse_frame = parts.at(3).toInt(&ok);
    if (!ok)
        return false;
    request.appearance_id = parts.at(4);
    request.accent_id = parts.at(5);
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
    const double scale = static_cast<double>(px) / kGrid;
    const double content = kStandaloneContentScale * profile.content_scale;

    QImage image = NewCanvas(px);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QColor outer = ResolveAccent(request.appearance_id, request.accent_id);
    outer.setAlphaF(static_cast<float>(std::clamp(kOuterOpacity * profile.outer_opacity_scale, 0.0, 1.0)));

    QColor inner = ResolveSemantic(request.state, request.appearance_id, request.accent_id);
    inner.setAlphaF(static_cast<float>(std::clamp(PulseOpacity(request.state, request.pulse_frame), 0.0, 1.0)));

    StrokeRing(painter, scale, kOuterRadius * content, kOuterStroke * content * profile.outer_stroke_scale, outer);
    StrokeRing(painter, scale, kInnerRadius * content * profile.inner_radius_scale,
               kInnerStroke * content * profile.inner_stroke_scale, inner);
    FillDisc(painter, scale, kCenter, kCenter, kDotRadius * content * profile.dot_radius_scale, inner);

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
        colour = ResolveSemantic(ShellIconState::Recording, request.appearance_id, request.accent_id);
        break;
    case ShellGlyph::Pause:
        colour = ResolveSemantic(ShellIconState::Paused, request.appearance_id, request.accent_id);
        break;
    case ShellGlyph::Resume:
        colour = ResolveAccent(request.appearance_id, request.accent_id);
        break;
    }

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
