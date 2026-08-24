#include "ExoEditPlayerItem.h"

#include "EditPlayerAdapter.h"
#include "QuickPreviewRgbaConverter.h"
#include "RoundedRectClipGeometry.h"

#include <QMetaObject>
#include <QMutexLocker>
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGGeometry>
#include <QSGImageNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QtMath>
#include <QtQuick/qsgtexture_platform.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <exosnap/engine/edit_frame_gpu_converter.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace exosnap::quick {
namespace {

using Microsoft::WRL::ComPtr;

// HDR10/PQ tone-map peak scale (reference-white multiples) for a natively-HDR10
// clip. Mirrors the Widgets editor's own static fallback -- kHdrFallbackPeakNits
// (1000) / kHdrReferenceWhiteNits (80) -- since the live per-display peak query
// the Record preview has is not wired for the editor on either frontend.
constexpr float kEditorHdrPeakScaleFallback = 12.5f;

class EditPlayerTextureNode final : public QSGNode {
  public:
    ~EditPlayerTextureNode() override {
        if (clip_node_ != nullptr) {
            removeChildNode(clip_node_);
            delete clip_node_;
            clip_node_ = nullptr;
            image_node_ = nullptr;
        }
        delete texture_wrapper_;
    }

    // Binds to Qt's own D3D11 device. EditFrameGpuConverter::Init takes a
    // BORROWED device/context, so it drops straight onto the scene graph's
    // rather than creating a second one the app would then have to share across.
    bool bind(QQuickWindow* window, QString& error) {
        if (device_ != nullptr)
            return true;
        QSGRendererInterface* renderer = window != nullptr ? window->rendererInterface() : nullptr;
        if (renderer == nullptr || renderer->graphicsApi() != QSGRendererInterface::Direct3D11) {
            error = QStringLiteral("Qt Quick is not using the Direct3D 11 scene-graph backend.");
            return false;
        }
        auto* device = static_cast<ID3D11Device*>(renderer->getResource(window, QSGRendererInterface::DeviceResource));
        auto* context = static_cast<ID3D11DeviceContext*>(
            renderer->getResource(window, QSGRendererInterface::DeviceContextResource));
        if (device == nullptr || context == nullptr) {
            error = QStringLiteral("Qt Quick did not expose its Direct3D 11 device.");
            return false;
        }
        device_ = device;
        context_ = context;
        window_ = window;
        return true;
    }

