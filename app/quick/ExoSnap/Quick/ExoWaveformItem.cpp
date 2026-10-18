#include "ExoWaveformItem.h"

#include "services/WaveformCacheService.h"

#include <exosnap/engine/edit_timeline.h>
#include <exosnap/engine/waveform_cache.h>

#include <QImage>
#include <QMatrix4x4>
#include <QPainter>
#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QSGTransformNode>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace exosnap::quick {
namespace {

using engine::WaveformPeak;

constexpr float kPeakAlpha = 0.55f;
constexpr float kLaneGap = 1.0f;

struct BlockNode {
    QSGTransformNode* transform = nullptr;
    std::uint32_t built_buckets = 0;
};

// Owns the per-block subtrees. Everything here is touched only on the render
// thread inside updatePaintNode.
class WaveformRootNode final : public QSGNode {
  public:
    QString key;
    int level = -1;
    int channels = 0;
    float height = 0;
    QColor color;
    std::map<std::uint64_t, BlockNode> blocks;
    QSGImageNode* image = nullptr;

    void Reset() {
        for (auto& [index, block] : blocks) {
            removeChildNode(block.transform);
            delete block.transform;
        }
        blocks.clear();
        if (image) {
            removeChildNode(image);
            delete image;
            image = nullptr;
        }
    }
};

QSGGeometryNode* MakeBandNode(const QColor& color) {
    auto* node = new QSGGeometryNode;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    auto* material = new QSGFlatColorMaterial;
    material->setColor(color);
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

struct Lane {
    float center = 0;
    float half = 0;
};

// Peak band and RMS band of one lane as step strips: four vertices per bucket
// at the bucket's start and end. Consecutive buckets meet in zero-width
// triangles, so each bucket is a flat-topped column.
void FillLane(QSGGeometryNode* peak_node, QSGGeometryNode* rms_node, const std::vector<WaveformPeak>& peaks,
              std::uint32_t buckets, int channels, int lane_channel, const Lane& lane, double bucket_samples) {
    auto* peak = peak_node->geometry();
    auto* rms = rms_node->geometry();
    peak->allocate(static_cast<int>(buckets * 4));
    rms->allocate(static_cast<int>(buckets * 4));
    auto* pv = peak->vertexDataAsPoint2D();
    auto* rv = rms->vertexDataAsPoint2D();
    for (std::uint32_t bucket = 0; bucket < buckets; ++bucket) {
        float lo = 0;
        float hi = 0;
        float level = 0;
        if (channels == 1) {
            const auto& a = peaks[bucket * engine::kWaveformChannels];
            const auto& b = peaks[bucket * engine::kWaveformChannels + 1];
            lo = engine::WaveformPeakToFloat(std::min(a.min, b.min));
            hi = engine::WaveformPeakToFloat(std::max(a.max, b.max));
            const float ra = engine::WaveformPeakToFloat(a.rms);
            const float rb = engine::WaveformPeakToFloat(b.rms);
            level = std::sqrt((ra * ra + rb * rb) / 2);
        } else {
            const auto& p = peaks[bucket * engine::kWaveformChannels + static_cast<std::uint32_t>(lane_channel)];
            lo = engine::WaveformPeakToFloat(p.min);
            hi = engine::WaveformPeakToFloat(p.max);
            level = engine::WaveformPeakToFloat(p.rms);
        }
        float top = lane.center - hi * lane.half;
        float bottom = lane.center - lo * lane.half;
        // Silence and DC still read as a one-pixel line where data exists.
        if (bottom - top < 1.0f) {
            const float middle = (top + bottom) / 2;
            top = middle - 0.5f;
            bottom = middle + 0.5f;
        }
        const float rms_top = std::max(top, lane.center - level * lane.half);
        const float rms_bottom = std::min(bottom, lane.center + level * lane.half);
        const auto x0 = static_cast<float>(bucket * bucket_samples);
        const auto x1 = static_cast<float>((bucket + 1) * bucket_samples);
        const auto at = bucket * 4;
        pv[at + 0].set(x0, top);
        pv[at + 1].set(x0, bottom);
        pv[at + 2].set(x1, top);
        pv[at + 3].set(x1, bottom);
        rv[at + 0].set(x0, rms_top);
        rv[at + 1].set(x0, std::max(rms_top, rms_bottom));
        rv[at + 2].set(x1, rms_top);
        rv[at + 3].set(x1, std::max(rms_top, rms_bottom));
    }
    peak_node->markDirty(QSGNode::DirtyGeometry);
    rms_node->markDirty(QSGNode::DirtyGeometry);
}

void PaintSoftwareLane(QPainter& painter, const std::vector<WaveformPeak>& peaks, std::uint32_t buckets, int channels,
                       int lane_channel, const Lane& lane, double first_x, double bucket_width, const QColor& color) {
    QColor peak_color = color;
    peak_color.setAlphaF(color.alphaF() * kPeakAlpha);
    for (std::uint32_t bucket = 0; bucket < buckets; ++bucket) {
        const auto& a = peaks[bucket * engine::kWaveformChannels];
        const auto& b = peaks[bucket * engine::kWaveformChannels + 1];
        const auto& channel = lane_channel == 0 ? a : b;
        const float lo = engine::WaveformPeakToFloat(channels == 1 ? std::min(a.min, b.min) : channel.min);
        const float hi = engine::WaveformPeakToFloat(channels == 1 ? std::max(a.max, b.max) : channel.max);
        const float ra = engine::WaveformPeakToFloat(a.rms);
        const float rb = engine::WaveformPeakToFloat(b.rms);
        const float rms = channels == 1 ? std::sqrt((ra * ra + rb * rb) / 2) : engine::WaveformPeakToFloat(channel.rms);
        float top = lane.center - hi * lane.half;
        float bottom = lane.center - lo * lane.half;
        if (bottom - top < 1.0f) {
            const float middle = (top + bottom) / 2;
            top = middle - 0.5f;
            bottom = middle + 0.5f;
        }
        const double x = first_x + bucket * bucket_width;
        painter.fillRect(QRectF(x, top, bucket_width, bottom - top), peak_color);
        const float rms_top = std::max(top, lane.center - rms * lane.half);
        const float rms_bottom = std::max(rms_top, std::min(bottom, lane.center + rms * lane.half));
        painter.fillRect(QRectF(x, rms_top, bucket_width, rms_bottom - rms_top), color);
    }
}

} // namespace

int WaveformLevelForSamplesPerPixel(double samples_per_pixel, int level_count) {
    if (level_count <= 0)
        return -1;
    if (!(samples_per_pixel > engine::kWaveformBaseBucketSamples))
        return 0;
    const int level = static_cast<int>(std::floor(std::log2(samples_per_pixel / engine::kWaveformBaseBucketSamples)));
    return std::clamp(level, 0, level_count - 1);
}

ExoWaveformItem::ExoWaveformItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setFlag(ItemObservesViewport, true);
    // Bucket blocks extend past the visible source window.
    setClip(true);
}

