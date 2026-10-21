#include "ExoWaveformItem.h"

#include "services/WaveformCacheService.h"

#include <exosnap/engine/audio_track_model.h>
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
#include <QTimer>
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
constexpr std::int64_t kPcmWindowSamples = 4096;
constexpr double kMaxVisiblePcmSamples = 8192;

QRectF VisibleRect(QQuickItem& item) {
    auto visible = item.boundingRect().intersected(item.clipRect());
    for (auto* ancestor = item.parentItem(); ancestor; ancestor = ancestor->parentItem())
        if (ancestor->clip())
            visible = visible.intersected(item.mapRectFromItem(ancestor, ancestor->boundingRect()));
    if (item.window())
        visible =
            visible.intersected(item.mapRectFromScene(QRectF(0, 0, item.window()->width(), item.window()->height())));
    return visible;
}

struct BlockNode {
    QSGTransformNode* transform = nullptr;
    std::uint32_t built_buckets = 0;
};

struct PcmNode {
    QSGTransformNode* transform = nullptr;
    std::weak_ptr<const WaveformCacheService::PcmWindow> data;
    std::weak_ptr<const WaveformCacheService::PcmWindow> successor;
};

// Owns the per-block subtrees. Everything here is touched only on the render
// thread inside updatePaintNode.
class WaveformRootNode final : public QSGNode {
  public:
    QString key;
    int level = -1;
    int channels = 0;
    float height = 0;
    float gain = 1;
    QColor color;
    std::map<std::uint64_t, BlockNode> blocks;
    std::map<std::int64_t, PcmNode> pcm;
    QSGImageNode* image = nullptr;