    bool ensureTextures(uint32_t width, uint32_t height, QString& error) {
        if (width == 0 || height == 0) {
            error = QStringLiteral("The decoder reported a frame with no pixels.");
            return false;
        }
        if (width == width_ && height == height_ && texture_wrapper_ != nullptr)
            return true;

        releaseTextures();
        width_ = width;
        height_ = height;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        // EditFrameGpuConverter writes BGRA8 by contract; QSGD3D11Texture
        // ::fromNative imports native textures as RGBA8. One extra full-screen
        // pass (QuickPreviewRgbaConverter, already used by the Record preview)
        // bridges the two rather than forking the shared converter.
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, converted_texture_.GetAddressOf()))) {
            error = QStringLiteral("Allocating the editor colour-conversion target failed.");
            return false;
        }
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, display_texture_.GetAddressOf()))) {
            error = QStringLiteral("Allocating the editor presentation target failed.");
            return false;
        }

        frame_converter_ = std::make_unique<exosnap::engine::EditFrameGpuConverter>();
        std::string converter_error;
        if (!frame_converter_->Init(device_, context_, converter_error)) {
            error = QStringLiteral("Initializing the editor GPU colour conversion failed: %1")
                        .arg(QString::fromStdString(converter_error));
            return false;
        }
        rgba_converter_ = std::make_unique<QuickPreviewRgbaConverter>();
        std::string rgba_error;
        if (!rgba_converter_->initialize(device_, context_, converted_texture_.Get(), display_texture_.Get(), width,
                                         height, rgba_error)) {
            error = QStringLiteral("Initializing the editor RGBA presentation pass failed: %1")
                        .arg(QString::fromStdString(rgba_error));
            return false;
        }

        texture_wrapper_ = QNativeInterface::QSGD3D11Texture::fromNative(
            display_texture_.Get(), window_, QSize(static_cast<int>(width), static_cast<int>(height)),
            QQuickWindow::TextureIsOpaque);
        if (texture_wrapper_ == nullptr) {
            error = QStringLiteral("Qt Quick could not wrap the editor D3D11 texture.");
            return false;
        }
        if (image_node_ == nullptr) {
            image_node_ = window_->createImageNode();
            if (image_node_ == nullptr) {
                error = QStringLiteral("Qt Quick could not create a scene-graph image node.");
                return false;
            }
            image_node_->setOwnsTexture(false);
            image_node_->setFiltering(QSGTexture::Linear);
            clip_node_ = new QSGClipNode;
            clip_node_->setFlag(QSGNode::OwnsGeometry, true);
            clip_node_->setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0));
            clip_node_->appendChildNode(image_node_);
            appendChildNode(clip_node_);
        }
        image_node_->setTexture(texture_wrapper_);
        image_node_->setSourceRect(QRectF(0, 0, width, height));
        return true;
    }

    bool upload(const exosnap::engine::RawDecodedVideoFrame& frame, QString& error) {
        if (!ensureTextures(frame.width, frame.height, error))
            return false;

        window_->beginExternalCommands();
        // The converter normally owns an immediate context of its own; here it
        // shares Qt Quick's, whose scissor/blend/depth state would otherwise be
        // inherited by these offscreen passes.
        context_->ClearState();
        std::string converter_error;
        bool ok =
            frame_converter_->Convert(frame, converted_texture_.Get(), kEditorHdrPeakScaleFallback, converter_error);
        if (!ok)
            error = QString::fromStdString(converter_error);
        if (ok) {
            std::string rgba_error;
            ok = rgba_converter_->convert(rgba_error);
            if (!ok)
                error = QString::fromStdString(rgba_error);
        }
        window_->endExternalCommands();
        if (!ok)
            return false;
        has_frame_ = true;
        return true;
    }

    // Contain-fit: the decoded frame keeps its aspect ratio inside the item, and
    // the letterbox bands stay the player frame's own background.
    void setGeometryForRect(const QRectF& bounds, qreal radius) {
        QRectF target;
        if (has_frame_ && width_ > 0 && height_ > 0 && !bounds.isEmpty()) {
            const qreal scale =
                std::min(bounds.width() / static_cast<qreal>(width_), bounds.height() / static_cast<qreal>(height_));
            const qreal fitted_width = static_cast<qreal>(width_) * scale;
            const qreal fitted_height = static_cast<qreal>(height_) * scale;
            target = QRectF(bounds.x() + (bounds.width() - fitted_width) / 2.0,
                            bounds.y() + (bounds.height() - fitted_height) / 2.0, fitted_width, fitted_height);
        }
        if (image_node_ != nullptr)
            image_node_->setRect(target);
        updateClipGeometry(bounds, radius);
    }

    void releaseTextures() {
        texture_wrapper_ = nullptr;
        if (image_node_ != nullptr && clip_node_ != nullptr) {
            removeChildNode(clip_node_);
            delete clip_node_;
            clip_node_ = nullptr;
            image_node_ = nullptr;
        }
        rgba_converter_.reset();
        frame_converter_.reset();
        display_texture_.Reset();
        converted_texture_.Reset();
        width_ = 0;
        height_ = 0;
        has_frame_ = false;
    }

    [[nodiscard]] QSize sourceSize() const {
        return QSize(static_cast<int>(width_), static_cast<int>(height_));
    }
    [[nodiscard]] quint64 generation() const noexcept {
        return generation_;
    }
    void setGeneration(quint64 generation) noexcept {
        generation_ = generation;
    }

  private:
    void updateClipGeometry(const QRectF& rect, qreal radius) {
        if (clip_node_ == nullptr)
            return;
        const qreal bounded_radius = std::clamp(radius, 0.0, std::min(rect.width(), rect.height()) / 2.0);
        clip_node_->setClipRect(rect);
        if (bounded_radius <= 0.0 || rect.isEmpty()) {
            clip_node_->setIsRectangular(true);
            return;
        }

        BuildRoundedRectClipGeometry(clip_node_->geometry(), rect, bounded_radius);
        clip_node_->setIsRectangular(false);
        clip_node_->markDirty(QSGNode::DirtyGeometry);
    }

    QSGTexture* texture_wrapper_ = nullptr;
    QSGImageNode* image_node_ = nullptr;
    QSGClipNode* clip_node_ = nullptr;
    QQuickWindow* window_ = nullptr;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ComPtr<ID3D11Texture2D> converted_texture_;
    ComPtr<ID3D11Texture2D> display_texture_;
    std::unique_ptr<exosnap::engine::EditFrameGpuConverter> frame_converter_;
    std::unique_ptr<QuickPreviewRgbaConverter> rgba_converter_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    quint64 generation_ = 0;
    bool has_frame_ = false;
};

} // namespace

ExoEditPlayerItem::ExoEditPlayerItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::NoButton);
}

ExoEditPlayerItem::~ExoEditPlayerItem() {
    setPlayerAdapter(nullptr);
}

EditPlayerAdapter* ExoEditPlayerItem::playerAdapter() const noexcept {
    return player_adapter_.data();
}

void ExoEditPlayerItem::setPlayerAdapter(EditPlayerAdapter* adapter) {
    if (player_adapter_ == adapter)
        return;
    if (player_adapter_ != nullptr)
        player_adapter_->detachPlayerItem(this);
    player_adapter_ = adapter;
    if (player_adapter_ != nullptr)
        player_adapter_->attachPlayerItem(this);
    emit playerAdapterChanged();
}

qreal ExoEditPlayerItem::cornerRadius() const noexcept {
    return corner_radius_;
}

void ExoEditPlayerItem::setCornerRadius(qreal radius) {
    const qreal bounded = std::max(0.0, radius);
    if (qFuzzyCompare(corner_radius_, bounded))
        return;
    corner_radius_ = bounded;
    emit cornerRadiusChanged();
    update();
}

