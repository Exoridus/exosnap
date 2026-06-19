#include "LucideIcon.h"

#include <QByteArray>
#include <QHash>
#include <QPainter>
#include <QString>
#include <QSvgRenderer>

namespace exosnap::ui::theme {
namespace {

// Inner SVG markup for each supported Lucide icon (official lucide.dev path data,
// 24×24 viewBox). Most are stroked; a value flagged kFilled draws filled instead
// (its markup carries fill='%1' rather than stroke='%1').
//
// The wrapping <svg> (viewBox/fill:none/stroke/stroke-width/caps/joins) is added by
// the renderer below; each entry here is just the inner geometry, with "%1" where
// the tint colour is substituted.
struct LucideEntry {
    const char* inner; // inner SVG with a single "%1" colour placeholder
    bool filled;       // true => "%1" is a fill (icon is inherently filled)
};

const QHash<QString, LucideEntry>& iconTable() {
    static const QHash<QString, LucideEntry> table = {
        // bug
        {QStringLiteral("bug"),
         {"<path stroke='%1' d='m8 2 1.88 1.88'/><path stroke='%1' d='M14.12 3.88 16 2'/>"
          "<path stroke='%1' d='M9 7.13v-1a3.003 3.003 0 1 1 6 0v1'/>"
          "<path stroke='%1' d='M12 20c-3.3 0-6-2.7-6-6v-3a4 4 0 0 1 4-4h4a4 4 0 0 1 4 4v3c0 3.3-2.7 6-6 6'/>"
          "<path stroke='%1' d='M12 20v-9'/><path stroke='%1' d='M6.53 9C4.6 8.8 3 7.1 3 5'/>"
          "<path stroke='%1' d='M6 13H2'/><path stroke='%1' d='M3 21c0-2.1 1.7-3.9 3.8-4'/>"
          "<path stroke='%1' d='M20.97 5c0 2.1-1.6 3.8-3.5 4'/><path stroke='%1' d='M22 13h-4'/>"
          "<path stroke='%1' d='M17.2 17c2.1.1 3.8 1.9 3.8 4'/>",
          false}},
        // shield-check
        {QStringLiteral("shield-check"),
         {"<path stroke='%1' d='M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 "
          "1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z'/>"
          "<path stroke='%1' d='m9 12 2 2 4-4'/>",
          false}},
        // check
        {QStringLiteral("check"), {"<path stroke='%1' d='M20 6 9 17l-5-5'/>", false}},
        // x
        {QStringLiteral("x"), {"<path stroke='%1' d='M18 6 6 18'/><path stroke='%1' d='m6 6 12 12'/>", false}},
        // lock
        {QStringLiteral("lock"),
         {"<rect width='18' height='11' x='3' y='11' rx='2' ry='2' stroke='%1'/>"
          "<path stroke='%1' d='M7 11V7a5 5 0 0 1 10 0v4'/>",
          false}},
        // layers
        {QStringLiteral("layers"),
         {"<path stroke='%1' d='M12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 "
          "0l8.58-3.9a1 1 0 0 0 0-1.83z'/>"
          "<path stroke='%1' d='M2 12a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 12'/>"
          "<path stroke='%1' d='M2 17a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 17'/>",
          false}},
        // chevron-down
        {QStringLiteral("chevron-down"), {"<path stroke='%1' d='m6 9 6 6 6-6'/>", false}},
        // chevron-up
        {QStringLiteral("chevron-up"), {"<path stroke='%1' d='m18 15-6-6-6 6'/>", false}},
        // upload
        {QStringLiteral("upload"),
         {"<path stroke='%1' d='M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4'/>"
          "<path stroke='%1' d='M17 8l-5-5-5 5'/><path stroke='%1' d='M12 3v12'/>",
          false}},
        // download
        {QStringLiteral("download"),
         {"<path stroke='%1' d='M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4'/>"
          "<path stroke='%1' d='M7 10l5 5 5-5'/><path stroke='%1' d='M12 15V3'/>",
          false}},
        // refresh-cw
        {QStringLiteral("refresh-cw"),
         {"<path stroke='%1' d='M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8'/>"
          "<path stroke='%1' d='M21 3v5h-5'/>"
          "<path stroke='%1' d='M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16'/>"
          "<path stroke='%1' d='M8 16H3v5'/>",
          false}},
        // rotate-ccw
        {QStringLiteral("rotate-ccw"),
         {"<path stroke='%1' d='M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8'/>"
          "<path stroke='%1' d='M3 3v5h5'/>",
          false}},
        // alert-triangle (triangle-alert)
        {QStringLiteral("alert-triangle"),
         {"<path stroke='%1' d='m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3'/>"
          "<path stroke='%1' d='M12 9v4'/><path stroke='%1' d='M12 17h.01'/>",
          false}},
        // info
        {QStringLiteral("info"),
         {"<circle cx='12' cy='12' r='10' stroke='%1'/>"
          "<path stroke='%1' d='M12 16v-4'/><path stroke='%1' d='M12 8h.01'/>",
          false}},
        // search (magnifying glass)
        {QStringLiteral("search"),
         {"<circle cx='11' cy='11' r='8' stroke='%1'/>"
          "<path stroke='%1' d='m21 21-4.3-4.3'/>",
          false}},
        // external-link
        {QStringLiteral("external-link"),
         {"<path stroke='%1' d='M15 3h6v6'/><path stroke='%1' d='M10 14 21 3'/>"
          "<path stroke='%1' d='M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6'/>",
          false}},
        // github (inherently filled mark)
        {QStringLiteral("github"),
         {"<path fill='%1' d='M9 19c-5 1.5-5-2.5-7-3m14 6v-3.87a3.37 3.37 0 0 0-.94-2.61c3.14-.35 6.44-1.54 "
          "6.44-7A5.44 5.44 0 0 0 20 4.77 5.07 5.07 0 0 0 19.91 1S18.73.65 16 2.48a13.38 13.38 0 0 0-7 0C6.27.65 "
          "5.09 1 5.09 1A5.07 5.07 0 0 0 5 4.77a5.44 5.44 0 0 0-1.5 3.78c0 5.42 3.3 6.61 6.44 7A3.37 3.37 0 0 0 9 "
          "18.13V22'/>",
          true}},
        // folder
        {QStringLiteral("folder"),
         {"<path stroke='%1' d='M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 "
          "3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z'/>",
          false}},
        // image (photo / capture-frame)
        {QStringLiteral("image"),
         {"<path stroke='%1' d='M21 15a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7a2 2 0 0 1 2-2h4l2 3h8a2 2 0 0 1 2 2z'/>",
          false}},
        // more-horizontal (ellipsis)
        {QStringLiteral("more-horizontal"),
         {"<circle cx='12' cy='12' r='1' fill='%1' stroke='%1'/>"
          "<circle cx='19' cy='12' r='1' fill='%1' stroke='%1'/>"
          "<circle cx='5' cy='12' r='1' fill='%1' stroke='%1'/>",
          true}},
        // bell (notification)
        {QStringLiteral("bell"),
         {"<path stroke='%1' d='M6 8a6 6 0 0 1 12 0c0 7 3 9 3 9H3s3-2 3-9'/>"
          "<path stroke='%1' d='M10.3 21a1.94 1.94 0 0 0 3.4 0'/>",
          false}},
        // check-circle
        {QStringLiteral("check-circle"),
         {"<path stroke='%1' d='M22 11.08V12a10 10 0 1 1-5.93-9.14'/>"
          "<path stroke='%1' d='M22 4 12 14.01l-3-3'/>",
          false}},
        // x-circle
        {QStringLiteral("x-circle"),
         {"<circle cx='12' cy='12' r='10' stroke='%1'/>"
          "<path stroke='%1' d='m15 9-6 6'/><path stroke='%1' d='m9 9 6 6'/>",
          false}},
        // camera
        {QStringLiteral("camera"),
         {"<path stroke='%1' d='M14.5 4h-5L7 7H4a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V9a2 2 0 0 0-2-2h-3z'/>"
          "<circle cx='12' cy='13' r='3' stroke='%1'/>",
          false}},
    };
    return table;
}

} // namespace

QPixmap lucidePixmap(const QString& name, const QString& color, int size, qreal dpr) {
    if (dpr <= 0.0)
        dpr = 1.0;
    if (size <= 0)
        size = 1;

    const int phys = static_cast<int>(static_cast<qreal>(size) * dpr + 0.5);
    QPixmap pix(phys, phys);
    pix.fill(Qt::transparent);
    pix.setDevicePixelRatio(dpr);

    const auto it = iconTable().constFind(name);
    if (it == iconTable().constEnd())
        return pix; // unknown name -> transparent pixmap at the requested logical size

    // Build the wrapping SVG. Stroke set matches the canonical Lucide style and
    // TransportDock's inline approach: viewBox 0 0 24 24, fill:none, stroke-width 1.7,
    // round caps/joins. The "%1" placeholders in the inner markup are filled with the
    // tint colour (a stroke for stroked icons, a fill for inherently-filled ones).
    const QString inner = QString::fromLatin1(it->inner).arg(color);

    QByteArray svg;
    svg.reserve(inner.size() + 220);
    svg.append("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke-width='1.7' "
               "stroke-linecap='round' stroke-linejoin='round'>");
    svg.append(inner.toUtf8());
    svg.append("</svg>");

    QSvgRenderer renderer(svg);
    if (!renderer.isValid())
        return pix;

    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // Render into the logical-size rect; the painter is scaled by the pixmap's DPR.
    renderer.render(&painter, QRectF(0, 0, size, size));
    return pix;
}

} // namespace exosnap::ui::theme
