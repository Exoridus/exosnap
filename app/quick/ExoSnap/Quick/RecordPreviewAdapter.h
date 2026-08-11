#pragma once

#include "PreviewMetricsSnapshot.h"
#include "PreviewUpdateScheduler.h"
#include "ReadyFrameCaptureService.h"

#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <recorder_core/preview_tap.h>
#include <recorder_core/recorder_session.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <windows.h>

namespace exosnap {
class DxgiCaptureHubService;
class RecordingCoordinator;
class WgcCaptureHubService;
enum class UiRecordingState;
} // namespace exosnap

namespace exosnap::quick {

class ExoPreviewItem;

class RecordPreviewAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("RecordPreviewAdapter is provided by the application")
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)
    Q_PROPERTY(bool sourceAvailable READ sourceAvailable NOTIFY sourceAvailableChanged FINAL)
    Q_PROPERTY(bool frameReady READ frameReady NOTIFY frameReadyChanged FINAL)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY sourceNameChanged FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged FINAL)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged FINAL)
    Q_PROPERTY(QSize sourceSize READ sourceSize NOTIFY sourceSizeChanged FINAL)
    Q_PROPERTY(double presentationRate READ presentationRate NOTIFY metricsChanged FINAL)
    Q_PROPERTY(double sourceDeliveryRate READ sourceDeliveryRate NOTIFY metricsChanged FINAL)
    Q_PROPERTY(double frameTimeP95Ms READ frameTimeP95Ms NOTIFY metricsChanged FINAL)
    Q_PROPERTY(double frameTimeP99Ms READ frameTimeP99Ms NOTIFY metricsChanged FINAL)
    Q_PROPERTY(double submitP95Us READ submitP95Us NOTIFY metricsChanged FINAL)
    Q_PROPERTY(qulonglong consumedFrames READ consumedFrames NOTIFY metricsChanged FINAL)
    Q_PROPERTY(qulonglong mutexMisses READ mutexMisses NOTIFY metricsChanged FINAL)
    Q_PROPERTY(bool recordingActive READ recordingActive NOTIFY recordingStateChanged FINAL)
    Q_PROPERTY(QString recordingStateText READ recordingStateText NOTIFY recordingStateChanged FINAL)
    Q_PROPERTY(qulonglong recordingDroppedFrames READ recordingDroppedFrames NOTIFY metricsChanged FINAL)

  public:
    explicit RecordPreviewAdapter(QObject* parent = nullptr);
    ~RecordPreviewAdapter() override;

    [[nodiscard]] bool active() const noexcept;
    void setActive(bool active);

    [[nodiscard]] bool sourceAvailable() const noexcept;
    [[nodiscard]] bool frameReady() const noexcept;
    [[nodiscard]] const QString& sourceName() const noexcept;
    [[nodiscard]] const QString& statusText() const noexcept;
    [[nodiscard]] const QString& errorText() const noexcept;
    [[nodiscard]] QSize sourceSize() const;
    [[nodiscard]] double presentationRate() const noexcept;
    [[nodiscard]] double sourceDeliveryRate() const noexcept;
    [[nodiscard]] double frameTimeP95Ms() const noexcept;
    [[nodiscard]] double frameTimeP99Ms() const noexcept;
    [[nodiscard]] double submitP95Us() const noexcept;
    [[nodiscard]] qulonglong consumedFrames() const noexcept;
    [[nodiscard]] qulonglong mutexMisses() const noexcept;
    [[nodiscard]] bool recordingActive() const noexcept;
    [[nodiscard]] const QString& recordingStateText() const noexcept;
    [[nodiscard]] qulonglong recordingDroppedFrames() const noexcept;

    [[nodiscard]] QVariantMap benchmarkSnapshot() const;
    // Typed read of the attached preview item's instrumentation. The frontend A/B
    // harness needs the raw values rather than benchmarkSnapshot()'s QVariantMap,
    // which is shaped for the QML metrics overlay. Returns a default-constructed
    // snapshot when no item is attached.
    [[nodiscard]] PreviewMetricsSnapshot previewMetricsSnapshot() const;
    Q_INVOKABLE void resetMetrics();

    void attachPreviewItem(ExoPreviewItem* item);
    void detachPreviewItem(ExoPreviewItem* item);
    void bindRecordingCoordinator(RecordingCoordinator* coordinator);
    void setPreviewTarget(const recorder_core::CaptureTarget& target);
    void clearPreviewTarget();
    void observeRecordingState(UiRecordingState state);
    void observeRecordingStats(const recorder_core::SessionStats& stats);
    void observeRecordingDiagnostics(const recorder_core::RecordingDiagnosticsSnapshot& snapshot);
    void requestReadyFrame(ReadyFrameComposition composition, ReadyFrameCaptureService::Callback callback);

  signals:
    void activeChanged();
    void sourceAvailableChanged();
    void frameReadyChanged();
    void sourceNameChanged();
    void statusTextChanged();
    void errorTextChanged();
    void sourceSizeChanged();
    void metricsChanged();
    void recordingStateChanged();

  private:
    void startPreview();
    void stopPreview();
    void synchronizeItemState();
    void updateMetrics();
    void setStatus(QString status);
    void setError(QString error);
    void acceptRecordingTexture(void* handle, uint32_t width, uint32_t height, recorder_core::PreviewTapDesc tap);
    // Builds the per-frame publish edge every producer gets handed. Captures
    // nothing but shared_ptr/QPointer copies, so it is safe to call from a
    // capture pump thread or the engine's video thread.
    [[nodiscard]] std::function<void()> makeFramePublishedSink();

    // Shared with every producer lambda, so it outlives any of them. Created
    // once and never replaced: the counters then describe the whole session
    // across idle-hub and engine-tap feeds, which is what the A/B reads.
    std::shared_ptr<PreviewUpdateScheduler> update_scheduler_ = std::make_shared<PreviewUpdateScheduler>();

    std::unique_ptr<DxgiCaptureHubService> dxgi_source_;
    std::unique_ptr<WgcCaptureHubService> wgc_source_;
    std::optional<recorder_core::CaptureTarget> selected_target_;
    QPointer<ExoPreviewItem> item_;
    QTimer metrics_timer_;
    std::atomic<quint64> source_epoch_{0};
    bool active_ = false;
    bool source_available_ = false;
    bool frame_ready_ = false;
    QString source_name_;
    QString status_text_ = QStringLiteral("Preview inactive");
    QString error_text_;
    QSize source_size_;
    double presentation_rate_ = 0.0;
    double source_delivery_rate_ = 0.0;
    double frame_time_p95_ms_ = 0.0;
    double frame_time_p99_ms_ = 0.0;
    double submit_p95_us_ = 0.0;
    qulonglong consumed_frames_ = 0;
    qulonglong mutex_misses_ = 0;
    std::atomic<bool> engine_feed_expected_{false};
    bool recording_active_ = false;
    QString recording_state_text_ = QStringLiteral("Ready");
    qulonglong recording_dropped_frames_ = 0;
    qulonglong recording_captured_frames_ = 0;
    qulonglong recording_encoded_packets_ = 0;
    qulonglong recording_texture_generations_ = 0;
    HANDLE ready_source_handle_ = nullptr;
    uint32_t ready_source_width_ = 0;
    uint32_t ready_source_height_ = 0;
    recorder_core::PreviewTapDesc ready_source_tap_;
    recorder_core::CaptureTarget ready_source_target_;
    bool ready_source_cursor_composited_ = false;

  public:
    // Joins any in-flight Ready-frame worker NOW rather than at destruction.
    //
    // The pool below is destroyed first *within this class*, which is enough for
    // this class's own members -- but not for the composition root. The worker
    // runs a callback supplied by RecordingCoordinator::CaptureFrame that
    // captures the coordinator's snapshot_pool_ by pointer, and the coordinator
    // is destroyed before this adapter. Clearing the requester only stops NEW
    // requests; the one already running still lands in freed storage. The owner
    // therefore has to join explicitly, before it tears anything down.
    void waitForPendingReadyFrames();

  private:
    // Declared last so it is destroyed FIRST: its destructor waits for an
    // in-flight Ready-frame capture. That worker opens its own D3D11 device,
    // duplicates a shared texture through DXGI and finally calls back into
    // state the composition root owns. As a detached std::thread it had no
    // owner, no join and no cancel path, so it could still be inside COM/DXGI
    // after the objects around it — and Qt's statics — were gone. The join
    // always terminates: every step is bounded local GPU work, the keyed-mutex
    // acquire uses a 500 ms timeout, and nothing in the worker blocks on the
    // GUI thread. Same idiom as DiagnosticsAdapter::probe_pool_.
    QThreadPool ready_frame_pool_;
};

} // namespace exosnap::quick
