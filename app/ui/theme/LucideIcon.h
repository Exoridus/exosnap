#pragma once

#include <QIcon>
#include <QPixmap>
#include <QString>

// Shared Lucide icon helper.
//
// Renders a named Lucide (24×24, viewBox 0 0 24 24) icon into a tinted, HiDPI-crisp
// QPixmap / QIcon at any logical size. This replaces the per-call-site inline-SVG
// approach pioneered by TransportDock::makeCaptureFrameButton — same technique
// (inline SVG → QSvgRenderer → tinted QPixmap), but centralised, DPR-aware, and
// fed by official Lucide path data so every surface uses one consistent stroke set.
//
// Stroke icons use stroke=color, stroke-width 1.7, round caps/joins, fill:none.
// A handful of inherently-filled glyphs (e.g. github) carry their own fill.
//
// Unknown names return an empty/transparent pixmap of the requested logical size
// (never crash). Keep all colour values token-driven at call sites (ExoSnapPalette).
namespace exosnap::ui::theme {

// Renders the named Lucide icon at `size` logical px, tinted to `color` (any
// QColor-parseable string, e.g. an ExoSnapPalette token). The pixmap is rendered
// at size*dpr device px with its devicePixelRatio set so it stays crisp on HiDPI.
QPixmap lucidePixmap(const QString& name, const QString& color, int size, qreal dpr = 1.0);

// Convenience: the same render wrapped in a QIcon.
inline QIcon lucideIcon(const QString& name, const QString& color, int size, qreal dpr = 1.0) {
    return QIcon(lucidePixmap(name, color, size, dpr));
}

} // namespace exosnap::ui::theme
