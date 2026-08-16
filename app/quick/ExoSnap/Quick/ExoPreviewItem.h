#pragma once

#include "PreviewMetricsSnapshot.h"
#include "PreviewUpdateScheduler.h"
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
#include <memory>

namespace exosnap::quick {

class ExoPreviewItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(exosnap::quick::RecordPreviewAdapter* previewAdapter READ previewAdapter WRITE setPreviewAdapter NOTIFY
                   previewAdapterChanged FINAL)
    Q_PROPERTY(qreal cornerRadius READ cornerRadius WRITE setCornerRadius NOTIFY cornerRadiusChanged FINAL)
    // The preview is the lower half of the Record page's Preview Surface, whose
    // upper half is the preview toolbar. Its top corners therefore meet a
    // straight divider and must be square while its bottom corners follow the
    // surface. Negative (the default) means "same as cornerRadius".
    Q_PROPERTY(qreal topCornerRadius READ topCornerRadius WRITE setTopCornerRadius NOTIFY topCornerRadiusChanged FINAL)
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
    [[nodiscard]] qreal topCornerRadius() const noexcept;
    void setTopCornerRadius(qreal radius);
    // The radius the top corners actually clip to, resolving the "follow
    // cornerRadius" default.
    [[nodiscard]] qreal resolvedTopCornerRadius() const noexcept;
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

    // GUI thread. Re-issues exactly one update when a publish is still owed a
    // presentation. Called on the window lifecycle transitions that can swallow
    // a render request — see PreviewUpdateScheduler for why the producer cannot
    // re-offer the frame itself. A no-op when nothing is outstanding.
    // `transition` names the caller for EXOSNAP_PREVIEW_TRACE.
    void reissuePendingPresentation(const char* transition);

    // The gate this item's renders are driven by, or nullptr before an adapter
    // is attached. The scene-graph node keeps its own shared_ptr copy so it can
    // record render passes without touching the GUI thread's members.
    [[nodiscard]] const std::shared_ptr<PreviewUpdateScheduler>& updateScheduler() const noexcept;

    [[nodiscard]] PreviewMetricsSnapshot metricsSnapshot() const;
    void resetMetrics();

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

    // Everything the scene-graph node's preprocess() needs from this item,
    // owned by a shared_ptr the node keeps a reference to (QCR-109).
    //
    // Why not the item itself: preprocess() runs on the render thread AFTER the
    // GUI-thread block of the synchronization stage has been released (Qt Quick
    // Scene Graph, "Threaded Render Loop": step 7 releases the GUI thread, step
    // 8 invokes preprocess), and the node returned from updatePaintNode() is not
    // destroyed with its item — its deletion is deferred to a synchronization
    // stage, which is exactly why Qt tells implementations to schedule cleanup
    // with QQuickWindow::scheduleRenderJob(BeforeSynchronizingStage /
    // AfterSynchronizingStage) rather than deleting directly. So a node can be
    // in preprocess() while the GUI thread runs ~ExoPreviewItem, and a
    // QPointer is a GUI-thread guard, not a synchronization primitive: even a
    // non-null read is only true until the next instruction.
    //
    // The link removes the question instead of timing it. The render thread
    // touches only the atomics it owns for the node's lifetime; `item` is read
    // on the GUI thread alone, inside the queued publication below.
    struct RenderLink {
        Metrics metrics;
        std::atomic<bool> render_loop_active{false};

        // GUI THREAD ONLY. Cleared by ~ExoPreviewItem before the QObject part of
        // the item goes away.
        QPointer<ExoPreviewItem> item;

        // Render thread. Posts the state to the application object — never to
        // the item — and resolves the item on the GUI thread, where the answer
        // cannot change under the check. `link` is taken by value so the
        // publication keeps it alive on its own.
        static void publishRenderState(std::shared_ptr<RenderLink> link, quint64 generation, bool ready, QSize size,
                                       QString error);
    };

  signals:
    void previewAdapterChanged();
    void cornerRadiusChanged();
    void topCornerRadiusChanged();
    void normalizedSourceRectChanged();
    void frameReadyChanged();
    void sourceSizeChanged();
    void errorTextChanged();

  protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    // Only for QEvent::Expose on the window. QQuickItem has no hook for it and
    // QWindow emits no signal, but it is the transition that ends the window's
    // non-renderable stretch — so it is where an owed frame becomes payable.
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    friend class RecordPreviewAdapter;

    PendingSource takePendingSource();
    bool queueRetainedSource();
    void applyRenderState(bool ready, const QSize& size, const QString& error);
    void postRenderState(quint64 generation, bool ready, QSize size, QString error);
    // The render-thread-owned half of this item's state. Never null.
    [[nodiscard]] Metrics& metrics() const noexcept {
        return render_link_->metrics;
    }
    [[nodiscard]] std::atomic<bool>& renderLoopFlag() const noexcept {
        return render_link_->render_loop_active;
    }
    static void closeHandle(void* handle) noexcept;
    static void* duplicateHandle(void* handle) noexcept;

    QPointer<RecordPreviewAdapter> preview_adapter_;
    qreal corner_radius_ = 12.0;
    qreal top_corner_radius_ = -1.0;
    QRectF normalized_source_rect_{0.0, 0.0, 1.0, 1.0};

    mutable QMutex pending_mutex_;
    PendingSource pending_;
    PendingSource retained_;
    quint64 next_generation_ = 1;
    std::atomic<quint64> active_generation_{0};

    std::shared_ptr<PreviewUpdateScheduler> update_scheduler_;

    QPointer<QQuickWindow> connected_window_;
    QMetaObject::Connection scene_graph_invalidated_connection_;
    QMetaObject::Connection scene_graph_initialized_connection_;
    QMetaObject::Connection screen_changed_connection_;

    bool frame_ready_ = false;
    QSize source_size_;
    QString error_text_;
    // Outlives this item whenever a scene-graph node still holds it.
    const std::shared_ptr<RenderLink> render_link_ = std::make_shared<RenderLink>();
};

} // namespace exosnap::quick