    void Reset() {
        for (auto& [index, block] : blocks) {
            removeChildNode(block.transform);
            delete block.transform;
        }
        blocks.clear();
        for (auto& [sample, node] : pcm) {
            removeChildNode(node.transform);
            delete node.transform;
        }
        pcm.clear();
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

std::vector<QLineF> PcmLines(const WaveformCacheService::PcmWindow& data,
                             const WaveformCacheService::PcmWindow* successor, int channel, const Lane& lane,
                             float gain) {
    std::vector<QLineF> lines;
    const auto end = data.first_sample + static_cast<std::int64_t>(data.samples.size() / 2);
    for (const auto& span : data.coverage) {
        const auto begin = std::max(data.first_sample, span.first);
        const auto limit = std::min(end, span.end);
        for (auto sample = begin; sample < limit; ++sample) {
            const auto at = static_cast<std::size_t>(sample - data.first_sample);
            float next = data.samples[(sample + 1 < limit ? at + 1 : at) * 2 + channel];
            if (sample + 1 == end && limit == end && successor && successor->first_sample == end &&
                std::any_of(successor->coverage.begin(), successor->coverage.end(),
                            [end](const auto& covered) { return covered.first <= end && covered.end > end; }))
                next = successor->samples[channel];
            lines.emplace_back(static_cast<double>(at),
                               lane.center - std::clamp(data.samples[at * 2 + channel] * gain, -1.0f, 1.0f) * lane.half,
                               static_cast<double>(at + 1),
                               lane.center - std::clamp(next * gain, -1.0f, 1.0f) * lane.half);
        }
    }
    return lines;
}

// Peak band and RMS band of one lane as step strips: four vertices per bucket
// at the bucket's start and end. Consecutive buckets meet in zero-width
// triangles, so each bucket is a flat-topped column.
void FillLane(QSGGeometryNode* peak_node, QSGGeometryNode* rms_node, const std::vector<WaveformPeak>& peaks,
              std::uint32_t buckets, int channels, int lane_channel, const Lane& lane, double bucket_samples,
              float gain) {
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
        float top = lane.center - std::clamp(hi * gain, -1.0f, 1.0f) * lane.half;
        float bottom = lane.center - std::clamp(lo * gain, -1.0f, 1.0f) * lane.half;
        level = std::clamp(level * gain, 0.0f, 1.0f);
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
                       int lane_channel, const Lane& lane, double first_x, double bucket_width, const QColor& color,
                       float gain) {
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
        const float rms = std::clamp(
            (channels == 1 ? std::sqrt((ra * ra + rb * rb) / 2) : engine::WaveformPeakToFloat(channel.rms)) * gain,
            0.0f, 1.0f);
        float top = lane.center - std::clamp(hi * gain, -1.0f, 1.0f) * lane.half;
        float bottom = lane.center - std::clamp(lo * gain, -1.0f, 1.0f) * lane.half;
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

struct ExoWaveformItem::PcmDetail {
    struct Interest {
        std::uint64_t ticket = 0;
        std::uint64_t generation = 0;
    };
    std::map<std::int64_t, Interest> interests;
    // Qt blocks the GUI thread during scene-graph synchronization. Only polish
    // publishes these immutable buffers; updatePaintNode reads them during sync.
    std::vector<std::shared_ptr<const WaveformCacheService::PcmWindow>> windows;
    std::uint64_t generation = 0;
    bool admission_pending = false;
    bool has_detail_result = false;
    QTimer retry_timer;
    std::vector<QPointer<QQuickItem>> ancestors;
    std::vector<QMetaObject::Connection> viewport_connections;

    std::shared_ptr<const WaveformCacheService::PcmWindow>
    successor(const WaveformCacheService::PcmWindow& data) const {
        const auto end = data.first_sample + static_cast<std::int64_t>(data.samples.size() / 2);
        const auto found = std::find_if(windows.begin(), windows.end(),
                                        [end](const auto& window) { return window->first_sample == end; });
        return found == windows.end() ? nullptr : *found;
    }
};

void ExoWaveformItem::retirePcm() {
    pcm_->retry_timer.stop();
    if (service_)
        for (const auto& [first, interest] : pcm_->interests)
            service_->cancelPcmWindow(interest.ticket);
    pcm_->interests.clear();
    pcm_->windows.clear();
    pcm_->admission_pending = false;
    pcm_->has_detail_result = false;
    ++pcm_->generation;
}

void ExoWaveformItem::schedulePcmPolish() {
    if (polish_queued_.exchange(true))
        return;
    QMetaObject::invokeMethod(
        this,
        [this] {
            polish_queued_.store(false);
            updatePolish();
        },
        Qt::QueuedConnection);
}

void ExoWaveformItem::updatePolish() {
    std::vector<QPointer<QQuickItem>> ancestors;
    for (auto* ancestor = parentItem(); ancestor; ancestor = ancestor->parentItem())
        ancestors.emplace_back(ancestor);
    if (ancestors != pcm_->ancestors) {
        for (const auto& connection : pcm_->viewport_connections)
            disconnect(connection);
        pcm_->viewport_connections.clear();
        pcm_->ancestors = std::move(ancestors);
        for (const auto& ancestor : pcm_->ancestors) {
            const auto observe = [this, &ancestor](auto signal) {
                pcm_->viewport_connections.push_back(
                    connect(ancestor.data(), signal, this, &ExoWaveformItem::schedulePcmPolish));
            };
            observe(&QQuickItem::xChanged);
            observe(&QQuickItem::yChanged);
            observe(&QQuickItem::widthChanged);
            observe(&QQuickItem::heightChanged);
            observe(&QQuickItem::visibleChanged);
            observe(&QQuickItem::clipChanged);
            observe(&QQuickItem::parentChanged);
            observe(&QQuickItem::rotationChanged);
            observe(&QQuickItem::scaleChanged);
            observe(&QQuickItem::transformOriginChanged);
        }
    }
    const auto first = engine::AudioSampleIndex(source_in_us_);
    const auto end = engine::AudioSampleIndex(source_out_us_);
    const auto visible = VisibleRect(*this);
    const double spp = width() > 0 ? static_cast<double>(end - first) / width() : 0;
    if (!service_ || cache_key_.isEmpty() || !isVisible() || visible.isEmpty() || first < 0 || spp <= 0 ||
        spp >= engine::kWaveformBaseBucketSamples || visible.width() * spp > kMaxVisiblePcmSamples) {
        const bool changed = !pcm_->windows.empty();
        retirePcm();
        if (changed)
            update();
        return;
    }
    const auto begin = static_cast<std::int64_t>(first + visible.left() * spp) / kPcmWindowSamples * kPcmWindowSamples;
    const auto last = static_cast<std::int64_t>(std::ceil(first + visible.right() * spp)) + 1;
    for (auto it = pcm_->interests.begin(); it != pcm_->interests.end();) {
        if (it->first < begin || it->first >= last) {
            service_->cancelPcmWindow(it->second.ticket);
            it = pcm_->interests.erase(it);
        } else {
            ++it;
        }
    }
    pcm_->admission_pending = false;
    for (auto sample = begin; sample < last; sample += kPcmWindowSamples) {
        if (pcm_->interests.contains(sample))
            continue;
        const auto generation = ++pcm_->generation;
        const auto ticket = service_->requestPcmWindow(cache_key_, sample, kPcmWindowSamples, generation);
        if (ticket)
            pcm_->interests.emplace(sample, PcmDetail::Interest{ticket, generation});
        else
            pcm_->admission_pending = true;
    }
    std::vector<std::shared_ptr<const WaveformCacheService::PcmWindow>> windows;
    bool pressure = false;
    bool has_detail_result = false;
    for (const auto& [sample, interest] : pcm_->interests) {
        auto snapshot = service_->pcmWindow(interest.ticket);
        pressure |= snapshot.state == WaveformCacheService::DetailState::MemoryPressure;
        has_detail_result |= snapshot.state == WaveformCacheService::DetailState::NoCoverage;
        if (snapshot.state == WaveformCacheService::DetailState::Ready && snapshot.window)
            windows.push_back(std::move(snapshot.window));
    }
    has_detail_result |= !windows.empty();
    const bool retry =
        pressure || (pcm_->admission_pending && service_->state(cache_key_) != WaveformCacheService::State::Unknown);
    if (retry && !pcm_->retry_timer.isActive())
        pcm_->retry_timer.start();
    else if (!retry)
        pcm_->retry_timer.stop();
    if (windows != pcm_->windows || has_detail_result != pcm_->has_detail_result) {
        pcm_->windows = std::move(windows);
        pcm_->has_detail_result = has_detail_result;
        update();
    }
}

int WaveformLevelForSamplesPerPixel(double samples_per_pixel, int level_count) {
    if (level_count <= 0)
        return -1;
    if (!(samples_per_pixel > engine::kWaveformBaseBucketSamples))
        return 0;
    const int level = static_cast<int>(std::floor(std::log2(samples_per_pixel / engine::kWaveformBaseBucketSamples)));
    return std::clamp(level, 0, level_count - 1);
}

ExoWaveformItem::ExoWaveformItem(QQuickItem* parent) : QQuickItem(parent), pcm_(std::make_unique<PcmDetail>()) {
    setFlag(ItemHasContents, true);
    setFlag(ItemObservesViewport, true);
    // Bucket blocks extend past the visible source window.
    setClip(true);
    connect(this, &QQuickItem::visibleChanged, this, &ExoWaveformItem::schedulePcmPolish);
    connect(this, &QQuickItem::parentChanged, this, &ExoWaveformItem::schedulePcmPolish);
    connect(this, &QQuickItem::windowChanged, this, &ExoWaveformItem::schedulePcmPolish);
    pcm_->retry_timer.setSingleShot(true);
    pcm_->retry_timer.setInterval(1000);
    connect(&pcm_->retry_timer, &QTimer::timeout, this, [this] {
        if (!service_)
            return;
        for (auto it = pcm_->interests.begin(); it != pcm_->interests.end();) {
            if (service_->pcmWindow(it->second.ticket).state == WaveformCacheService::DetailState::MemoryPressure) {
                service_->cancelPcmWindow(it->second.ticket);
                it = pcm_->interests.erase(it);
            } else {
                ++it;
            }
        }
        schedulePcmPolish();
    });
}

ExoWaveformItem::~ExoWaveformItem() {
    // QQuickItem emits parent/window changes during base destruction, when
    // slots on the derived object are no longer valid.
    disconnect(this, nullptr, this, nullptr);
    for (const auto& connection : pcm_->viewport_connections)
        disconnect(connection);
    if (service_)
        disconnect(service_.data(), nullptr, this, nullptr);
    retirePcm();
}

QObject* ExoWaveformItem::service() const {
    return service_.data();
}

void ExoWaveformItem::setService(QObject* service) {
    auto* typed = qobject_cast<WaveformCacheService*>(service);
    if (typed == service_.data())
        return;
    retirePcm();
    if (service_)
        disconnect(service_.data(), nullptr, this, nullptr);
    service_ = typed;
    if (typed) {
        connect(typed, &WaveformCacheService::progress, this, [this](const QString& key) { onServiceKey(key); });
        connect(typed, &WaveformCacheService::ready, this, &ExoWaveformItem::onServiceKey);
        connect(typed, &WaveformCacheService::blocksLoaded, this, &ExoWaveformItem::onServiceKey);
        connect(typed, &WaveformCacheService::sourceAvailabilityChanged, this, &ExoWaveformItem::onServiceKey);
        connect(typed, &WaveformCacheService::pcmInterestsInvalidated, this, [this](const QString& key) {
            if (key.isEmpty() || key == cache_key_) {
                retirePcm();
                schedulePcmPolish();
                update();
            }
        });
        connect(typed, &QObject::destroyed, this, [this] {
            retirePcm();
            update();
        });
        connect(typed, &WaveformCacheService::pcmWindowChanged, this, [this](quint64 ticket, quint64 generation) {
            if (pcm_->admission_pending ||
                std::any_of(pcm_->interests.begin(), pcm_->interests.end(), [=](const auto& entry) {
                    return entry.second.ticket == ticket && entry.second.generation == generation;
                }))
                schedulePcmPolish();
        });
    }
    emit serviceChanged();
    schedulePcmPolish();
    update();
}

void ExoWaveformItem::onServiceKey(const QString& key) {
    if (key == cache_key_) {
        schedulePcmPolish();
        update();
    }
}

void ExoWaveformItem::setCacheKey(const QString& key) {
    if (key == cache_key_)
        return;
    retirePcm();
    cache_key_ = key;
    emit cacheKeyChanged();
    schedulePcmPolish();
    update();
}

void ExoWaveformItem::setSourceInUs(qint64 value) {
    if (value == source_in_us_)
        return;
    source_in_us_ = value;
    schedulePcmPolish();
    emit sourceWindowChanged();
    update();
}

void ExoWaveformItem::setSourceOutUs(qint64 value) {
    if (value == source_out_us_)
        return;
    source_out_us_ = value;
    schedulePcmPolish();
    emit sourceWindowChanged();
    update();
}

void ExoWaveformItem::setGainDb(qreal value) {
    if (!std::isfinite(value))
        return;
    const auto bounded = std::clamp(value, qreal{engine::kMinGainDb}, qreal{engine::kMaxGainDb});
    if (bounded == gain_db_)
        return;
    gain_db_ = bounded;
    emit gainDbChanged();
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
    schedulePcmPolish();
    if (new_geometry.size() != old_geometry.size())
        update();
}

QSGNode* ExoWaveformItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) {
    const float gain = engine::GainDbToLinear(static_cast<float>(gain_db_), false);
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
    if (software_renderer) {
        delete root;
        root = nullptr;
    }
    if (!root)
        root = new WaveformRootNode;

    const auto visible = VisibleRect(*this);
    schedulePcmPolish();
    if (visible.isEmpty()) {
        root->Reset();
        return root;
    }

    const double detail_spp = static_cast<double>(end_sample - first_sample) / width;
    const bool detail = detail_spp < engine::kWaveformBaseBucketSamples &&
                        visible.width() * detail_spp <= kMaxVisiblePcmSamples && pcm_->has_detail_result;
    if (detail) {
        last_level_.store(-1, std::memory_order_relaxed);
        const auto h = static_cast<float>(height);
        if (root->key != cache_key_ || root->level != -2 || root->channels != channel_count_ || root->height != h ||
            root->color != color_ || root->gain != gain) {
            root->Reset();
            root->key = cache_key_;
            root->level = -2;
            root->channels = channel_count_;
            root->height = h;
            root->color = color_;
            root->gain = gain;
        }
        const std::vector<Lane> lanes = channel_count_ == 2
                                            ? std::vector<Lane>{{h / 4, std::max(0.0f, h / 4 - kLaneGap)},
                                                                {3 * h / 4, std::max(0.0f, h / 4 - kLaneGap)}}
                                            : std::vector<Lane>{{h / 2, std::max(0.0f, h / 2 - kLaneGap / 2)}};
        QImage image;
        QPainter painter;
        if (software_renderer) {
            const auto dpr = window()->effectiveDevicePixelRatio();
            image = QImage(QSize(qCeil(visible.width() * dpr), qCeil(visible.height() * dpr)),
                           QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(dpr);
            image.fill(Qt::transparent);
            painter.begin(&image);
            painter.translate(-visible.left(), -visible.top());
            painter.setPen(QPen(color_, 1));
        }
        for (auto it = root->pcm.begin(); it != root->pcm.end();) {
            if (std::none_of(pcm_->windows.begin(), pcm_->windows.end(), [&](const auto& data) {
                    return data->first_sample == it->first && it->second.data.lock() == data &&
                           it->second.successor.lock() == pcm_->successor(*data);
                })) {
                root->removeChildNode(it->second.transform);
                delete it->second.transform;
                it = root->pcm.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& data : pcm_->windows) {
            const auto successor = pcm_->successor(*data);
            const double x = (data->first_sample - first_sample) / detail_spp;
            auto found = root->pcm.find(data->first_sample);
            QSGTransformNode* transform = found == root->pcm.end() ? nullptr : found->second.transform;
            if (!software_renderer && !transform) {
                transform = new QSGTransformNode;
                root->appendChildNode(transform);
                root->pcm.emplace(data->first_sample, PcmNode{transform, data, successor});
            }
            if (software_renderer || found == root->pcm.end()) {
                for (int channel = 0; channel < 2; ++channel) {
                    const auto lines =
                        PcmLines(*data, successor.get(), channel, lanes[channel_count_ == 1 ? 0 : channel], gain);
                    if (software_renderer) {
                        for (const auto& line : lines)
                            painter.drawLine(
                                QLineF(x + line.x1() / detail_spp, line.y1(), x + line.x2() / detail_spp, line.y2()));
                    } else {
                        auto* node = MakeBandNode(color_);
                        node->geometry()->setDrawingMode(QSGGeometry::DrawLines);
                        node->geometry()->allocate(static_cast<int>(lines.size() * 2));
                        auto* vertices = node->geometry()->vertexDataAsPoint2D();
                        for (std::size_t i = 0; i < lines.size(); ++i) {
                            vertices[i * 2].set(static_cast<float>(lines[i].x1()), static_cast<float>(lines[i].y1()));
                            vertices[i * 2 + 1].set(static_cast<float>(lines[i].x2()),
                                                    static_cast<float>(lines[i].y2()));
                        }
                        transform->appendChildNode(node);
                    }
                }
            }
            if (transform) {
                QMatrix4x4 matrix;
                matrix.translate(static_cast<float>(x), 0);
                matrix.scale(static_cast<float>(1 / detail_spp), 1);
                transform->setMatrix(matrix);
            }
        }
        if (software_renderer) {
            painter.end();
            root->image = window()->createImageNode();
            root->image->setOwnsTexture(true);
            root->image->setTexture(window()->createTextureFromImage(image));
            root->image->setSourceRect(QRectF(0, 0, image.width(), image.height()));
            root->image->setRect(visible);
            root->appendChildNode(root->image);
        }
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
        root->color != color_ || root->gain != gain) {
        root->Reset();
        root->key = cache_key_;
        root->level = level;
        root->channels = channels;
        root->height = lane_height;
        root->color = color_;
        root->gain = gain;
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
                                  bucket_samples / samples_per_pixel, color_, gain);
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
                         bucket_samples, gain);
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
