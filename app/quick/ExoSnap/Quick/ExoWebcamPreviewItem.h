#pragma once

#include "WebcamPreviewFeed.h"

#include <QPointer>
#include <QQuickItem>

namespace exosnap::quick {

// The camera alone, drawn on the GPU by the recording's compositor so mirror,
// opacity and chroma key look exactly as they will in the file. Keyed and
// translucent parts show what lies behind the item. Fitted into the item's
// bounds at the camera's aspect ratio.
//
// While visible on screen with a feed attached, the item counts as a viewer of
// that feed, which is what keeps the camera open for it.
class ExoWebcamPreviewItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(exosnap::quick::WebcamPreviewFeed* feed READ feed WRITE setFeed NOTIFY feedChanged FINAL)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY hasFrameChanged FINAL)

  public:
    explicit ExoWebcamPreviewItem(QQuickItem* parent = nullptr);
    ~ExoWebcamPreviewItem() override;

    [[nodiscard]] WebcamPreviewFeed* feed() const noexcept;
    void setFeed(WebcamPreviewFeed* feed);
    [[nodiscard]] bool hasFrame() const noexcept;

  signals:
    void feedChanged();
    void hasFrameChanged();

  protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;

  private:
    void updateViewerRegistration();
    void onFrameChanged();

    QPointer<WebcamPreviewFeed> feed_;
    bool registered_viewer_ = false;
    bool has_frame_ = false;
    quint64 revision_ = 1;
};

} // namespace exosnap::quick
