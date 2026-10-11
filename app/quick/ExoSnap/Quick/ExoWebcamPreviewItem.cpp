#include "ExoWebcamPreviewItem.h"

#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QtQuick/qsgtexture_platform.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <exosnap/engine/gpu_compositor.h>
#include <exosnap/engine/gpu_surface_inventory.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace exosnap::quick {
namespace {

using Microsoft::WRL::ComPtr;

// Owns the GPU side on the render thread. Everything it draws comes from the
// copies handed over at the sync point, so preprocess() never touches the item.
class WebcamPreviewNode final : public QSGNode {
  public:
    WebcamPreviewNode() {
        setFlag(UsePreprocess, true);
    }

    ~WebcamPreviewNode() override {
        delete texture_wrapper_;
    }

    // Sync point, GUI thread blocked.
    void synchronize(QQuickWindow* window, const WebcamPreviewFeed& feed, quint64 revision, const QRectF& bounds) {
        window_ = window;
        if (revision != revision_) {
            revision_ = revision;
            frame_ = feed.frame();
            mirror_ = feed.mirror();
            opacity_ = feed.opacity();
            chroma_ = feed.chroma();
            dirty_ = true;
        }
        if (frame_ != nullptr && frame_->HasCompletePayload())
            ensureTarget(static_cast<UINT>(frame_->width), static_cast<UINT>(frame_->height));
        if (image_node_ != nullptr)
            image_node_->setRect(fitted(bounds));
    }

    void preprocess() override {
        if (!dirty_ || !ready_ || window_ == nullptr || frame_ == nullptr)
            return;
        std::string error;
        window_->beginExternalCommands();
        context_->ClearState();
        const bool drawn =
            compositor_.BeginFrame(background_.Get(), error) &&
            compositor_.DrawWebcam(
                *frame_, exosnap::engine::WebcamPixelRect{0, 0, static_cast<int>(width_), static_cast<int>(height_)},
                mirror_, chroma_, error, opacity_);
        window_->endExternalCommands();
        // A frame that failed to draw is not retried: the next camera frame is
        // at most one camera interval away and carries the same request.
        dirty_ = false;
        if (!drawn)
            qWarning("webcam settings preview: %s", error.c_str());
    }

  private:
    // The camera's own resolution, so the compositor samples it 1:1 and the
    // scene graph does the one scale into the item.
    void ensureTarget(UINT width, UINT height) {
        if (ready_ && width == width_ && height == height_)
            return;
        release();
        if (window_ == nullptr)
            return;
        QSGRendererInterface* renderer = window_->rendererInterface();
        if (renderer == nullptr || renderer->graphicsApi() != QSGRendererInterface::Direct3D11)
            return;
        device_ = static_cast<ID3D11Device*>(renderer->getResource(window_, QSGRendererInterface::DeviceResource));
        context_ = static_cast<ID3D11DeviceContext*>(
            renderer->getResource(window_, QSGRendererInterface::DeviceContextResource));
        if (device_ == nullptr || context_ == nullptr)
            return;

        // Transparent, so keyed and translucent camera pixels show the page.
        // The alpha blend over zero leaves premultiplied colour, which is what
        // the scene graph expects from a texture it did not create itself.
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        // Qt Quick describes a native texture it wraps as RGBA8.
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const std::vector<uint8_t> clear(static_cast<size_t>(width) * height * 4u, 0u);
        D3D11_SUBRESOURCE_DATA init{clear.data(), width * 4u, 0};
        if (FAILED(exosnap::engine::CreateTrackedTexture2D(device_.Get(), &desc, &init, background_.GetAddressOf(),
                                                           exosnap::engine::GpuSurfaceOwner::Preview))) {
            release();
            return;
        }
        std::string error;
        if (!compositor_.Init(device_.Get(), context_.Get(), width, height, error, DXGI_FORMAT_R8G8B8A8_UNORM)) {
            qWarning("webcam settings preview: %s", error.c_str());
            release();
            return;
        }
        texture_wrapper_ = QNativeInterface::QSGD3D11Texture::fromNative(
            compositor_.Result(), window_, QSize(static_cast<int>(width), static_cast<int>(height)),
            QQuickWindow::TextureHasAlphaChannel);
        if (texture_wrapper_ == nullptr) {
            release();
            return;
        }
        image_node_ = window_->createImageNode();
        image_node_->setOwnsTexture(false);
        image_node_->setFiltering(QSGTexture::Linear);
        image_node_->setTexture(texture_wrapper_);
        appendChildNode(image_node_);
        width_ = width;
        height_ = height;
        ready_ = true;
        dirty_ = true;
    }

