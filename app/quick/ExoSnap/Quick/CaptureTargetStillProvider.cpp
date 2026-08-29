#include "CaptureTargetStillProvider.h"

#include <QMutexLocker>

#include <iterator>

namespace exosnap::quick {
namespace {

// The identity's colon is not illegal in a URL path, but it reads as a scheme
// separator to every human and half the tooling that ever prints one.
QString providerKey(const QString& identity) {
    QString key = identity;
    key.replace(QLatin1Char(':'), QLatin1Char('-'));
    return key;
}

} // namespace

CaptureTargetStillProvider::CaptureTargetStillProvider() : QQuickImageProvider(QQuickImageProvider::Image) {
}

QString CaptureTargetStillProvider::providerId() {
    return QStringLiteral("capture-target");
}

QImage CaptureTargetStillProvider::requestImage(const QString& id, QSize* size, const QSize& requested_size) {
    const QString key = id.section(QLatin1Char('/'), 0, 0);
    QImage still;
    {
        QMutexLocker lock(&mutex_);
        still = stills_.value(key);
    }
    if (size != nullptr)
        *size = still.size();
    if (still.isNull() || !requested_size.isValid() || requested_size.isEmpty())
        return still;
    return still.scaled(requested_size.boundedTo(still.size()), Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QString CaptureTargetStillProvider::submitStill(const QString& identity, QImage image) {
    const QString key = providerKey(identity);
    unsigned generation = 0;
    {
        QMutexLocker lock(&mutex_);
        stills_.insert(key, std::move(image));
        generation = ++generations_[key];
    }
    return QStringLiteral("image://%1/%2/%3").arg(providerId(), key, QString::number(generation));
}

void CaptureTargetStillProvider::retainOnly(const QSet<QString>& identities) {
    QSet<QString> keys;
    keys.reserve(identities.size());
    for (const QString& identity : identities)
        keys.insert(providerKey(identity));

    QMutexLocker lock(&mutex_);
    for (auto it = stills_.begin(); it != stills_.end();)
        it = keys.contains(it.key()) ? std::next(it) : stills_.erase(it);
    // The generations of a dropped key are NOT forgotten. A window that comes
    // back would otherwise reuse generation 1, and QML would answer that URL
    // out of its pixmap cache with the pixels of the previous window.
}

} // namespace exosnap::quick