bool ExoEditPlayerItem::hasFrame() const noexcept {
    return has_frame_;
}

QSize ExoEditPlayerItem::sourceSize() const {
    return source_size_;
}

const QString& ExoEditPlayerItem::errorText() const noexcept {
    return error_text_;
}

void ExoEditPlayerItem::presentFrame(exosnap::engine::RawDecodedVideoFrame frame) {
    // Present-gate, ahead of any GPU work: the playback clock has already passed
    // this frame's own timestamp, so drawing it would show the past.
    const int64_t clock = clock_us_.load(std::memory_order_relaxed);
    if (clock >= 0 && frame.pts_us < clock)
        return;

    {
        QMutexLocker lock(&pending_mutex_);
        pending_.frame = std::move(frame);
        pending_.clear = false;
        pending_.generation = next_generation_++;
        active_generation_.store(pending_.generation, std::memory_order_release);
    }
    // update() belongs to the GUI thread; this call arrives on the decoder's.
    QMetaObject::invokeMethod(this, [this]() { update(); }, Qt::QueuedConnection);
}

void ExoEditPlayerItem::setClockUs(int64_t media_time_us) noexcept {
    clock_us_.store(media_time_us, std::memory_order_relaxed);
}

void ExoEditPlayerItem::clearFrame() {
    {
        QMutexLocker lock(&pending_mutex_);
        pending_.frame.reset();
        pending_.clear = true;
        pending_.generation = next_generation_++;
        active_generation_.store(pending_.generation, std::memory_order_release);
    }
    clock_us_.store(-1, std::memory_order_relaxed);
    applyRenderState(false, {}, {});
    QMetaObject::invokeMethod(this, [this]() { update(); }, Qt::QueuedConnection);
}

ExoEditPlayerItem::PendingFrame ExoEditPlayerItem::takePendingFrame() {
    QMutexLocker lock(&pending_mutex_);
    PendingFrame result = std::move(pending_);
    pending_.frame.reset();
    pending_.clear = false;
    pending_.generation = 0;
    return result;
}

QSGNode* ExoEditPlayerItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) {
    auto* node = static_cast<EditPlayerTextureNode*>(old_node);
    PendingFrame pending = takePendingFrame();

    if (pending.clear) {
        delete node;
        postRenderState(pending.generation, false, {}, {});
        return nullptr;
    }

    if (node == nullptr) {
        if (!pending.frame.has_value())
            return nullptr;
        node = new EditPlayerTextureNode;
    }

    if (pending.frame.has_value()) {
        node->setGeneration(pending.generation);
        QString error;
        if (!node->bind(window(), error)) {
            postRenderState(pending.generation, false, {}, error);
        } else if (!node->upload(*pending.frame, error)) {
            postRenderState(pending.generation, false, node->sourceSize(), error);
        } else {
            postRenderState(pending.generation, true, node->sourceSize(), {});
        }
    }

    node->setGeometryForRect(isVisible() ? boundingRect() : QRectF{}, corner_radius_);
    return node;
}

void ExoEditPlayerItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged) {
        if (value.boolValue)
            update();
        return;
    }
    if (change != ItemSceneChange)
        return;

    if (scene_graph_invalidated_connection_)
        disconnect(scene_graph_invalidated_connection_);
    if (scene_graph_initialized_connection_)
        disconnect(scene_graph_initialized_connection_);
    connected_window_ = value.window;
    if (value.window == nullptr)
        return;

    scene_graph_invalidated_connection_ = connect(value.window, &QQuickWindow::sceneGraphInvalidated, this, [this]() {
        // Every D3D11 resource the node held belonged to the invalidated scene
        // graph. The next decoded frame rebuilds them from scratch.
        postRenderState(active_generation_.load(std::memory_order_acquire), false, source_size_, {});
    });
    scene_graph_initialized_connection_ =
        connect(value.window, &QQuickWindow::sceneGraphInitialized, this, [this]() { update(); });
}

void ExoEditPlayerItem::applyRenderState(bool ready, const QSize& size, const QString& error) {
    if (has_frame_ != ready) {
        has_frame_ = ready;
        emit hasFrameChanged();
    }
    if (source_size_ != size) {
        source_size_ = size;
        emit sourceSizeChanged();
    }
    if (error_text_ != error) {
        error_text_ = error;
        emit errorTextChanged();
    }
}

void ExoEditPlayerItem::postRenderState(quint64 generation, bool ready, QSize size, QString error) {
    QMetaObject::invokeMethod(
        this,
        [this, generation, ready, size, error = std::move(error)]() {
            if (generation != active_generation_.load(std::memory_order_acquire))
                return;
            applyRenderState(ready, size, error);
        },
        Qt::QueuedConnection);
}

void ExoEditPlayerItem::publishRenderStateFromRenderThread(quint64 generation, bool ready, QSize size, QString error) {
    postRenderState(generation, ready, size, std::move(error));
}

} // namespace exosnap::quick