    [[nodiscard]] QRectF fitted(const QRectF& bounds) const {
        if (width_ == 0 || height_ == 0 || bounds.isEmpty())
            return {};
        const qreal scale = std::min(bounds.width() / width_, bounds.height() / height_);
        const QSizeF size(width_ * scale, height_ * scale);
        return QRectF(bounds.center() - QPointF(size.width() / 2.0, size.height() / 2.0), size);
    }

    void release() {
        if (image_node_ != nullptr) {
            removeChildNode(image_node_);
            delete image_node_;
            image_node_ = nullptr;
        }
        delete texture_wrapper_;
        texture_wrapper_ = nullptr;
        compositor_ = exosnap::engine::GpuCompositor{};
        background_.Reset();
        context_.Reset();
        device_.Reset();
        width_ = 0;
        height_ = 0;
        ready_ = false;
    }

    QQuickWindow* window_ = nullptr;
    QSGTexture* texture_wrapper_ = nullptr;
    QSGImageNode* image_node_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> background_;
    exosnap::engine::GpuCompositor compositor_;
    UINT width_ = 0;
    UINT height_ = 0;
    bool ready_ = false;
    bool dirty_ = false;

    quint64 revision_ = 0;
    std::shared_ptr<const exosnap::engine::WebcamFrameSnapshot> frame_;
    bool mirror_ = false;
    float opacity_ = 1.0f;
    exosnap::engine::ChromaKeyParams chroma_;
};

} // namespace

ExoWebcamPreviewItem::ExoWebcamPreviewItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

ExoWebcamPreviewItem::~ExoWebcamPreviewItem() {
    if (registered_viewer_ && feed_ != nullptr)
        feed_->removeViewer();
}

WebcamPreviewFeed* ExoWebcamPreviewItem::feed() const noexcept {
    return feed_;
}

void ExoWebcamPreviewItem::setFeed(WebcamPreviewFeed* feed) {
    if (feed_ == feed)
        return;
    if (feed_ != nullptr) {
        disconnect(feed_, nullptr, this, nullptr);
        if (registered_viewer_)
            feed_->removeViewer();
        registered_viewer_ = false;
    }
    feed_ = feed;
    if (feed_ != nullptr) {
        connect(feed_, &WebcamPreviewFeed::frameChanged, this, &ExoWebcamPreviewItem::onFrameChanged);
        connect(feed_, &WebcamPreviewFeed::styleChanged, this, [this]() {
            ++revision_;
            update();
        });
    }
    updateViewerRegistration();
    onFrameChanged();
    emit feedChanged();
}

bool ExoWebcamPreviewItem::hasFrame() const noexcept {
    return has_frame_;
}

void ExoWebcamPreviewItem::onFrameChanged() {
    const bool has_frame = feed_ != nullptr && feed_->frame() != nullptr && feed_->frame()->HasCompletePayload();
    if (has_frame_ != has_frame) {
        has_frame_ = has_frame;
        emit hasFrameChanged();
    }
    ++revision_;
    update();
}

void ExoWebcamPreviewItem::updateViewerRegistration() {
    const bool wants = feed_ != nullptr && isVisible() && window() != nullptr;
    if (wants == registered_viewer_)
        return;
    registered_viewer_ = wants;
    if (wants)
        feed_->addViewer();
    else
        feed_->removeViewer();
}

void ExoWebcamPreviewItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged || change == ItemSceneChange)
        updateViewerRegistration();
}

void ExoWebcamPreviewItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
    QQuickItem::geometryChange(new_geometry, old_geometry);
    update();
}

QSGNode* ExoWebcamPreviewItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) {
    if (feed_ == nullptr || window() == nullptr) {
        delete old_node;
        return nullptr;
    }
    auto* node = static_cast<WebcamPreviewNode*>(old_node);
    if (node == nullptr)
        node = new WebcamPreviewNode;
    node->synchronize(window(), *feed_, revision_, boundingRect());
    return node;
}

} // namespace exosnap::quick
