#include "ExoPreviewItem.h"

#include "QuickPreviewRgbaConverter.h"
#include "RecordPreviewAdapter.h"
#include "RoundedRectClipGeometry.h"

#include <QMetaObject>
#include <QMutexLocker>
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGGeometry>
#include <QSGImageNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QScopeGuard>
#include <QThread>
#include <QtMath>
#include <QtQuick/qsgtexture_platform.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <recorder_core/gpu_hdr_tonemap.h>
#include <recorder_core/preview_shared_texture.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace exosnap::quick {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

qint64 nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
}

class PreviewTextureNode final : public QSGNode {
  public:
    PreviewTextureNode() {
        setFlag(UsePreprocess, true);
    }

    ~PreviewTextureNode() override {
        if (clip_node_ != nullptr) {
            removeChildNode(clip_node_);
            delete clip_node_;
            clip_node_ = nullptr;
            image_node_ = nullptr;
        }
        delete texture_wrapper_;
    }

    void adopt(ExoPreviewItem::PendingSource source, QQuickWindow* window, ExoPreviewItem* owner,
               ExoPreviewItem::Metrics& metrics, QString& error) {
        releaseGpuResources();
        window_ = window;
        owner_ = owner;
        metrics_ = &metrics;
        generation_ = source.generation;
        if (source.clear || source.handle == nullptr)
            return;

        const auto close = qScopeGuard([handle = source.handle]() { CloseHandle(static_cast<HANDLE>(handle)); });
        QSGRendererInterface* renderer = window != nullptr ? window->rendererInterface() : nullptr;
        if (renderer == nullptr || renderer->graphicsApi() != QSGRendererInterface::Direct3D11) {
            error = QStringLiteral("Qt Quick is not using the Direct3D 11 scene-graph backend.");
            return;
        }

        auto* device = static_cast<ID3D11Device*>(renderer->getResource(window, QSGRendererInterface::DeviceResource));
        if (device == nullptr) {
            error = QStringLiteral("Qt Quick did not expose its Direct3D 11 device.");
            return;
        }
        device_ = device;
        auto* device_context = static_cast<ID3D11DeviceContext*>(
            renderer->getResource(window, QSGRendererInterface::DeviceContextResource));
        if (device_context == nullptr) {
            error = QStringLiteral("Qt Quick did not expose its Direct3D 11 device context.");
            return;
        }
        context_ = device_context;

        ComPtr<ID3D11Device1> device1;
        HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(device1.GetAddressOf()));
        if (FAILED(hr)) {
            error = QStringLiteral("Qt Quick D3D11 device does not support OpenSharedResource1 (0x%1).")
                        .arg(static_cast<unsigned long>(hr), 8, 16, QLatin1Char('0'));
            return;
        }

        hr = device1->OpenSharedResource1(static_cast<HANDLE>(source.handle),
                                          IID_PPV_ARGS(shared_texture_.GetAddressOf()));
        if (FAILED(hr) || shared_texture_ == nullptr) {
            error = QStringLiteral("Opening the ExoSnap preview texture on Qt's D3D11 device failed (0x%1).")
                        .arg(static_cast<unsigned long>(hr), 8, 16, QLatin1Char('0'));
            return;
        }
        hr = shared_texture_.As(&keyed_mutex_);
        if (FAILED(hr)) {
            error = QStringLiteral("The ExoSnap preview texture has no keyed mutex (0x%1).")
                        .arg(static_cast<unsigned long>(hr), 8, 16, QLatin1Char('0'));
            return;
        }

        D3D11_TEXTURE2D_DESC desc{};
        shared_texture_->GetDesc(&desc);
        metrics.source_dxgi_format.store(static_cast<uint32_t>(desc.Format), std::memory_order_relaxed);
        width_ = desc.Width;
        height_ = desc.Height;
        tap_ = source.tap;