ExoWaveformItem::~ExoWaveformItem() = default;

QObject* ExoWaveformItem::service() const {
    return service_.data();
}

void ExoWaveformItem::setService(QObject* service) {
    auto* typed = qobject_cast<WaveformCacheService*>(service);
    if (typed == service_.data())
        return;
    if (service_)
        disconnect(service_.data(), nullptr, this, nullptr);
    service_ = typed;
    if (typed) {
        connect(typed, &WaveformCacheService::progress, this, [this](const QString& key) { onServiceKey(key); });
        connect(typed, &WaveformCacheService::ready, this, &ExoWaveformItem::onServiceKey);
        connect(typed, &WaveformCacheService::blocksLoaded, this, &ExoWaveformItem::onServiceKey);
    }
    emit serviceChanged();
    update();
}

void ExoWaveformItem::onServiceKey(const QString& key) {
    if (key == cache_key_)
        update();
}

void ExoWaveformItem::setCacheKey(const QString& key) {
    if (key == cache_key_)
        return;
    cache_key_ = key;
    emit cacheKeyChanged();
    update();
}

void ExoWaveformItem::setSourceInUs(qint64 value) {
    if (value == source_in_us_)
        return;
    source_in_us_ = value;
    emit sourceWindowChanged();
    update();
}

