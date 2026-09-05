#include "RecordPreviewAdapter.h"

#include "ExoPreviewItem.h"
#include "PreviewPercentile.h"

#include "services/CaptureSourceKey.h"
#include "services/DxgiCaptureHubService.h"
#include "services/RecordingCoordinator.h"
#include "services/WgcCaptureHubService.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <exosnap/engine/recorder_session.h>

#include <windows.h>

#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace exosnap::quick {
namespace {

// The single owner of one queued shared-texture HANDLE; consumers hold it via
// shared_ptr, so the close happens exactly once, when the last reader is done.
// Non-copyable and non-movable for the reason the shared-texture path has been
// hardened for twice already: a copy means two owners and a double CloseHandle,
// and because Windows recycles handle values the second close can hit an
// unrelated object instead of failing. Handles travel as `void*` here, which is
// exactly where an accidental by-value capture hides.
struct QueuedSharedHandle {
    explicit QueuedSharedHandle(void* value) : handle(value) {
    }
    QueuedSharedHandle(const QueuedSharedHandle&) = delete;
    QueuedSharedHandle& operator=(const QueuedSharedHandle&) = delete;
    ~QueuedSharedHandle() {
        if (handle != nullptr)
            CloseHandle(static_cast<HANDLE>(handle));
    }
    void* handle = nullptr;
};

static_assert(!std::is_copy_constructible_v<QueuedSharedHandle>, "an owned HANDLE must never be copied");
static_assert(!std::is_copy_assignable_v<QueuedSharedHandle>, "an owned HANDLE must never be copied");
static_assert(!std::is_move_constructible_v<QueuedSharedHandle>,
              "no consumer moves one; add a move that clears the source first");

// The engine-source and feed-lifecycle lines share the switch the item's own
// presentation trace uses, so one variable turns the whole chain on. Read once:
// these sit on paths that run per recording, not per frame, but the trace is a
// diagnostic and must cost nothing when nobody asked for it.
bool previewTraceEnabled() {
    static const bool enabled = qEnvironmentVariableIntValue("EXOSNAP_PREVIEW_TRACE") != 0;
    return enabled;
}

QString targetDescription(const exosnap::engine::CaptureTarget& target) {
    const QString description = QString::fromUtf8(target.description);
    return description.isEmpty() ? QStringLiteral("Primary display") : description;
}

QString stateTextFor(UiRecordingState state) {
    switch (state) {
    case UiRecordingState::LoadingCapabilities:
        return QStringLiteral("Loading capabilities");
    case UiRecordingState::Ready:
        return QStringLiteral("Ready");
    case UiRecordingState::Blocked:
        return QStringLiteral("Blocked");
    case UiRecordingState::Preparing:
        return QStringLiteral("Preparing");
    case UiRecordingState::Recording:
        return QStringLiteral("Recording");
    case UiRecordingState::Paused:
        return QStringLiteral("Paused");
    case UiRecordingState::Stopping:
        return QStringLiteral("Stopping");
    case UiRecordingState::Saving:
        return QStringLiteral("Saving");
    case UiRecordingState::Completed:
        return QStringLiteral("Completed");
    case UiRecordingState::Failed:
        return QStringLiteral("Failed");
    case UiRecordingState::Countdown:
        return QStringLiteral("Countdown");
    case UiRecordingState::RegionSelecting:
        return QStringLiteral("Selecting region");
    case UiRecordingState::ArmedFromRecovery:
        return QStringLiteral("Recovery paused");
    }
    return QStringLiteral("Unknown");
}

} // namespace

RecordPreviewAdapter::RecordPreviewAdapter(QObject* parent)
    : QObject(parent), dxgi_source_(std::make_unique<DxgiCaptureHubService>()),
      wgc_source_(std::make_unique<WgcCaptureHubService>()) {
    metrics_timer_.setInterval(250);
    metrics_timer_.setTimerType(Qt::CoarseTimer);
    connect(&metrics_timer_, &QTimer::timeout, this, &RecordPreviewAdapter::updateMetrics);
}

RecordPreviewAdapter::~RecordPreviewAdapter() {
    stopPreview();
    releaseEngineSource();
    // Member destruction then joins ready_frame_pool_ (declared last, so
    // destroyed first). A Ready-frame worker in flight uses its own D3D11
    // device and touches none of the members torn down above it.
}

