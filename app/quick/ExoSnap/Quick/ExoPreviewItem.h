#pragma once

#include "PreviewMetricsSnapshot.h"
#include "RecordPreviewAdapter.h"

#include <QMutex>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QSize>

#include <recorder_core/preview_tap.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace exosnap::quick {

class ExoPreviewItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(exosnap::quick::RecordPreviewAdapter* previewAdapter READ previewAdapter WRITE setPreviewAdapter NOTIFY
                   previewAdapterChanged FINAL)
    Q_PROPERTY(qreal cornerRadius READ cornerRadius WRITE setCornerRadius NOTIFY cornerRadiusChanged FINAL)
    Q_PROPERTY(QRectF normalizedSourceRect READ normalizedSourceRect WRITE setNormalizedSourceRect NOTIFY
                   normalizedSourceRectChanged FINAL)
    Q_PROPERTY(bool frameReady READ frameReady NOTIFY frameReadyChanged FINAL)
    Q_PROPERTY(QSize sourceSize READ sourceSize NOTIFY sourceSizeChanged FINAL)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged FINAL)

  public:
    explicit ExoPreviewItem(QQuickItem* parent = nullptr);
    ~ExoPreviewItem() override;

    [[nodiscard]] RecordPreviewAdapter* previewAdapter() const noexcept;
    void setPreviewAdapter(RecordPreviewAdapter* adapter);

    [[nodiscard]] qreal cornerRadius() const noexcept;
    void setCornerRadius(qreal radius);
    [[nodiscard]] QRectF normalizedSourceRect() const noexcept;
    void setNormalizedSourceRect(const QRectF& rect);

    [[nodiscard]] bool frameReady() const noexcept;
    [[nodiscard]] QSize sourceSize() const;
    [[nodiscard]] const QString& errorText() const noexcept;

    // GUI-thread handoff. Ownership of nt_handle transfers to this item. The
    // handle is opened and closed on the scene-graph render thread.
    void presentSharedTexture(void* nt_handle, uint32_t width, uint32_t height, recorder_core::PreviewTapDesc tap);
    void clearSharedTexture();

    // GUI thread. One scene update because something the preview shows may have
    // changed — in practice a producer published a new frame. The item does not
    // reach for the producer itself: RecordPreviewAdapter owns the transport and
    // the PreviewUpdateScheduler that decides when this is worth calling.
    void requestSceneUpdate();

    [[nodiscard]] PreviewMetricsSnapshot metricsSnapshot() const;
    void resetMetrics();
    void publishRenderStateFromRenderThread(quint64 generation, bool ready, QSize size, QString error);
    [[nodiscard]] bool renderLoopActive() const noexcept;

    // Render-thread transport state. Public only so the private scene-graph
    // node implemented in the .cpp can consume it; this is not a QML API.
    struct PendingSource {
        void* handle = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        recorder_core::PreviewTapDesc tap{};
        quint64 generation = 0;
        bool clear = false;
    };

    static constexpr size_t kMetricWindow = 1024;
    struct Metrics {
        std::atomic<quint64> render_frames{0};
        std::atomic<quint64> consumed_frames{0};
        std::atomic<quint64> mutex_misses{0};
        std::atomic<quint64> interval_write{0};
        std::atomic<quint64> scene_interval_write{0};
        std::atomic<quint64> submit_write{0};
        std::array<std::atomic<qint64>, kMetricWindow> interval_ns{};
        std::array<std::atomic<qint64>, kMetricWindow> scene_interval_ns{};
        std::array<std::atomic<qint64>, kMetricWindow> submit_ns{};
        std::atomic<qint64> last_consumed_ns{0};
        std::atomic<qint64> last_scene_frame_ns{0};
        std::atomic<uint32_t> source_dxgi_format{0};
    };

  signals:
    void previewAdapterChanged();
    void cornerRadiusChanged();
    void normalizedSourceRectChanged();
    void frameReadyChanged();
    void sourceSizeChanged();
    void errorTextChanged();

  protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

  private:
    friend class RecordPreviewAdapter;

    PendingSource takePendingSource();
    bool queueRetainedSource();
    void applyRenderState(bool ready, const QSize& size, const QString& error);
    void postRenderState(quint64 generation, bool ready, QSize size, QString error);
    static void closeHandle(void* handle) noexcept;
    static void* duplicateHandle(void* handle) noexcept;

    QPointer<RecordPreviewAdapter> preview_adapter_;
    qreal corner_radius_ = 12.0;
    QRectF normalized_source_rect_{0.0, 0.0, 1.0, 1.0};

    mutable QMutex pending_mutex_;
    PendingSource pending_;
    PendingSource retained_;
    quint64 next_generation_ = 1;
    std::atomic<quint64> active_generation_{0};

    QPointer<QQuickWindow> connected_window_;
    QMetaObject::Connection scene_graph_invalidated_connection_;
    QMetaObject::Connection scene_graph_initialized_connection_;

    bool frame_ready_ = false;
    QSize source_size_;
    QString error_text_;
    std::atomic<bool> render_loop_active_{false};
    Metrics metrics_;
};

} // namespace exosnap::quick
