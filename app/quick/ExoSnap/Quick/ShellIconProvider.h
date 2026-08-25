#pragma once

// The Qt Quick image provider the shell surfaces draw their icons from.
//
// Qt.labs.platform's SystemTrayIcon takes its icon as a URL and loads it through
// QQuickPixmap, which resolves `image://` providers -- so a rendered mark reaches
// a native tray with no temporary file and no per-frame allocation. Two
// properties of that path shape everything here:
//
//   * The provider is asked for a `requestedSize` of QSize(-1, -1), because the
//     icon has no width or height to pass on. The pixel size therefore travels
//     in the URL, and the caller is the one that knows it.
//   * QQuickPixmap caches by URL. A URL must therefore identify its rendering
//     completely; two different images behind one URL is a stale icon that
//     nothing invalidates.
//
// Both are why ui/brand/ShellIconRenderer builds the id from every input.

#include <QQuickImageProvider>
#include <QString>

#include "ui/brand/ShellIconRenderer.h"

namespace exosnap::quick {

// The provider's name in the engine. ui/brand/ShellIconRenderer owns the value
// and the URL builder, so a caller can address the provider without depending on
// Qt Quick.
using ui::brand::kShellIconProviderId;

class ShellIconProvider : public QQuickImageProvider {
  public:
    ShellIconProvider();

    // Called on Qt Quick's own loader thread. The cache behind it is guarded.
    QImage requestImage(const QString& id, QSize* size, const QSize& requested_size) override;

  private:
    ui::brand::ShellIconCache cache_;
};

} // namespace exosnap::quick