bool RecordPreviewAdapter::active() const noexcept {
    return active_;
}

void RecordPreviewAdapter::setActive(bool active) {
    if (active_ == active)
        return;
    active_ = active;
    emit activeChanged();
    // A held engine source is presented as soon as there is somewhere to put it.
    presentEngineSourceIfPossible();
    applyPreviewRunState();
}

bool RecordPreviewAdapter::surfaceVisible() const noexcept {
    return surface_visible_;
}

void RecordPreviewAdapter::setSurfaceVisible(bool visible) {
    if (surface_visible_ == visible)
        return;
    surface_visible_ = visible;
    emit surfaceVisibleChanged();
    applyPreviewRunState();
}

void RecordPreviewAdapter::applyPreviewRunState() {
    // While the engine owns the capture, the preview draws the recording's own
    // WYSIWYG texture, which arrives through acceptRecordingTexture() and needs
    // no hub subscription at all — so there is no duplication to suspend, and
    // dropping and retaking the subscription under an outstanding lease is
    // exactly the traffic QCR-104's gate has to defer. The gate stands still for
    // the whole engine-fed window and is re-evaluated when the lease comes back
    // (observeRecordingState). Read here rather than in QML because
    // `engine_feed_expected_` is not a property and must not become one: it is
    // written from the coordinator's release hook, on whichever thread that runs.
    if (engine_feed_expected_.load(std::memory_order_acquire))
        return;
    const bool want_running = active_ && surface_visible_;
    if (want_running == preview_running_)
        return;
    preview_running_ = want_running;
    if (want_running)
        startPreview();
    else
        stopPreview();
}

bool RecordPreviewAdapter::sourceAvailable() const noexcept {
    return source_available_;
}

bool RecordPreviewAdapter::frameReady() const noexcept {
    return frame_ready_;
}

const QString& RecordPreviewAdapter::sourceName() const noexcept {
    return source_name_;
}

const QString& RecordPreviewAdapter::statusText() const noexcept {
    return status_text_;
}

const QString& RecordPreviewAdapter::errorText() const noexcept {
    return error_text_;
}

QSize RecordPreviewAdapter::sourceSize() const {
    return source_size_;
}

double RecordPreviewAdapter::presentationRate() const noexcept {
    return presentation_rate_;
}

double RecordPreviewAdapter::sourceDeliveryRate() const noexcept {
    return source_delivery_rate_;
}

double RecordPreviewAdapter::frameTimeP95Ms() const noexcept {
    return frame_time_p95_ms_;
}

double RecordPreviewAdapter::frameTimeP99Ms() const noexcept {
    return frame_time_p99_ms_;
}

double RecordPreviewAdapter::submitP95Us() const noexcept {
    return submit_p95_us_;
}

qulonglong RecordPreviewAdapter::consumedFrames() const noexcept {
    return consumed_frames_;
}

qulonglong RecordPreviewAdapter::mutexMisses() const noexcept {
    return mutex_misses_;
}

bool RecordPreviewAdapter::recordingActive() const noexcept {
    return recording_active_;
}

const QString& RecordPreviewAdapter::recordingStateText() const noexcept {
    return recording_state_text_;
}

qulonglong RecordPreviewAdapter::recordingDroppedFrames() const noexcept {
    return recording_dropped_frames_;
}