        D3D11_TEXTURE2D_DESC local_desc = desc;
        local_desc.MiscFlags = 0;
        local_desc.CPUAccessFlags = 0;
        local_desc.Usage = D3D11_USAGE_DEFAULT;
        local_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = device_->CreateTexture2D(&local_desc, nullptr, local_texture_.GetAddressOf());
        if (FAILED(hr)) {
            error = QStringLiteral("Allocating the Qt Quick preview copy failed (0x%1).")
                        .arg(static_cast<unsigned long>(hr), 8, 16, QLatin1Char('0'));
            return;
        }

        ID3D11Texture2D* display_texture = local_texture_.Get();
        if (tap_.transform != recorder_core::PreviewTapTransform::None) {
            if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
                error = QStringLiteral("The preview transform requires an FP16 scRGB source.");
                return;
            }
            D3D11_TEXTURE2D_DESC sdr_desc = local_desc;
            // QSGD3D11Texture::fromNative() imports generic RGBA textures and
            // internally describes them as RGBA8. The Widgets renderer samples
            // BGRA directly, but the Quick-owned display target must be RGBA8.
            sdr_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sdr_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            hr = device_->CreateTexture2D(&sdr_desc, nullptr, sdr_texture_.GetAddressOf());
            if (FAILED(hr)) {
                error = QStringLiteral("Allocating the Quick HDR tone-map target failed (0x%1).")
                            .arg(static_cast<unsigned long>(hr), 8, 16, QLatin1Char('0'));
                return;
            }
            tone_mapper_ = std::make_unique<recorder_core::HdrToneMapper>();
            std::string tone_map_error;
            const bool sdr_scrgb = tap_.transform == recorder_core::PreviewTapTransform::ScrgbSdr;
            if (!tone_mapper_->Init(device_.Get(), context_.Get(), width_, height_, tap_.peak_scale, sdr_scrgb,
                                    tone_map_error)) {
                error = QStringLiteral("Initializing the Quick HDR tone-map pass failed: %1")
                            .arg(QString::fromStdString(tone_map_error));
                return;
            }
            display_texture = sdr_texture_.Get();
        } else if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            D3D11_TEXTURE2D_DESC rgba_desc = local_desc;
            rgba_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rgba_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            hr = device_->CreateTexture2D(&rgba_desc, nullptr, sdr_texture_.GetAddressOf());
            if (FAILED(hr)) {
                error = QStringLiteral("Allocating the Quick RGBA presentation target failed (0x%1).")
                            .arg(static_cast<unsigned long>(hr), 8, 16, QLatin1Char('0'));
                return;
            }
            rgba_converter_ = std::make_unique<QuickPreviewRgbaConverter>();
            std::string conversion_error;
            if (!rgba_converter_->initialize(device_.Get(), context_.Get(), local_texture_.Get(), sdr_texture_.Get(),
                                             width_, height_, conversion_error)) {
                error = QStringLiteral("Initializing the Quick GPU format conversion failed: %1")
                            .arg(QString::fromStdString(conversion_error));
                return;
            }
            display_texture = sdr_texture_.Get();
        }

        texture_wrapper_ = QNativeInterface::QSGD3D11Texture::fromNative(
            display_texture, window, QSize(static_cast<int>(width_), static_cast<int>(height_)),
            QQuickWindow::TextureIsOpaque);
        if (texture_wrapper_ == nullptr) {
            error = QStringLiteral("Qt Quick could not wrap the D3D11 preview texture.");
            return;
        }
        image_node_ = window->createImageNode();
        if (image_node_ == nullptr) {
            error = QStringLiteral("Qt Quick could not create a scene-graph image node.");
            return;
        }
        image_node_->setOwnsTexture(false);
        image_node_->setFiltering(QSGTexture::Linear);
        image_node_->setTexture(texture_wrapper_);
        clip_node_ = new QSGClipNode;
        clip_node_->setFlag(QSGNode::OwnsGeometry, true);
        clip_node_->setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0));
        clip_node_->appendChildNode(image_node_);
        appendChildNode(clip_node_);
        valid_ = true;
    }

    bool consume(QQuickWindow* window, ExoPreviewItem::Metrics& metrics, QString& error) {
        if (!valid_ || keyed_mutex_ == nullptr || context_ == nullptr)
            return false;
        if (keyed_mutex_->AcquireSync(recorder_core::kPreviewSharedConsumerKey, 0) != S_OK) {
            metrics.mutex_misses.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const qint64 submit_start = nowNs();
        window->beginExternalCommands();
        // HdrToneMapper normally owns its immediate context in the Widgets
        // renderer. Here it shares Qt Quick's context, whose scissor/blend/depth
        // state is otherwise inherited by the offscreen conversion pass.
        context_->ClearState();
        context_->CopyResource(local_texture_.Get(), shared_texture_.Get());
        keyed_mutex_->ReleaseSync(recorder_core::kPreviewSharedProducerKey);
        bool converted = true;
        if (tone_mapper_ != nullptr) {
            std::string tone_map_error;
            if (!tone_mapper_->Convert(local_texture_.Get(), sdr_texture_.Get(), tone_map_error)) {
                error = QString::fromStdString(tone_map_error);
                converted = false;
            }
        } else if (rgba_converter_ != nullptr) {
            std::string conversion_error;
            if (!rgba_converter_->convert(conversion_error)) {
                error = QString::fromStdString(conversion_error);
                converted = false;
            }
        }
        window->endExternalCommands();
        if (!converted)
            return false;
        const qint64 consumed_at = nowNs();

        const quint64 submit_index = metrics.submit_write.fetch_add(1, std::memory_order_relaxed);
        metrics.submit_ns[submit_index % ExoPreviewItem::kMetricWindow].store(consumed_at - submit_start,
                                                                              std::memory_order_relaxed);
        const qint64 previous = metrics.last_consumed_ns.exchange(consumed_at, std::memory_order_relaxed);
        if (previous > 0) {
            const quint64 interval_index = metrics.interval_write.fetch_add(1, std::memory_order_relaxed);
            metrics.interval_ns[interval_index % ExoPreviewItem::kMetricWindow].store(consumed_at - previous,
                                                                                      std::memory_order_relaxed);
        }
        metrics.consumed_frames.fetch_add(1, std::memory_order_relaxed);
        has_frame_ = true;
        return true;
    }

    void preprocess() override {
        if (!valid_ || window_ == nullptr || owner_ == nullptr || metrics_ == nullptr || !owner_->renderLoopActive()) {
            return;
        }
        QString error;
        const bool first_frame = !has_frame_;
        const bool consumed = consume(window_, *metrics_, error);
        if (!error.isEmpty()) {
            conversion_error_active_ = true;
            owner_->publishRenderStateFromRenderThread(generation_, has_frame_, sourceSize(), error);
        }
        if (consumed && (first_frame || conversion_error_active_)) {
            if (image_node_ != nullptr)
                image_node_->setRect(target_rect_);
            conversion_error_active_ = false;
            owner_->publishRenderStateFromRenderThread(generation_, true, sourceSize(), {});
        }
        if (has_frame_)
            recordSceneFrame(*metrics_);
        // Deliberately no window_->update() here. Asking for the next render at
        // the end of this one is what made the scene graph redraw the whole
        // window at display refresh on a quiet desktop. Renders are requested by
        // ExoPreviewItem::requestSceneUpdate(), driven by the producers' per-frame
        // publish edge (PreviewUpdateScheduler), plus the ordinary QQuickItem
        // invalidations — geometry, radius, source rect, visibility, a new source
        // and scene-graph re-initialisation.
    }

    void setGeometryForRect(const QRectF& rect, qreal radius, const QRectF& normalized_source_rect) {
        if (rect == geometry_rect_ && qFuzzyCompare(radius, geometry_radius_) &&
            normalized_source_rect == normalized_source_rect_)
            return;
        geometry_rect_ = rect;
        geometry_radius_ = radius;
        normalized_source_rect_ = normalized_source_rect;
        target_rect_ = rect;
        if (image_node_ != nullptr) {
            image_node_->setRect(has_frame_ ? rect : QRectF{});
            image_node_->setSourceRect(QRectF(normalized_source_rect.x() * width_, normalized_source_rect.y() * height_,
                                              normalized_source_rect.width() * width_,
                                              normalized_source_rect.height() * height_));
        }
        updateClipGeometry(rect, radius);
    }

    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }
    [[nodiscard]] bool hasFrame() const noexcept {
        return has_frame_;
    }
    [[nodiscard]] QSize sourceSize() const {
        return QSize(static_cast<int>(width_), static_cast<int>(height_));
    }
    [[nodiscard]] quint64 generation() const noexcept {
        return generation_;
    }

  private:
    static void recordSceneFrame(ExoPreviewItem::Metrics& metrics) {
        const qint64 rendered_at = nowNs();
        const qint64 previous = metrics.last_scene_frame_ns.exchange(rendered_at, std::memory_order_relaxed);
        if (previous > 0) {
            const quint64 index = metrics.scene_interval_write.fetch_add(1, std::memory_order_relaxed);
            metrics.scene_interval_ns[index % ExoPreviewItem::kMetricWindow].store(rendered_at - previous,
                                                                                   std::memory_order_relaxed);
        }
        metrics.render_frames.fetch_add(1, std::memory_order_relaxed);
    }

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

    void releaseGpuResources() {
        valid_ = false;
        has_frame_ = false;
        // setTexture() intentionally has no null state. This helper only runs
        // while adopting into a fresh node; replacement deletes the old node
        // in updatePaintNode before constructing the new transport state.
        texture_wrapper_ = nullptr;
        rgba_converter_.reset();
        tone_mapper_.reset();
        sdr_texture_.Reset();
        local_texture_.Reset();
        keyed_mutex_.Reset();
        shared_texture_.Reset();
        context_.Reset();
        device_.Reset();
        width_ = 0;
        height_ = 0;
    }

    QSGTexture* texture_wrapper_ = nullptr;
    QSGImageNode* image_node_ = nullptr;
    QSGClipNode* clip_node_ = nullptr;
    QQuickWindow* window_ = nullptr;
    QPointer<ExoPreviewItem> owner_;
    ExoPreviewItem::Metrics* metrics_ = nullptr;
    QRectF target_rect_;
    QRectF geometry_rect_;
    qreal geometry_radius_ = -1.0;
    QRectF normalized_source_rect_{0.0, 0.0, 1.0, 1.0};

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> shared_texture_;
    ComPtr<IDXGIKeyedMutex> keyed_mutex_;
    ComPtr<ID3D11Texture2D> local_texture_;
    ComPtr<ID3D11Texture2D> sdr_texture_;
    std::unique_ptr<recorder_core::HdrToneMapper> tone_mapper_;
    std::unique_ptr<QuickPreviewRgbaConverter> rgba_converter_;
    recorder_core::PreviewTapDesc tap_{};
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    quint64 generation_ = 0;
    bool valid_ = false;
    bool has_frame_ = false;
    bool conversion_error_active_ = false;
};

} // namespace