void ExoWaveformItem::setSourceOutUs(qint64 value) {
    if (value == source_out_us_)
        return;
    source_out_us_ = value;
    emit sourceWindowChanged();
    update();
}

void ExoWaveformItem::setChannelCount(int count) {
    const int bounded = std::clamp(count, 1, 2);
    if (bounded == channel_count_)
        return;
    channel_count_ = bounded;
    emit channelCountChanged();
    update();
}

void ExoWaveformItem::setColor(const QColor& color) {
    if (color == color_)
        return;
    color_ = color;
    emit colorChanged();
    update();
}

void ExoWaveformItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
    QQuickItem::geometryChange(new_geometry, old_geometry);
    if (new_geometry.size() != old_geometry.size())
        update();
}

QSGNode* ExoWaveformItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) {
    auto* root = static_cast<WaveformRootNode*>(old_node);
    const double width = this->width();
    const double height = this->height();
    const std::int64_t first_sample = engine::AudioSampleIndex(source_in_us_);
    const std::int64_t end_sample = engine::AudioSampleIndex(source_out_us_);
    if (!service_ || cache_key_.isEmpty() || width <= 0 || height <= 0 || end_sample <= first_sample ||
        first_sample < 0) {
        last_level_.store(-1, std::memory_order_relaxed);
        delete root;
        return nullptr;
    }
    const bool software_renderer =
        window() && window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
    // Attach a complete software subtree so the renderer resolves inherited
    // transforms and clipping instead of caching a newly added child's identity.
    if (software_renderer) {
        delete root;
        root = nullptr;
    }
    if (!root)
        root = new WaveformRootNode;

    QRectF visible = boundingRect().intersected(clipRect());
    if (window())
        visible = visible.intersected(mapRectFromScene(QRectF(0, 0, window()->width(), window()->height())));
    if (visible.isEmpty()) {
        root->Reset();
        return root;
    }

    const int level_count = static_cast<int>(service_->levelCount(cache_key_));
    if (level_count == 0) {
        // Primes the layout of a published waveform; blocksLoaded follows.
        (void)service_->tryReadPublishedBlock(cache_key_, 0, 0);
        root->Reset();
        last_level_.store(-1, std::memory_order_relaxed);
        return root;
    }

    const double samples_per_pixel = static_cast<double>(end_sample - first_sample) / width;
    const int level = WaveformLevelForSamplesPerPixel(samples_per_pixel, level_count);
    last_level_.store(level, std::memory_order_relaxed);
    const int channels = channel_count_;
    const auto lane_height = static_cast<float>(height);
    if (root->key != cache_key_ || root->level != level || root->channels != channels || root->height != lane_height ||
        root->color != color_) {
        root->Reset();
        root->key = cache_key_;
        root->level = level;
        root->channels = channels;
        root->height = lane_height;
        root->color = color_;
    }

    const auto bucket_samples = static_cast<double>(engine::WaveformBucketSamples(static_cast<std::uint32_t>(level)));
    const double block_samples = bucket_samples * kWaveformBlockBuckets;
    const double visible_first = first_sample + visible.left() * samples_per_pixel;
    const double visible_end = first_sample + visible.right() * samples_per_pixel;
    const auto first_block = static_cast<std::uint64_t>(visible_first / block_samples);
    const auto last_block = static_cast<std::uint64_t>(std::max(visible_first, visible_end - 1) / block_samples);

    for (auto it = root->blocks.begin(); it != root->blocks.end();) {
        if (it->first + 1 < first_block || it->first > last_block + 1) {
            root->removeChildNode(it->second.transform);
            delete it->second.transform;
            it = root->blocks.erase(it);
        } else {
            ++it;
        }
    }

    std::vector<Lane> lanes;
    if (channels == 2) {
        const float half_lane = lane_height / 4;
        lanes.push_back(Lane{half_lane, std::max(0.0f, half_lane - kLaneGap)});
        lanes.push_back(Lane{3 * half_lane, std::max(0.0f, half_lane - kLaneGap)});
    } else {
        lanes.push_back(Lane{lane_height / 2, std::max(0.0f, lane_height / 2 - kLaneGap / 2)});
    }
    if (software_renderer) {
        // The software adaptation skips custom geometry nodes. Rasterize the same
        // analyzed buckets into a supported image node instead.
        const qreal dpr = window()->effectiveDevicePixelRatio();
        QImage image(QSize(qCeil(visible.width() * dpr), qCeil(visible.height() * dpr)),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.translate(-visible.left(), -visible.top());
        for (std::uint64_t block = first_block; block <= last_block; ++block) {
            const auto data = service_->tryReadPublishedBlock(cache_key_, static_cast<std::uint32_t>(level), block);
            if (!data)
                continue;
            const auto buckets = static_cast<std::uint32_t>(data->peaks.size() / engine::kWaveformChannels);
            const double first_x = (static_cast<double>(block) * block_samples - first_sample) / samples_per_pixel;
            for (std::size_t lane = 0; lane < lanes.size(); ++lane)
                PaintSoftwareLane(painter, data->peaks, buckets, channels, static_cast<int>(lane), lanes[lane], first_x,
                                  bucket_samples / samples_per_pixel, color_);
        }
        painter.end();
        root->image = window()->createImageNode();
        root->image->setOwnsTexture(true);
        root->image->setTexture(window()->createTextureFromImage(image));
        root->image->setSourceRect(QRectF(0, 0, image.width(), image.height()));
        root->image->setRect(visible);
        // Adding to a live software scene graph immediately inspects the texture.
        root->appendChildNode(root->image);
        return root;
    }
    QColor peak_color = color_;
    peak_color.setAlphaF(color_.alphaF() * kPeakAlpha);

    for (std::uint64_t block = first_block; block <= last_block; ++block) {
        const auto data = service_->tryReadPublishedBlock(cache_key_, static_cast<std::uint32_t>(level), block);
        auto found = root->blocks.find(block);
        if (!data)
            continue;
        const auto buckets = static_cast<std::uint32_t>(data->peaks.size() / engine::kWaveformChannels);
        if (found == root->blocks.end()) {
            auto* transform = new QSGTransformNode;
            for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
                transform->appendChildNode(MakeBandNode(peak_color));
                transform->appendChildNode(MakeBandNode(color_));
            }
            root->appendChildNode(transform);
            found = root->blocks.emplace(block, BlockNode{transform, 0}).first;
        }
        if (found->second.built_buckets != buckets) {
            auto* child = found->second.transform->firstChild();
            for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
                auto* peak_node = static_cast<QSGGeometryNode*>(child);
                auto* rms_node = static_cast<QSGGeometryNode*>(child->nextSibling());
                FillLane(peak_node, rms_node, data->peaks, buckets, channels, static_cast<int>(lane), lanes[lane],
                         bucket_samples);
                child = rms_node->nextSibling();
            }
            found->second.built_buckets = buckets;
        }
    }

    // Vertices are in samples relative to their block's first sample, so only
    // this placement depends on the view: a pan moves blocks, never buckets.
    for (auto& [block, node] : root->blocks) {
        QMatrix4x4 matrix;
        matrix.translate(
            static_cast<float>((static_cast<double>(block) * block_samples - first_sample) / samples_per_pixel), 0);
        matrix.scale(static_cast<float>(1.0 / samples_per_pixel), 1);
        node.transform->setMatrix(matrix);
    }
    return root;
}

} // namespace exosnap::quick