QVariantMap RecordPreviewAdapter::benchmarkSnapshot() const {
    const PreviewMetricsSnapshot metrics = previewMetricsSnapshot();
    const DxgiCaptureHubService::PreviewPublishStats publisher =
        dxgi_source_ != nullptr ? dxgi_source_->GetPreviewPublishStats() : DxgiCaptureHubService::PreviewPublishStats{};
    return {
        {QStringLiteral("source_name"), source_name_},
        {QStringLiteral("source_width"), source_size_.width()},
        {QStringLiteral("source_height"), source_size_.height()},
        {QStringLiteral("frame_ready"), frame_ready_},
        {QStringLiteral("render_frames"), QVariant::fromValue<qulonglong>(metrics.render_frames)},
        {QStringLiteral("consumed_frames"), QVariant::fromValue<qulonglong>(metrics.consumed_frames)},
        {QStringLiteral("mutex_misses"), QVariant::fromValue<qulonglong>(metrics.mutex_misses)},
        {QStringLiteral("scene_render_fps"), metrics.scene_fps},
        {QStringLiteral("scene_frame_ms_p50"), metrics.scene_frame_ms_p50},
        {QStringLiteral("scene_frame_ms_p95"), metrics.scene_frame_ms_p95},
        {QStringLiteral("scene_frame_ms_p99"), metrics.scene_frame_ms_p99},
        {QStringLiteral("scene_frame_ms_max"), metrics.scene_frame_ms_max},
        {QStringLiteral("source_delivery_fps"), metrics.source_delivery_fps},
        {QStringLiteral("source_interval_ms_p95"), metrics.source_interval_ms_p95},
        {QStringLiteral("source_interval_ms_p99"), metrics.source_interval_ms_p99},
        {QStringLiteral("producer_publish_attempts"), QVariant::fromValue<qulonglong>(publisher.attempts)},
        {QStringLiteral("producer_published_frames"), QVariant::fromValue<qulonglong>(publisher.published)},
        {QStringLiteral("producer_contention_drops"), QVariant::fromValue<qulonglong>(publisher.dropped_on_contention)},
        {QStringLiteral("submit_us_p50"), metrics.submit_us_p50},
        {QStringLiteral("submit_us_p95"), metrics.submit_us_p95},
        {QStringLiteral("submit_us_p99"), metrics.submit_us_p99},
        {QStringLiteral("source_dxgi_format"), metrics.source_dxgi_format},
        {QStringLiteral("preview_publish_signals"), QVariant::fromValue<qulonglong>(metrics.publish_signals)},
        {QStringLiteral("preview_coalesced_signals"), QVariant::fromValue<qulonglong>(metrics.coalesced_signals)},
        {QStringLiteral("preview_scene_update_requests"),
         QVariant::fromValue<qulonglong>(metrics.scene_update_requests)},
        {QStringLiteral("transport"), QStringLiteral("NT shared handle + keyed mutex + GPU CopyResource")},
        {QStringLiteral("cpu_readback"), false},
        {QStringLiteral("native_child_hwnd"), false},
        {QStringLiteral("recording_state"), recording_state_text_},
        {QStringLiteral("recording_capture_frames"), QVariant::fromValue<qulonglong>(recording_captured_frames_)},
        {QStringLiteral("recording_encoded_packets"), QVariant::fromValue<qulonglong>(recording_encoded_packets_)},
        {QStringLiteral("recording_dropped_frames"), QVariant::fromValue<qulonglong>(recording_dropped_frames_)},
        {QStringLiteral("recording_texture_generations"),
         QVariant::fromValue<qulonglong>(recording_texture_generations_)},
        // The engine source's own funnel. `announcements > presentations` with
        // deferrals standing means the handle arrived and is being held for a
        // consumer; `announcements == 0` means it never arrived at all.
        {QStringLiteral("engine_source_announcements"), QVariant::fromValue<qulonglong>(engine_source_announcements_)},
        {QStringLiteral("engine_source_deferrals"), QVariant::fromValue<qulonglong>(engine_source_deferrals_)},
        {QStringLiteral("engine_source_presentations"), QVariant::fromValue<qulonglong>(engine_source_presentations_)},
        {QStringLiteral("engine_source_epoch"), QVariant::fromValue<qulonglong>(engine_source_epoch_)},
    };
}

void RecordPreviewAdapter::waitForPendingReadyFrames() {
    ready_frame_pool_.waitForDone();
}

PreviewMetricsSnapshot RecordPreviewAdapter::previewMetricsSnapshot() const {
    PreviewMetricsSnapshot snapshot = item_ != nullptr ? item_->metricsSnapshot() : PreviewMetricsSnapshot{};
    // The scheduler lives here, not on the item: the item is only ever told to
    // redraw, it never learns why.
    snapshot.publish_signals = update_scheduler_->PublishSignals();
    snapshot.coalesced_signals = update_scheduler_->CoalescedSignals();
    snapshot.wakeups = update_scheduler_->Wakeups();
    snapshot.scene_update_requests = update_scheduler_->SceneUpdateRequests();

    const auto fill = [](std::vector<double> samples, double* p50, double* p95, double* p99, double* max) {
        if (samples.empty())
            return;
        std::sort(samples.begin(), samples.end());
        *p50 = PercentileSorted(samples, 0.50);
        *p95 = PercentileSorted(samples, 0.95);
        *p99 = PercentileSorted(samples, 0.99);
        *max = samples.back();
    };
    fill(update_scheduler_->PublishIntervalsMs(), &snapshot.publish_interval_ms_p50, &snapshot.publish_interval_ms_p95,
         &snapshot.publish_interval_ms_p99, &snapshot.publish_interval_ms_max);
    fill(update_scheduler_->PresentationDebtAgesMs(), &snapshot.debt_age_ms_p50, &snapshot.debt_age_ms_p95,
         &snapshot.debt_age_ms_p99, &snapshot.debt_age_ms_max);
    return snapshot;
}

