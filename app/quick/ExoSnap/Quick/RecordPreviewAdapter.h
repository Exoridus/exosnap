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
    // Whether the surface the preview draws on can be seen at all — the window is
    // shown and not minimized. Distinct from `active`, which only says the Record
    // destination is the selected one: a minimized window keeps that true, and
    // the capture hub then duplicated the desktop at ~66 Hz, published into the
    // shared texture and armed a scene update per frame for a window that cannot
    // render. See setSurfaceVisible() for the one case that is deliberately NOT
    // suspended.
    Q_PROPERTY(bool surfaceVisible READ surfaceVisible WRITE setSurfaceVisible NOTIFY surfaceVisibleChanged FINAL)
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

    [[nodiscard]] bool surfaceVisible() const noexcept;
    // Suspends/resumes the preview's own capture subscription. The engine-fed
    // case is exempt: while a recording owns the capture, the preview draws the
    // engine's WYSIWYG texture, whose shared handle is published once per capture
    // start rather than per frame — dropping it on a minimize would leave a black
    // preview until the recording ended, because nothing re-publishes it on
    // restore. That case costs no extra duplication anyway: the hub lease is out,
    // so the pump the suspension exists to stop is not running.
    void setSurfaceVisible(bool visible);

    [[nodiscard]] bool sourceAvailable() const noexcept;
    [[nodiscard]] bool frameReady() const noexcept;
    [[nodiscard]] const QString& sourceName() const noexcept;
    [[nodiscard]] const QString& statusText() const noexcept;
    [[nodiscard]] const QString& errorText() const noexcept;
    [[nodiscard]] QSize sourceSize() const;

    // The redraw gate the producers arm. Handed to ExoPreviewItem so it can ask
    // whether a published frame is still owed a presentation after the window
    // was unable to render; the item never arms or disarms it.
    [[nodiscard]] const std::shared_ptr<PreviewUpdateScheduler>& updateScheduler() const noexcept {
        return update_scheduler_;
    }
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
    void surfaceVisibleChanged();
    void sourceAvailableChanged();
    void frameReadyChanged();
    void sourceNameChanged();
    void statusTextChanged();
    void errorTextChanged();
    void sourceSizeChanged();
    void metricsChanged();
    void recordingStateChanged();

  private:
    // The one place `active` and `surfaceVisible` are resolved into "is the
    // preview subscription running". Both setters go through it, and so does the
    // end of observeRecordingState(), because losing the engine lease can turn a
    // hidden-but-exempt preview into one that should now be suspended.
    void applyPreviewRunState();
    void startPreview();
    void stopPreview();
    void synchronizeItemState();
    void updateMetrics();
    void setStatus(QString status);
    void setError(QString error);
    void acceptRecordingTexture(void* handle, uint32_t width, uint32_t height, recorder_core::PreviewTapDesc tap);
    // Hands the current engine source to the item as a duplicate handle, or
    // records that it is being held until one is available. Idempotent, so every
    // event that can make a consumer appear may simply call it.
    void presentEngineSourceIfPossible();
    void releaseEngineSource();
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
    // Defaults to true so a host that never sets it (the QML tests, the render
    // harness) behaves exactly as before this gate existed.
    bool surface_visible_ = true;
    bool preview_running_ = false;
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

    // The engine's shared texture for the current feed, owned here rather than
    // by whichever item existed when it was announced. See acceptRecordingTexture
    // for why the one-shot handover could not be kept.
    struct EngineSource {
        void* handle = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        recorder_core::PreviewTapDesc tap{};
    };
    EngineSource engine_source_;
    // Bumped per announcement, so a queued presentation from a previous feed can
    // be told apart from the current one.
    qulonglong engine_source_epoch_ = 0;
    qulonglong engine_source_announcements_ = 0;
    qulonglong engine_source_deferrals_ = 0;
    qulonglong engine_source_presentations_ = 0;
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