ExoPreviewItem::ExoPreviewItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::NoButton);
}

ExoPreviewItem::~ExoPreviewItem() {
    setPreviewAdapter(nullptr);
    QMutexLocker lock(&pending_mutex_);
    closeHandle(pending_.handle);
    closeHandle(retained_.handle);
    pending_.handle = nullptr;
    retained_.handle = nullptr;
}

RecordPreviewAdapter* ExoPreviewItem::previewAdapter() const noexcept {
    return preview_adapter_.data();
}

void ExoPreviewItem::setPreviewAdapter(RecordPreviewAdapter* adapter) {
    if (preview_adapter_ == adapter)
        return;
    if (preview_adapter_ != nullptr)
        preview_adapter_->detachPreviewItem(this);
    preview_adapter_ = adapter;
    if (preview_adapter_ != nullptr)
        preview_adapter_->attachPreviewItem(this);
    emit previewAdapterChanged();
}

qreal ExoPreviewItem::cornerRadius() const noexcept {
    return corner_radius_;
}

void ExoPreviewItem::setCornerRadius(qreal radius) {
    const qreal bounded = std::max(0.0, radius);
    if (qFuzzyCompare(corner_radius_, bounded))
        return;
    corner_radius_ = bounded;
    emit cornerRadiusChanged();
    update();
}