void RecordPreviewAdapter::resetMetrics() {
    if (item_ != nullptr)
        item_->resetMetrics();
    if (dxgi_source_ != nullptr)
        dxgi_source_->ResetPreviewPublishStats();
    update_scheduler_->ResetCounters();
    recording_dropped_frames_ = 0;
    recording_captured_frames_ = 0;
    recording_encoded_packets_ = 0;
    recording_texture_generations_ = 0;
    updateMetrics();
    // updateMetrics() only signals a real change, and a reset can leave every
    // value where it already was while `recording_dropped_frames_` above went
    // back to zero. The harness reads these right after resetting them, so the
    // one place that must announce unconditionally says so here.
    emit metricsChanged();
}

void RecordPreviewAdapter::attachPreviewItem(ExoPreviewItem* item) {
    if (item_ == item)
        return;
    if (item_ != nullptr)
        item_->clearSharedTexture();
    item_ = item;
    if (item_ != nullptr) {
        connect(item_, &ExoPreviewItem::frameReadyChanged, this, &RecordPreviewAdapter::synchronizeItemState,
                Qt::UniqueConnection);
        connect(item_, &ExoPreviewItem::sourceSizeChanged, this, &RecordPreviewAdapter::synchronizeItemState,
                Qt::UniqueConnection);
        connect(item_, &ExoPreviewItem::errorTextChanged, this, &RecordPreviewAdapter::synchronizeItemState,
                Qt::UniqueConnection);
    }
    // A newly attached item has no texture, so the subscription is restarted even
    // when it was already running — hence the reset before the gate is asked.
    preview_running_ = false;
    // An item created after the engine announced its texture must still get it;
    // the announcement is never repeated.
    presentEngineSourceIfPossible();
    applyPreviewRunState();
}

void RecordPreviewAdapter::detachPreviewItem(ExoPreviewItem* item) {
    if (item_ != item)
        return;
    stopPreview();
    // The subscription is gone with the item, so the gate must agree — otherwise
    // a re-attach would find `preview_running_` still true and never resubscribe.
    preview_running_ = false;
    item_ = nullptr;
}

void RecordPreviewAdapter::bindRecordingCoordinator(RecordingCoordinator* coordinator) {
    if (coordinator == nullptr)
        return;
    QPointer<RecordPreviewAdapter> safe_self(this);
    coordinator->SetPreviewCaptureReleaseHook([safe_self]() {
        if (safe_self == nullptr)
            return;
        safe_self->engine_feed_expected_.store(true, std::memory_order_release);
        if (safe_self->dxgi_source_ != nullptr)
            safe_self->dxgi_source_->RequestEngineLease();
        if (safe_self->wgc_source_ != nullptr)
            safe_self->wgc_source_->RequestEngineLease();
    });
    coordinator->SetPreviewFramePublishedCallback(makeFramePublishedSink());
    coordinator->SetPreviewSharedHandleReadyCallback(
        [safe_self](void* handle, uint32_t width, uint32_t height, exosnap::engine::PreviewTapDesc tap) {
            auto handle_owner = std::make_shared<QueuedSharedHandle>(handle);
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [safe_self, handle_owner, width, height, tap]() {
                    if (safe_self == nullptr)
                        return;
                    // Deliberately NOT gated on engine_feed_expected_. The engine
                    // announces its shared texture exactly once per session, from
                    // the video thread as the first tapped frame goes through,
                    // while that flag is set by the coordinator's capture-release
                    // hook on another path entirely. When the announcement won the
                    // race it was dropped here — and nothing ever announced again,
                    // so the engine published into a texture with no consumer for
                    // the rest of the recording while the item kept polling the
                    // hub's abandoned one. Measured: 535 publish attempts, one
                    // success, 534 timeouts, against a consumer that never once
                    // acquired.
                    //
                    // The adapter owns the source and releases it when the feed
                    // ends, so accepting it early costs nothing and removes the
                    // race instead of narrowing it.
                    void* raw_handle = std::exchange(handle_owner->handle, nullptr);
                    safe_self->acceptRecordingTexture(raw_handle, width, height, tap);
                },
                Qt::QueuedConnection);
        });
}

