#pragma once

#include <QMutex>
#include <QPointer>
#include <QQuickItem>
#include <QSize>
#include <QString>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>

#include <exosnap/engine/edit_player_engine.h>
#include <exosnap/engine/edit_timeline_compositor.h>

#include <atomic>
#include <cstdint>
#include <optional>

namespace exosnap::quick {

class EditPlayerAdapter;

struct EditFramePresentation {
    quint64 request_generation = 0;
    quint64 revision = 0;
    bool operator==(const EditFramePresentation&) const = default;
};

// Scene-graph video surface for the Edit player.
//
// Sibling of ExoPreviewItem, built on the same skeleton -- QSGImageNode under a
// QSGClipNode, generation counters, beginExternalCommands/endExternalCommands
// around every native pass, sceneGraphInvalidated/Initialized reconnection, and
// render-state postback to the GUI thread -- but NOT on the same transport. The
// Record preview receives a shared NT handle to a GPU texture; the editor's
// decoder hands over CPU YUV planes (RawDecodedVideoFrame), whose entire
// cross-thread lifetime model is the frame's own `backing_frame` shared_ptr. So
// the payload here is a single-slot mailbox of decoded frames, uploaded and
// colour-converted on Qt's own D3D11 device by EditFrameGpuConverter.
//
// There is deliberately NO child HWND anywhere in this path -- no
// WA_NativeWindow, no CreateWindowEx, no QQuickWidget, no createWindowContainer.
// The Widgets EditPlayerSurface/EditPlayerRenderer pair created a WS_CHILD
// window with its own swap chain and render thread; reintroducing one would
// restore exactly the input/compositing barrier a native child HWND introduces.
//
// The present-gate that policy DOES survive: a frame whose PTS is already behind
// the playback clock is dropped in presentFrame, before any GPU work, exactly as
// EditPlayerRenderer::PresentFrame's gate did.
class ExoEditPlayerItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(exosnap::quick::EditPlayerAdapter* playerAdapter READ playerAdapter WRITE setPlayerAdapter NOTIFY
                   playerAdapterChanged FINAL)
    Q_PROPERTY(qreal cornerRadius READ cornerRadius WRITE setCornerRadius NOTIFY cornerRadiusChanged FINAL)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY hasFrameChanged FINAL)
    Q_PROPERTY(QSize sourceSize READ sourceSize NOTIFY sourceSizeChanged FINAL)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged FINAL)
    Q_PROPERTY(QVariant presentedTimeUs READ presentedTimeUs NOTIFY presentedTimeChanged FINAL)
    Q_PROPERTY(QVariant sceneGraphConsumedRevision READ sceneGraphConsumedRevision NOTIFY presentedTimeChanged FINAL)

  public:
    explicit ExoEditPlayerItem(QQuickItem* parent = nullptr);
    ~ExoEditPlayerItem() override;

    [[nodiscard]] EditPlayerAdapter* playerAdapter() const noexcept;
    void setPlayerAdapter(EditPlayerAdapter* adapter);

    [[nodiscard]] qreal cornerRadius() const noexcept;
    void setCornerRadius(qreal radius);
    [[nodiscard]] bool hasFrame() const noexcept;
    [[nodiscard]] QSize sourceSize() const;
    [[nodiscard]] const QString& errorText() const noexcept;
    [[nodiscard]] QVariant presentedTimeUs() const;
    [[nodiscard]] QVariant sceneGraphConsumedRevision() const;

    // Called from the decoder's own worker thread. Newest-wins: an undrawn frame
    // is superseded rather than queued.
    void presentFrame(exosnap::engine::RawDecodedVideoFrame frame);
    void presentFrame(exosnap::engine::RawDecodedVideoFrame frame, int64_t timeline_time_us,
                      std::optional<EditFramePresentation> presentation = {});
    void presentTimelineFrame(exosnap::engine::TimelineVideoFrame frame,
                              std::optional<EditFramePresentation> presentation = {});
    // Playback clock in absolute media time (µs), or negative for "no clock".
    // Safe from any thread; read by the present gate above.
    void setClockUs(int64_t media_time_us) noexcept;
    // Drops the mailbox and the shown picture (clip closed / replaced).
    void clearFrame();
    void supersedePendingFrame();

    void publishRenderStateFromRenderThread(quint64 generation, bool ready, QSize size, QString error,
                                            std::optional<int64_t> timeline_time_us = {},
                                            std::optional<EditFramePresentation> presentation = {});

  signals:
    void playerAdapterChanged();
    void cornerRadiusChanged();
    void hasFrameChanged();
    void sourceSizeChanged();
    void errorTextChanged();
    void presentedTimeChanged();
    void framePresented(qint64 timeline_time_us);
    void frameConsumedWithRevision(qint64 timeline_time_us, quint64 request_generation, quint64 revision);

  protected:
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

  public:
    // Render-thread transport state. Public only so the scene-graph node
    // implemented in the .cpp can consume it; this is not a QML API.
    struct PendingFrame {
        std::optional<exosnap::engine::RawDecodedVideoFrame> frame;
        std::optional<exosnap::engine::TimelineVideoFrame> timeline;
        std::optional<int64_t> timeline_time_us;
        std::optional<EditFramePresentation> presentation;
        quint64 generation = 0;
        bool clear = false;
    };

    [[nodiscard]] PendingFrame takePendingFrame();

  private:
    void applyRenderState(bool ready, const QSize& size, const QString& error, std::optional<int64_t> timeline_time_us,
                          std::optional<EditFramePresentation> presentation);
    void postRenderState(quint64 generation, bool ready, QSize size, QString error,
                         std::optional<int64_t> timeline_time_us = {},
                         std::optional<EditFramePresentation> presentation = {});
    void scheduleUpdate();

    QPointer<EditPlayerAdapter> player_adapter_;
    qreal corner_radius_ = 12.0;

    mutable QMutex pending_mutex_;
    PendingFrame pending_;
    quint64 next_generation_ = 1;
    std::atomic<quint64> active_generation_{0};
    std::atomic<int64_t> clock_us_{-1};
    std::atomic<bool> update_pending_{false};

    QPointer<QQuickWindow> connected_window_;
    QMetaObject::Connection scene_graph_invalidated_connection_;
    QMetaObject::Connection scene_graph_initialized_connection_;

    bool has_frame_ = false;
    QSize source_size_;
    QString error_text_;
    std::optional<int64_t> presented_time_us_;
    std::optional<EditFramePresentation> presented_presentation_;
};

} // namespace exosnap::quick