QRectF ExoPreviewItem::normalizedSourceRect() const noexcept {
    return normalized_source_rect_;
}

void ExoPreviewItem::setNormalizedSourceRect(const QRectF& rect) {
    QRectF bounded = rect.normalized().intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    if (bounded.width() <= 0.0 || bounded.height() <= 0.0)
        bounded = QRectF(0.0, 0.0, 1.0, 1.0);
    if (normalized_source_rect_ == bounded)
        return;
    normalized_source_rect_ = bounded;
    emit normalizedSourceRectChanged();
    update();
}

bool ExoPreviewItem::frameReady() const noexcept {
    return frame_ready_;
}

QSize ExoPreviewItem::sourceSize() const {
    return source_size_;
}

const QString& ExoPreviewItem::errorText() const noexcept {
    return error_text_;
}

void ExoPreviewItem::presentSharedTexture(void* nt_handle, uint32_t width, uint32_t height,
                                          recorder_core::PreviewTapDesc tap) {
    Q_ASSERT(QThread::currentThread() == thread());
    const quint64 generation = next_generation_++;
    void* pending_handle = duplicateHandle(nt_handle);
    {
        QMutexLocker lock(&pending_mutex_);
        closeHandle(pending_.handle);
        closeHandle(retained_.handle);
        retained_ = {nt_handle, width, height, tap, generation, false};
        pending_ = {pending_handle, width, height, tap, generation, false};
    }
    active_generation_.store(generation, std::memory_order_release);
    if (pending_handle == nullptr) {
        render_loop_active_.store(false, std::memory_order_release);
        applyRenderState(false, {}, QStringLiteral("Duplicating the D3D11 preview shared handle failed."));
        return;
    }
    render_loop_active_.store(true, std::memory_order_release);
    update();
}