void RecordPreviewAdapter::setPreviewTarget(const exosnap::engine::CaptureTarget& target) {
    const bool changed = !selected_target_.has_value() || selected_target_->kind != target.kind ||
                         selected_target_->native_id != target.native_id ||
                         selected_target_->description != target.description;
    selected_target_ = target;
    if (changed && preview_running_ && !engine_feed_expected_.load(std::memory_order_acquire))
        startPreview();
}

void RecordPreviewAdapter::clearPreviewTarget() {
    if (!selected_target_.has_value())
        return;
    selected_target_.reset();
    if (preview_running_)
        startPreview();
}

void RecordPreviewAdapter::observeRecordingState(UiRecordingState state) {
    const bool active = state == UiRecordingState::Recording || state == UiRecordingState::Paused;
    const QString state_text = stateTextFor(state);
    if (recording_active_ != active || recording_state_text_ != state_text) {
        recording_active_ = active;
        recording_state_text_ = state_text;
        emit recordingStateChanged();
    }
    if (ShouldRevertPreviewFromPushedMode(state) && engine_feed_expected_.exchange(false, std::memory_order_acq_rel)) {
        // The consumer half of the funnel, once, at the moment the engine feed
        // ends. The per-transition trace in ExoPreviewItem only fires on expose,
        // screen and scene-graph events, so a recording during which nothing
        // moved left no record of how the preview did at all.
        if (previewTraceEnabled()) {
            const PreviewMetricsSnapshot metrics = previewMetricsSnapshot();
            qInfo("preview-trace: engine-feed-ended publishes=%llu updates=%llu renders=%llu consumed=%llu "
                  "mutex_misses=%llu source_fps=%.2f",
                  metrics.publish_signals, metrics.scene_update_requests, metrics.render_frames,
                  metrics.consumed_frames, metrics.mutex_misses, metrics.source_delivery_fps);
        }
        // The engine's texture belongs to the feed that just ended. Holding it
        // past that would hand a dead surface to the next item that attaches.
        releaseEngineSource();
        if (dxgi_source_ != nullptr)
            dxgi_source_->ReturnEngineLease();
        if (wgc_source_ != nullptr)
            wgc_source_->ReturnEngineLease();
        // Losing the engine feed removes the exemption in applyPreviewRunState():
        // a preview that was kept alive only because the recording owned the
        // capture must now suspend if its surface is still not visible.
        applyPreviewRunState();
    }
}

void RecordPreviewAdapter::observeRecordingStats(const exosnap::engine::SessionStats& stats) {
    recording_captured_frames_ = stats.video_frames_captured;
    recording_encoded_packets_ = stats.encoded_video_packets;
    // No signal. Neither of these two counters backs a Q_PROPERTY: they are read
    // only by benchmarkSnapshot(), which the A/B harness pulls on demand. This
    // ran on the stats callback at ~8 Hz and invalidated every binding on
    // presentationRate, sourceDeliveryRate, frameTimeP95Ms, frameTimeP99Ms,
    // submitP95Us, consumedFrames, mutexMisses and recordingDroppedFrames —
    // eight properties, none of which this function touches.
}

void RecordPreviewAdapter::observeRecordingDiagnostics(const exosnap::engine::RecordingDiagnosticsSnapshot& snapshot) {
    const qulonglong dropped = snapshot.capture.frames_dropped_problem();
    if (recording_dropped_frames_ == dropped)
        return;
    recording_dropped_frames_ = dropped;
    emit metricsChanged();
}

