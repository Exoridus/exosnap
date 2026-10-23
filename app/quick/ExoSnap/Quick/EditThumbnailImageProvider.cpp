#include "EditThumbnailImageProvider.h"

#include <QMetaObject>
#include <QStringList>

#include <mutex>

namespace exosnap::quick {

QString EditThumbnailUrl(const QString& identity_hex, qint64 source_time_us, int height) {
    return QStringLiteral("image://%1/%2/%3/%4")
        .arg(QLatin1String(kEditThumbnailProviderId), identity_hex.toLower())
        .arg(source_time_us)
        .arg(height);
}

struct EditThumbnailImageResponse::State {
    std::mutex mutex;
    QImage image;
    bool done = false;
    std::weak_ptr<ThumbnailRequestBroker> broker;
    std::uint64_t ticket = 0;
    QQuickImageResponse* receiver = nullptr;
};

EditThumbnailImageResponse::EditThumbnailImageResponse() : state_(std::make_shared<State>()) {
    state_->receiver = this;
}

EditThumbnailImageResponse::~EditThumbnailImageResponse() {
    std::shared_ptr<ThumbnailRequestBroker> broker;
    std::uint64_t ticket = 0;
    {
        std::lock_guard lock(state_->mutex);
        state_->receiver = nullptr;
        broker = state_->broker.lock();
        ticket = state_->ticket;
    }
    if (broker && ticket != 0)
        broker->Cancel(ticket);
}

QQuickTextureFactory* EditThumbnailImageResponse::textureFactory() const {
    std::lock_guard lock(state_->mutex);
    return QQuickTextureFactory::textureFactoryForImage(state_->image);
}

QString EditThumbnailImageResponse::errorString() const {
    std::lock_guard lock(state_->mutex);
    return state_->done && state_->image.isNull() ? QStringLiteral("Thumbnail unavailable") : QString();
}

void EditThumbnailImageResponse::cancel() {
    std::shared_ptr<ThumbnailRequestBroker> broker;
    std::uint64_t ticket = 0;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->done)
            return;
        broker = state_->broker.lock();
        ticket = state_->ticket;
    }
    // The broker completes the ticket with an empty image, which still emits
    // finished(): the engine needs it to release this response.
    if (broker && ticket != 0)
        broker->Cancel(ticket);
}

void EditThumbnailImageResponse::complete(const QImage& image) {
    completion()(image);
}

ThumbnailCompletion EditThumbnailImageResponse::completion() const {
    return [state = state_](const QImage& image) {
        std::lock_guard lock(state->mutex);
        if (state->done || !state->receiver)
            return;
        state->done = true;
        state->image = image;
        // Revocation and posting share the lock: destruction cannot begin
        // between checking the receiver and queuing the signal. QObject drops
        // the queued call if the response dies before delivery. Memory hits
        // also wait until the engine has connected to finished().
        QMetaObject::invokeMethod(state->receiver, &QQuickImageResponse::finished, Qt::QueuedConnection);
    };
}

void EditThumbnailImageResponse::attach(std::weak_ptr<ThumbnailRequestBroker> broker, std::uint64_t ticket) {
    std::lock_guard lock(state_->mutex);
    state_->broker = std::move(broker);
    state_->ticket = ticket;
}

EditThumbnailImageProvider::EditThumbnailImageProvider(std::shared_ptr<ThumbnailRequestBroker> broker)
    : broker_(std::move(broker)) {
}

QQuickImageResponse* EditThumbnailImageProvider::requestImageResponse(const QString& id, const QSize&) {
    auto* response = new EditThumbnailImageResponse;
    const QStringList parts = id.split(QLatin1Char('/'));
    bool time_ok = false;
    bool height_ok = false;
    const qint64 time_us = parts.size() == 3 ? parts.at(1).toLongLong(&time_ok) : 0;
    const int height = parts.size() == 3 ? parts.at(2).toInt(&height_ok) : 0;
    if (!broker_ || !time_ok || !height_ok || !ParseContentIdentity(parts.at(0))) {
        response->complete(QImage());
        return response;
    }
    const std::uint64_t ticket =
        broker_->Request(parts.at(0).toLower().toStdString(), time_us, height, response->completion());
    response->attach(broker_, ticket);
    return response;
}

} // namespace exosnap::quick
