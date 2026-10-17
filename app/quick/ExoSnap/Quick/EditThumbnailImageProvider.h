#pragma once

#include "services/ThumbnailCache.h"

#include <QQuickAsyncImageProvider>
#include <QQuickImageResponse>
#include <QQuickTextureFactory>
#include <QString>

#include <cstdint>
#include <memory>

namespace exosnap::quick {

// URI scheme of per-tile timeline thumbnails:
//   image://exothumb/<content sha256 hex>/<source time us>/<height px>
// The source must be registered with EditTimelineAdapter first; an unknown
// source or an undecodable frame yields an empty image, never a placeholder.
inline constexpr char kEditThumbnailProviderId[] = "exothumb";

[[nodiscard]] QString EditThumbnailUrl(const QString& identity_hex, qint64 source_time_us, int height);

// Resolves thumbnail URLs through the shared request broker: memory hits
// complete immediately, everything else is served by the single thumbnail
// worker. A response the engine cancels (its Image went away) is withdrawn from
// the queue before it decodes.
class EditThumbnailImageProvider final : public QQuickAsyncImageProvider {
  public:
    explicit EditThumbnailImageProvider(std::shared_ptr<ThumbnailRequestBroker> broker);

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requested_size) override;

  private:
    std::shared_ptr<ThumbnailRequestBroker> broker_;
};

class EditThumbnailImageResponse final : public QQuickImageResponse {
    Q_OBJECT
  public:
    EditThumbnailImageResponse();
    ~EditThumbnailImageResponse() override;

    [[nodiscard]] QQuickTextureFactory* textureFactory() const override;
    [[nodiscard]] QString errorString() const override;
    void cancel() override;

    // Called once, from any thread.
    void complete(const QImage& image);
    [[nodiscard]] ThumbnailCompletion completion() const;
    void attach(std::weak_ptr<ThumbnailRequestBroker> broker, std::uint64_t ticket);

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace exosnap::quick