void RecordPreviewAdapter::requestReadyFrame(ReadyFrameComposition composition,
                                             ReadyFrameCaptureService::Callback callback) {
    HANDLE duplicate = nullptr;
    if (ready_source_handle_ == nullptr || !frame_ready_ ||
        DuplicateHandle(GetCurrentProcess(), ready_source_handle_, GetCurrentProcess(), &duplicate, 0, FALSE,
                        DUPLICATE_SAME_ACCESS) == FALSE) {
        callback(false, 0, 0, {}, QStringLiteral("No Ready preview frame is available"));
        return;
    }
    ReadyFrameSource source;
    source.shared_handle = duplicate;
    source.width = ready_source_width_;
    source.height = ready_source_height_;
    source.tap = ready_source_tap_;
    source.target = ready_source_target_;
    source.cursor_already_composited = ready_source_cursor_composited_;
    ReadyFrameCaptureService::Capture(ready_frame_pool_, std::move(source), std::move(composition),
                                      std::move(callback));
}

std::function<void()> RecordPreviewAdapter::makeFramePublishedSink() {
    QPointer<RecordPreviewAdapter> safe_self(this);
    auto scheduler = update_scheduler_;
    return [safe_self, scheduler]() {
        // Producer thread — a capture pump or the engine's video thread. One
        // atomic exchange in the common case, one posted event when a wake-up
        // is not already in flight. Touches no QObject: the receiver below is
        // unconditionally the application object, for the same reason the
        // handle sink in startPreview() explains.
        if (!scheduler->ArmWake())
            return;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [safe_self, scheduler]() {
                // Re-arm BEFORE requesting the update, and unconditionally: a
                // publish landing during the request must produce a fresh
                // wake-up rather than be swallowed, and a wake-up that finds
                // nothing alive must still leave the gate open.
                scheduler->DisarmWake();
                if (safe_self == nullptr || safe_self->item_ == nullptr)
                    return;
                scheduler->RecordSceneUpdateRequested();
                safe_self->item_->requestSceneUpdate();
            },
            Qt::QueuedConnection);
    };
}

void RecordPreviewAdapter::acceptRecordingTexture(void* handle, uint32_t width, uint32_t height,
                                                  exosnap::engine::PreviewTapDesc tap) {
    if (handle == nullptr)
        return;
    ++engine_source_announcements_;

    // The adapter, not the item, owns the engine's source for the whole feed.
    //
    // The engine announces its shared texture EXACTLY ONCE per session: the
    // handle callback fires inside the "texture does not exist yet" branch, and
    // nothing re-announces. Handing that one handle straight to whatever item
    // happened to exist at that instant made the preview's whole session depend
    // on a race — the announcement arrives on the GUI thread, and if the Record
    // page was not current right then, the handle was closed and silently
    // dropped. The engine kept publishing into a texture with no consumer, the
    // node kept polling the hub texture nobody publishes into any more, and both
    // sides then missed their complementary keys for the rest of the run.
    //
    // Keeping the canonical handle here also survives what a one-shot handover
    // cannot: an item destroyed and rebuilt, a page left and returned to, a
    // scene-graph rebuild, or a late queued delivery.
    releaseEngineSource();
    engine_source_.handle = handle;
    engine_source_.width = width;
    engine_source_.height = height;
    engine_source_.tap = tap;
    ++engine_source_epoch_;
    if (previewTraceEnabled()) {
        qInfo("preview-trace: engine-source-announced epoch=%llu size=%ux%u active=%d item=%d", engine_source_epoch_,
              width, height, active_ ? 1 : 0, item_ != nullptr ? 1 : 0);
    }
    presentEngineSourceIfPossible();
}

void RecordPreviewAdapter::presentEngineSourceIfPossible() {
    if (engine_source_.handle == nullptr)
        return;
    if (!active_ || item_ == nullptr) {
        // Held, not dropped. Counted apart from an announcement that never
        // arrived, because the two need completely different investigations.
        ++engine_source_deferrals_;
        return;
    }

    // The item takes ownership of what it is given and closes it once opened, so
    // it gets a duplicate and the canonical handle stays here for the next item.
    HANDLE duplicate = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(engine_source_.handle), GetCurrentProcess(),
                        &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) == FALSE) {
        setStatus(QStringLiteral("Preview unavailable · could not duplicate the recording texture"));
        return;
    }

    item_->presentSharedTexture(duplicate, engine_source_.width, engine_source_.height, engine_source_.tap);
    ++recording_texture_generations_;
    ++engine_source_presentations_;
    if (previewTraceEnabled()) {
        qInfo("preview-trace: engine-source-presented epoch=%llu presentations=%llu deferrals=%llu",
              engine_source_epoch_, engine_source_presentations_, engine_source_deferrals_);
    }
    setStatus(QStringLiteral("Live · recording WYSIWYG texture"));
}

