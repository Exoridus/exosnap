#include "ShellIconProvider.h"

#include <QDebug>

namespace exosnap::quick {

using ui::brand::ShellGlyphRequest;
using ui::brand::ShellMarkRequest;

ShellIconProvider::ShellIconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {
}

QImage ShellIconProvider::requestImage(const QString& id, QSize* size, const QSize& requested_size) {
    // requested_size is deliberately ignored. The tray icon carries no width or
    // height, so Qt asks for QSize(-1, -1) and the real size is in the id --
    // honouring an invalid request here would silently produce a 1 px icon.
    Q_UNUSED(requested_size);

    QImage image;
    ShellMarkRequest mark;
    ShellGlyphRequest glyph;
    if (ui::brand::ParseMarkImageId(id, mark)) {
        image = cache_.mark(mark);
    } else if (ui::brand::ParseGlyphImageId(id, glyph)) {
        image = cache_.glyph(glyph);
    } else {
        // A malformed id is a programming error in the caller, not a missing
        // asset, and returning a null image leaves the tray with whatever it had.
        qWarning().noquote() << "ShellIconProvider: unrecognised image id" << id;
        return {};
    }

    if (size != nullptr)
        *size = image.size();
    return image;
}

} // namespace exosnap::quick
