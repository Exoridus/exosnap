#pragma once

// UpdaterTheme.h -- the dark-default (Studio-Mint) design tokens plus the small
// vector glyphs the updater window paints (check / cross / warning / spinner /
// shield-check). Header-only so ProgressRing, StepListWidget and UpdaterWindow
// share one source of truth for colour and iconography instead of each
// re-deriving the palette. Values are the frozen `dark-default` theme from the
// design canon (themes.jsx): bg #0E0E10, accent Studio Mint #9BD9D2, status
// tints computed with the canon dark alpha set (dim 0.13, border 0.44).

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>

namespace updater_theme {

// ── Palette (dark-default) ──────────────────────────────────────────────────
inline QColor bg() {
    return QColor(0x0E, 0x0E, 0x10);
}
inline QColor surf() {
    return QColor(0x15, 0x15, 0x17);
}
inline QColor surf2() {
    return QColor(0x1C, 0x1C, 0x1F);
}
inline QColor line() {
    return QColor(255, 255, 255, 18);
} // rgba(255,255,255,0.07)
inline QColor line2() {
    return QColor(255, 255, 255, 31);
} // rgba(255,255,255,0.12)
inline QColor ink() {
    return QColor(0xF1, 0xF1, 0xEF);
}
inline QColor mut() {
    return QColor(0x9C, 0x9C, 0x9A);
}
inline QColor dim() {
    return QColor(0x65, 0x65, 0x6A);
}
inline QColor mint() {
    return QColor(0x9B, 0xD9, 0xD2);
} // accent (Studio Mint)
inline QColor mintInk() {
    return QColor(0x08, 0x13, 0x0F);
} // ink on accent
inline QColor success() {
    return QColor(0x84, 0xCB, 0xA2);
}
inline QColor caution() {
    return QColor(0xE6, 0xC5, 0x7C);
}
inline QColor error() {
    return QColor(0xE0, 0x78, 0x6C);
}
inline QColor ringTrack() {
    return QColor(255, 255, 255, 20);
} // rgba(255,255,255,0.08)

inline QColor withAlpha(QColor c, double a) {
    c.setAlphaF(static_cast<float>(a));
    return c;
}
// Status tints: dim fill = alpha 0.13, border = alpha 0.44 (canon dark set).
inline QColor statusDim(const QColor& c) {
    return withAlpha(c, 0.13);
}
inline QColor statusBorder(const QColor& c) {
    return withAlpha(c, 0.44);
}
// Accent tints: dim fill = 0.14, strong border (b2) = 0.60.
inline QColor accentDim() {
    return withAlpha(mint(), 0.14);
}
inline QColor accentBorder() {
    return withAlpha(mint(), 0.60);
}

// ── Fonts ────────────────────────────────────────────────────────────────────
// Register the bundled faces once; safe to call repeatedly.
inline void ensureFontsLoaded() {
    static bool loaded = false;
    if (loaded)
        return;
    loaded = true;
    const char* faces[] = {
        ":/updater/fonts/IBMPlexMono-Regular.ttf",    ":/updater/fonts/IBMPlexMono-Medium.ttf",
        ":/updater/fonts/HankenGrotesk-Regular.ttf",  ":/updater/fonts/HankenGrotesk-Medium.ttf",
        ":/updater/fonts/HankenGrotesk-SemiBold.ttf", ":/updater/fonts/HankenGrotesk-Bold.ttf",
    };
    for (const char* f : faces)
        QFontDatabase::addApplicationFont(QString::fromUtf8(f));
}

inline QFont ui(int px, int weight = QFont::Medium) {
    ensureFontsLoaded();
    QFont f(QStringLiteral("Hanken Grotesk"));
    f.setPixelSize(px);
    f.setWeight(static_cast<QFont::Weight>(weight));
    return f;
}
inline QFont mono(int px, int weight = QFont::Medium) {
    ensureFontsLoaded();
    QFont f(QStringLiteral("IBM Plex Mono"));
    f.setPixelSize(px);
    f.setWeight(static_cast<QFont::Weight>(weight));
    return f;
}

// ── Vector glyphs (painter draws into a square rect `r`) ─────────────────────
inline void paintCheck(QPainter& p, const QRectF& r, const QColor& c, double stroke) {
    QPen pen(c, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    path.moveTo(r.left() + r.width() * 0.24, r.top() + r.height() * 0.52);
    path.lineTo(r.left() + r.width() * 0.43, r.top() + r.height() * 0.70);
    path.lineTo(r.left() + r.width() * 0.78, r.top() + r.height() * 0.30);
    p.drawPath(path);
}

inline void paintCross(QPainter& p, const QRectF& r, const QColor& c, double stroke) {
    QPen pen(c, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    const double ix = r.width() * 0.30;
    const double iy = r.height() * 0.30;
    p.drawLine(QPointF(r.left() + ix, r.top() + iy), QPointF(r.right() - ix, r.bottom() - iy));
    p.drawLine(QPointF(r.right() - ix, r.top() + iy), QPointF(r.left() + ix, r.bottom() - iy));
}

inline void paintWarning(QPainter& p, const QRectF& r, const QColor& c, double stroke) {
    QPen pen(c, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const double cx = r.center().x();
    QPainterPath tri;
    tri.moveTo(cx, r.top() + r.height() * 0.14);
    tri.lineTo(r.left() + r.width() * 0.12, r.bottom() - r.height() * 0.16);
    tri.lineTo(r.right() - r.width() * 0.12, r.bottom() - r.height() * 0.16);
    tri.closeSubpath();
    p.drawPath(tri);
    // exclamation
    p.drawLine(QPointF(cx, r.top() + r.height() * 0.40), QPointF(cx, r.top() + r.height() * 0.62));
    p.setPen(QPen(c, stroke * 1.05, Qt::SolidLine, Qt::RoundCap));
    p.drawPoint(QPointF(cx, r.bottom() - r.height() * 0.24));
}

// A queued dot: small filled dot centred in `r`.
inline void paintDot(QPainter& p, const QRectF& r, const QColor& c, double d) {
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(r.center(), d / 2.0, d / 2.0);
}

// Spinner: track ring + a mint leading arc (a frozen ~270° sweep from the top).
inline void paintSpinner(QPainter& p, const QRectF& r, const QColor& track, const QColor& lead, double stroke,
                         int startDeg = 90, int spanDeg = -270) {
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(track, stroke, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(r, 0, 360 * 16);
    p.setPen(QPen(lead, stroke, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(r, startDeg * 16, spanDeg * 16);
}

// Refresh/retry: a circular arrow rather than a spinner, so a static action icon
// reads as "try again" and never as indefinite progress.
inline void paintRefresh(QPainter& p, const QRectF& r, const QColor& c, double stroke) {
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(c, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QRectF arc = r.adjusted(r.width() * 0.16, r.height() * 0.16, -r.width() * 0.16, -r.height() * 0.16);
    p.drawArc(arc, 35 * 16, 285 * 16);

    QPainterPath arrow;
    arrow.moveTo(r.left() + r.width() * 0.72, r.top() + r.height() * 0.10);
    arrow.lineTo(r.left() + r.width() * 0.80, r.top() + r.height() * 0.34);
    arrow.lineTo(r.left() + r.width() * 0.56, r.top() + r.height() * 0.29);
    p.drawPath(arrow);
}

// Shield with an inner check (footer "safe / keep-on" mark).
inline void paintShieldCheck(QPainter& p, const QRectF& r, const QColor& c, double stroke) {
    QPen pen(c, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const double w = r.width();
    const double h = r.height();
    QPainterPath shield;
    shield.moveTo(r.center().x(), r.top() + h * 0.04);
    shield.lineTo(r.right() - w * 0.12, r.top() + h * 0.22);
    shield.lineTo(r.right() - w * 0.12, r.top() + h * 0.52);
    shield.cubicTo(r.right() - w * 0.12, r.top() + h * 0.80, r.center().x(), r.bottom() - h * 0.02, r.center().x(),
                   r.bottom() - h * 0.02);
    shield.cubicTo(r.center().x(), r.bottom() - h * 0.02, r.left() + w * 0.12, r.top() + h * 0.80, r.left() + w * 0.12,
                   r.top() + h * 0.52);
    shield.lineTo(r.left() + w * 0.12, r.top() + h * 0.22);
    shield.closeSubpath();
    p.drawPath(shield);
    QRectF inner(r.left() + w * 0.24, r.top() + h * 0.22, w * 0.52, h * 0.52);
    paintCheck(p, inner, c, stroke);
}

} // namespace updater_theme
