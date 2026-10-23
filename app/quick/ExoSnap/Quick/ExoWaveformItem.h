#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

#include <atomic>
#include <memory>

namespace exosnap {
class WaveformCacheService;
}

namespace exosnap::quick {

// Scene-graph waveform of one audio clip's source window.
//
// Draws cached peaks and asynchronous normalized PCM detail of the stream
// named by `cacheKey` between
// `sourceInUs` and `sourceOutUs` (media-local source time) across the item's
// width. The level of detail is chosen so every pixel column covers at least
// one bucket. Geometry is built per fixed bucket block in absolute source
// sample units and only positioned per frame, so panning never shifts bucket
// boundaries and a block that gains analysed buckets is rebuilt alone. The
// software renderer rasterizes the visible cached buckets at the window's
// device pixel ratio.
//
// Ranges without data draw nothing. Gain scales amplitudes around each channel
// center without changing source data. Muted/disabled presentation and fade
// envelopes belong to the surrounding QML.
class ExoWaveformItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    // A WaveformCacheService. Typed as QObject so QML needs no registration of
    // the service class.
    Q_PROPERTY(QObject* service READ service WRITE setService NOTIFY serviceChanged FINAL)
    Q_PROPERTY(QString cacheKey READ cacheKey WRITE setCacheKey NOTIFY cacheKeyChanged FINAL)
    Q_PROPERTY(qint64 sourceInUs READ sourceInUs WRITE setSourceInUs NOTIFY sourceWindowChanged FINAL)
    Q_PROPERTY(qint64 sourceOutUs READ sourceOutUs WRITE setSourceOutUs NOTIFY sourceWindowChanged FINAL)
    // 2 draws each channel in its own lane; 1 draws one combined lane.
    Q_PROPERTY(int channelCount READ channelCount WRITE setChannelCount NOTIFY channelCountChanged FINAL)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged FINAL)
    Q_PROPERTY(qreal gainDb READ gainDb WRITE setGainDb NOTIFY gainDbChanged FINAL)

  public:
    explicit ExoWaveformItem(QQuickItem* parent = nullptr);
    ~ExoWaveformItem() override;

    [[nodiscard]] QObject* service() const;
    void setService(QObject* service);
    [[nodiscard]] const QString& cacheKey() const noexcept {
        return cache_key_;
    }
    void setCacheKey(const QString& key);
    [[nodiscard]] qint64 sourceInUs() const noexcept {
        return source_in_us_;
    }
    void setSourceInUs(qint64 value);
    [[nodiscard]] qint64 sourceOutUs() const noexcept {
        return source_out_us_;
    }
    void setSourceOutUs(qint64 value);
    [[nodiscard]] int channelCount() const noexcept {
        return channel_count_;
    }
    void setChannelCount(int count);
    [[nodiscard]] const QColor& color() const noexcept {
        return color_;
    }
    void setColor(const QColor& color);
    [[nodiscard]] qreal gainDb() const noexcept {
        return gain_db_;
    }
    void setGainDb(qreal value);

    // Peak pyramid level used for the current geometry, or -1 for PCM detail
    // or an empty drawing. Exposed for tests.
    [[nodiscard]] int levelForTest() const noexcept {
        return last_level_.load(std::memory_order_relaxed);
    }

  signals:
    void serviceChanged();
    void cacheKeyChanged();
    void sourceWindowChanged();
    void channelCountChanged();
    void colorChanged();
    void gainDbChanged();

  protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
    void updatePolish() override;

  private:
    void onServiceKey(const QString& key);
    void retirePcm();
    void schedulePcmPolish();
    struct PcmDetail;
    std::unique_ptr<PcmDetail> pcm_;
    std::atomic<bool> polish_queued_{false};

    QPointer<WaveformCacheService> service_;
    QString cache_key_;
    qint64 source_in_us_ = 0;
    qint64 source_out_us_ = 0;
    int channel_count_ = 2;
    QColor color_{Qt::white};
    qreal gain_db_ = 0;
    std::atomic<int> last_level_{-1};
};

// Level whose bucket width does not exceed `samples_per_pixel`, clamped to the
// pyramid. Pure, so the level-of-detail rule is testable without a scene graph.
[[nodiscard]] int WaveformLevelForSamplesPerPixel(double samples_per_pixel, int level_count);

} // namespace exosnap::quick