void RecordPreviewAdapter::releaseEngineSource() {
    if (engine_source_.handle == nullptr)
        return;
    CloseHandle(static_cast<HANDLE>(engine_source_.handle));
    engine_source_ = EngineSource{};
}

void RecordPreviewAdapter::startPreview() {
    stopPreview();
    if (!active_ || item_ == nullptr)
        return;

    exosnap::engine::CaptureTarget target;
    const bool available = selected_target_.has_value() && selected_target_->native_id != 0;
    if (available)
        target = *selected_target_;
    if (source_available_ != available) {
        source_available_ = available;
        emit sourceAvailableChanged();
    }
    if (!available) {
        source_name_.clear();
        emit sourceNameChanged();
        setStatus(QStringLiteral("No capture source is selected"));
        // Status, not error. Having chosen nothing yet is the expected empty state,
        // and the Record page already fills the stage with its placeholder --
        // "Choose what to record" plus a Choose source button. A second sentence
        // saying the same thing was drawn centred on the same stage, so the two
        // overlapped, and the caution colour made an ordinary starting point look
        // like something had gone wrong.
        setError({});
        return;
    }

    const QString name = targetDescription(target);
    if (source_name_ != name) {
        source_name_ = name;
        emit sourceNameChanged();
    }
    setError({});
    setStatus(QStringLiteral("Opening real DXGI preview source…"));
    const quint64 epoch = source_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    QPointer<RecordPreviewAdapter> safe_self(this);
    QPointer<ExoPreviewItem> safe_item(item_);
    const auto cursor_composited = std::make_shared<std::atomic_bool>(false);
    const auto sink = [safe_self, safe_item, epoch, target, cursor_composited](
                          void* handle, uint32_t width, uint32_t height, exosnap::engine::PreviewTapDesc tap) {
        auto handle_owner = std::make_shared<QueuedSharedHandle>(handle);
        // The receiver is unconditionally the application object, never the item.
        // This lambda runs on the capture pump thread, and Unsubscribe() is
        // explicitly NOT a barrier (DxgiCaptureHubService.h) -- frames already in
        // flight still arrive. Reading `safe_item` here to *pick* the receiver
        // therefore raced ~ExoPreviewItem: the QPointer could test non-null and
        // the item be freed before invokeMethod dereferenced it. The in-lambda
        // null check below still gives the intended "drop it" semantics, and it
        // runs on the GUI thread where the answer cannot change under it.
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [safe_self, safe_item, handle_owner, width, height, tap, epoch, target, cursor_composited]() {
                if (safe_self == nullptr || safe_item == nullptr ||
                    safe_self->source_epoch_.load(std::memory_order_acquire) != epoch || !safe_self->active_ ||
                    safe_self->engine_feed_expected_.load(std::memory_order_acquire)) {
                    return;
                }
                void* raw_handle = std::exchange(handle_owner->handle, nullptr);
                HANDLE snapshot_handle = nullptr;
                if (DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(raw_handle), GetCurrentProcess(),
                                    &snapshot_handle, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE) {
                    if (safe_self->ready_source_handle_ != nullptr)
                        CloseHandle(safe_self->ready_source_handle_);
                    safe_self->ready_source_handle_ = snapshot_handle;
                    safe_self->ready_source_width_ = width;
                    safe_self->ready_source_height_ = height;
                    safe_self->ready_source_tap_ = tap;
                    safe_self->ready_source_target_ = target;
                    safe_self->ready_source_cursor_composited_ = cursor_composited->load(std::memory_order_acquire);
                }
                safe_item->presentSharedTexture(raw_handle, width, height, tap);
                safe_self->setStatus(QStringLiteral("Waiting for first GPU frame…"));
            },
            Qt::QueuedConnection);
    };
    bool subscribed = false;
    if (target.kind == exosnap::engine::CaptureTarget::Kind::Monitor && dxgi_source_ != nullptr) {
        subscribed =
            dxgi_source_->Subscribe(reinterpret_cast<HMONITOR>(target.native_id), sink, makeFramePublishedSink());
    }
    if (!subscribed && wgc_source_ != nullptr) {
        cursor_composited->store(true, std::memory_order_release);
        CaptureSourceKey key;
        key.kind = target.kind == exosnap::engine::CaptureTarget::Kind::Window ? CaptureSourceKey::Kind::Window
                                                                               : CaptureSourceKey::Kind::Monitor;
        key.native_id = target.native_id;
        subscribed = wgc_source_->Subscribe(std::move(key), sink, makeFramePublishedSink());
    }
    if (!subscribed) {
        source_epoch_.fetch_add(1, std::memory_order_acq_rel);
        setStatus(QStringLiteral("Preview unavailable"));
        setError(QStringLiteral("The selected capture source could not be opened on Qt Quick's D3D11 adapter."));
        return;
    }
    metrics_timer_.start();
}