void ExoPreviewItem::clearSharedTexture() {
    const quint64 generation = next_generation_++;
    {
        QMutexLocker lock(&pending_mutex_);
        closeHandle(pending_.handle);
        closeHandle(retained_.handle);
        retained_ = {};
        pending_ = {nullptr, 0, 0, {}, generation, true};
    }
    active_generation_.store(generation, std::memory_order_release);
    render_loop_active_.store(false, std::memory_order_release);
    applyRenderState(false, {}, {});
    update();
}

void ExoPreviewItem::requestSceneUpdate() {
    Q_ASSERT(QThread::currentThread() == thread());
    update();
}

PreviewMetricsSnapshot ExoPreviewItem::metricsSnapshot() const {
    PreviewMetricsSnapshot snapshot;
    snapshot.render_frames = metrics_.render_frames.load(std::memory_order_relaxed);
    snapshot.consumed_frames = metrics_.consumed_frames.load(std::memory_order_relaxed);
    snapshot.mutex_misses = metrics_.mutex_misses.load(std::memory_order_relaxed);
    snapshot.source_dxgi_format = metrics_.source_dxgi_format.load(std::memory_order_relaxed);

    const quint64 interval_count =
        std::min<quint64>(metrics_.interval_write.load(std::memory_order_relaxed), kMetricWindow);
    std::vector<double> intervals;
    intervals.reserve(static_cast<size_t>(interval_count));
    for (quint64 i = 0; i < interval_count; ++i) {
        const qint64 value = metrics_.interval_ns[i].load(std::memory_order_relaxed);
        if (value > 0)
            intervals.push_back(static_cast<double>(value) / 1'000'000.0);
    }
    snapshot.source_interval_ms_p95 = percentile(intervals, 0.95);
    snapshot.source_interval_ms_p99 = percentile(intervals, 0.99);
    if (!intervals.empty()) {
        const double total_ms = std::accumulate(intervals.begin(), intervals.end(), 0.0);
        snapshot.source_delivery_fps = total_ms > 0.0 ? 1000.0 * static_cast<double>(intervals.size()) / total_ms : 0.0;
    }

    const quint64 scene_interval_count =
        std::min<quint64>(metrics_.scene_interval_write.load(std::memory_order_relaxed), kMetricWindow);
    std::vector<double> scene_intervals;
    scene_intervals.reserve(static_cast<size_t>(scene_interval_count));
    for (quint64 i = 0; i < scene_interval_count; ++i) {
        const qint64 value = metrics_.scene_interval_ns[i].load(std::memory_order_relaxed);
        if (value > 0)
            scene_intervals.push_back(static_cast<double>(value) / 1'000'000.0);
    }
    snapshot.scene_frame_ms_p50 = percentile(scene_intervals, 0.50);
    snapshot.scene_frame_ms_p95 = percentile(scene_intervals, 0.95);
    snapshot.scene_frame_ms_p99 = percentile(scene_intervals, 0.99);
    snapshot.scene_frame_ms_max =
        scene_intervals.empty() ? 0.0 : *std::max_element(scene_intervals.begin(), scene_intervals.end());
    if (!scene_intervals.empty()) {
        const double total_ms = std::accumulate(scene_intervals.begin(), scene_intervals.end(), 0.0);
        snapshot.scene_fps = total_ms > 0.0 ? 1000.0 * static_cast<double>(scene_intervals.size()) / total_ms : 0.0;
    }

    const quint64 submit_count =
        std::min<quint64>(metrics_.submit_write.load(std::memory_order_relaxed), kMetricWindow);
    std::vector<double> submits;
    submits.reserve(static_cast<size_t>(submit_count));
    for (quint64 i = 0; i < submit_count; ++i) {
        const qint64 value = metrics_.submit_ns[i].load(std::memory_order_relaxed);
        if (value > 0)
            submits.push_back(static_cast<double>(value) / 1'000.0);
    }
    snapshot.submit_us_p50 = percentile(submits, 0.50);
    snapshot.submit_us_p95 = percentile(submits, 0.95);
    snapshot.submit_us_p99 = percentile(submits, 0.99);
    return snapshot;
}

void ExoPreviewItem::resetMetrics() {
    metrics_.render_frames.store(0, std::memory_order_relaxed);
    metrics_.consumed_frames.store(0, std::memory_order_relaxed);
    metrics_.mutex_misses.store(0, std::memory_order_relaxed);
    metrics_.interval_write.store(0, std::memory_order_relaxed);
    metrics_.scene_interval_write.store(0, std::memory_order_relaxed);
    metrics_.submit_write.store(0, std::memory_order_relaxed);
    metrics_.last_consumed_ns.store(0, std::memory_order_relaxed);
    metrics_.last_scene_frame_ns.store(0, std::memory_order_relaxed);
    for (auto& sample : metrics_.interval_ns)
        sample.store(0, std::memory_order_relaxed);
    for (auto& sample : metrics_.scene_interval_ns)
        sample.store(0, std::memory_order_relaxed);
    for (auto& sample : metrics_.submit_ns)
        sample.store(0, std::memory_order_relaxed);
}

QSGNode* ExoPreviewItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) {
    auto* node = static_cast<PreviewTextureNode*>(old_node);
    const PendingSource pending = takePendingSource();
    if (pending.generation != 0 && (node == nullptr || pending.generation != node->generation())) {
        delete node;
        node = nullptr;
        if (pending.clear) {
            postRenderState(pending.generation, false, {}, {});
            return nullptr;
        }
        node = new PreviewTextureNode;
        QString error;
        node->adopt(pending, window(), this, metrics_, error);
        if (!error.isEmpty()) {
            render_loop_active_.store(false, std::memory_order_release);
            postRenderState(pending.generation, false, {}, error);
        } else {
            postRenderState(pending.generation, false, node->sourceSize(), {});
        }
    } else if (pending.handle != nullptr) {
        closeHandle(pending.handle);
    }

    if (node == nullptr)
        return nullptr;

    if (node->valid() && isVisible()) {
        node->setGeometryForRect(boundingRect(), corner_radius_, normalized_source_rect_);
    } else {
        node->setGeometryForRect({}, 0.0, normalized_source_rect_);
    }
    return node;
}

