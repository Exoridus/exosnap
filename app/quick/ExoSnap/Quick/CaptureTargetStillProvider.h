#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QSet>
#include <QString>

namespace exosnap::quick {

// Publishes the source picker's target stills to QML.
//
// The requested URL carries a generation ("<key>/<n>") because an Image whose
// source string does not change is never reloaded: without it a refreshed still
// would sit in Qt's pixmap cache and the card would show the first frame
// forever. The generation is not part of the lookup.
class CaptureTargetStillProvider final : public QQuickImageProvider {
  public:
    CaptureTargetStillProvider();

    [[nodiscard]] QImage requestImage(const QString& id, QSize* size, const QSize& requested_size) override;

    // `identity` is the picker's target identity ("window:73"); the returned
    // string is the URL QML must use for it.
    QString submitStill(const QString& identity, QImage image);
    // Drops the stills of targets that no longer exist. The picker keeps its
    // stills across an open/close cycle on purpose: reopening then shows the
    // last known image at once instead of a grid of placeholders that fills in
    // one card at a time.
    void retainOnly(const QSet<QString>& identities);

    [[nodiscard]] static QString providerId();

  private:
    mutable QMutex mutex_;
    QHash<QString, QImage> stills_;
    QHash<QString, unsigned> generations_;
};

} // namespace exosnap::quick