void RecordPreviewAdapter::stopPreview() {
    source_epoch_.fetch_add(1, std::memory_order_acq_rel);
    if (dxgi_source_ != nullptr)
        dxgi_source_->Unsubscribe();
    if (wgc_source_ != nullptr)
        wgc_source_->Unsubscribe();
    if (ready_source_handle_ != nullptr) {
        CloseHandle(ready_source_handle_);
        ready_source_handle_ = nullptr;
    }
    ready_source_width_ = 0;
    ready_source_height_ = 0;
    metrics_timer_.stop();
    if (item_ != nullptr)
        item_->clearSharedTexture();
    if (frame_ready_) {
        frame_ready_ = false;
        emit frameReadyChanged();
    }
    if (!source_size_.isEmpty()) {
        source_size_ = {};
        emit sourceSizeChanged();
    }
    if (!preview_running_)
        setStatus(QStringLiteral("Preview inactive"));
}

void RecordPreviewAdapter::synchronizeItemState() {
    if (item_ == nullptr)
        return;
    const bool ready = item_->frameReady();
    if (frame_ready_ != ready) {
        frame_ready_ = ready;
        emit frameReadyChanged();
    }
    const QSize size = item_->sourceSize();
    if (source_size_ != size) {
        source_size_ = size;
        emit sourceSizeChanged();
    }
    setError(item_->errorText());
    if (!error_text_.isEmpty())
        setStatus(QStringLiteral("Preview unavailable"));
    else if (frame_ready_)
        setStatus(engine_feed_expected_.load(std::memory_order_acquire)
                      ? QStringLiteral("Live · recording WYSIWYG texture")
                      : QStringLiteral("Live · D3D11 scene texture"));
}

void RecordPreviewAdapter::updateMetrics() {
    const PreviewMetricsSnapshot metrics = item_ != nullptr ? item_->metricsSnapshot() : PreviewMetricsSnapshot{};
    // Bit-equality, not an epsilon. These are percentile READINGS, not measured
    // quantities being compared for closeness: a percentile over a sample window
    // is either the same sample as last time or a different one, and inventing a
    // tolerance here would silently stop reporting a real drift. `==` on doubles
    // is exactly the right question, and the same one every other setter in this
    // class asks of its own value.
    const bool changed = presentation_rate_ != metrics.scene_fps ||
                         source_delivery_rate_ != metrics.source_delivery_fps ||
                         frame_time_p95_ms_ != metrics.scene_frame_ms_p95 ||
                         frame_time_p99_ms_ != metrics.scene_frame_ms_p99 || submit_p95_us_ != metrics.submit_us_p95 ||
                         consumed_frames_ != metrics.consumed_frames || mutex_misses_ != metrics.mutex_misses;
    presentation_rate_ = metrics.scene_fps;
    source_delivery_rate_ = metrics.source_delivery_fps;
    frame_time_p95_ms_ = metrics.scene_frame_ms_p95;
    frame_time_p99_ms_ = metrics.scene_frame_ms_p99;
    submit_p95_us_ = metrics.submit_us_p95;
    consumed_frames_ = metrics.consumed_frames;
    mutex_misses_ = metrics.mutex_misses;
    if (changed)
        emit metricsChanged();
}

void RecordPreviewAdapter::setStatus(QString status) {
    if (status_text_ == status)
        return;
    status_text_ = std::move(status);
    emit statusTextChanged();
}

void RecordPreviewAdapter::setError(QString error) {
    if (error_text_ == error)
        return;
    error_text_ = std::move(error);
    emit errorTextChanged();
}

} // namespace exosnap::quick