void ExoPreviewItem::itemChange(ItemChange change, const ItemChangeData& value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged) {
        render_loop_active_.store(value.boolValue, std::memory_order_release);
        if (value.boolValue)
            update();
    } else if (change == ItemSceneChange) {
        if (scene_graph_invalidated_connection_)
            disconnect(scene_graph_invalidated_connection_);
        if (scene_graph_initialized_connection_)
            disconnect(scene_graph_initialized_connection_);
        connected_window_ = value.window;
        if (value.window != nullptr) {
            scene_graph_invalidated_connection_ =
                connect(value.window, &QQuickWindow::sceneGraphInvalidated, this, [this]() {
                    render_loop_active_.store(false, std::memory_order_release);
                    const quint64 generation = active_generation_.load(std::memory_order_acquire);
                    postRenderState(generation, false, source_size_, {});
                });
            scene_graph_initialized_connection_ =
                connect(value.window, &QQuickWindow::sceneGraphInitialized, this, [this]() {
                    if (queueRetainedSource()) {
                        render_loop_active_.store(isVisible(), std::memory_order_release);
                        update();
                    }
                });
            if (queueRetainedSource())
                update();
        }
    }
}

ExoPreviewItem::PendingSource ExoPreviewItem::takePendingSource() {
    QMutexLocker lock(&pending_mutex_);
    PendingSource result = pending_;
    pending_.handle = nullptr;
    pending_.generation = 0;
    pending_.clear = false;
    return result;
}

bool ExoPreviewItem::queueRetainedSource() {
    Q_ASSERT(QThread::currentThread() == thread());
    QMutexLocker lock(&pending_mutex_);
    if (retained_.handle == nullptr || retained_.generation == 0)
        return false;
    void* handle = duplicateHandle(retained_.handle);
    if (handle == nullptr)
        return false;
    closeHandle(pending_.handle);
    pending_ = retained_;
    pending_.handle = handle;
    return true;
}

void ExoPreviewItem::applyRenderState(bool ready, const QSize& size, const QString& error) {
    if (frame_ready_ != ready) {
        frame_ready_ = ready;
        emit frameReadyChanged();
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

void ExoPreviewItem::postRenderState(quint64 generation, bool ready, QSize size, QString error) {
    QMetaObject::invokeMethod(
        this,
        [this, generation, ready, size, error = std::move(error)]() {
            if (generation != active_generation_.load(std::memory_order_acquire))
                return;
            applyRenderState(ready, size, error);
        },
        Qt::QueuedConnection);
}

void ExoPreviewItem::publishRenderStateFromRenderThread(quint64 generation, bool ready, QSize size, QString error) {
    postRenderState(generation, ready, size, std::move(error));
}

bool ExoPreviewItem::renderLoopActive() const noexcept {
    return render_loop_active_.load(std::memory_order_acquire);
}

void ExoPreviewItem::closeHandle(void* handle) noexcept {
    if (handle != nullptr)
        CloseHandle(static_cast<HANDLE>(handle));
}

void* ExoPreviewItem::duplicateHandle(void* handle) noexcept {
    if (handle == nullptr)
        return nullptr;
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(handle), GetCurrentProcess(), &duplicate, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        return nullptr;
    }
    return duplicate;
}

} // namespace exosnap::quick
