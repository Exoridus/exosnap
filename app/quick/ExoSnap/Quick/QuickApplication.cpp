#include "QuickApplication.h"

#include "QuickWindowChrome.h"

#include "services/DisplayIdentityEnumerator.h"
#include "services/DisplayIdentityResolver.h"
#include "services/RecordingCoordinator.h"
#include "services/TargetDisplayFacts.h"
#include "services/UpdateService.h"

#include "diagnostics/AppLog.h"
#include "diagnostics/ConfigSummary.h"
#include "diagnostics/CrashSessionContext.h"
#include "diagnostics/ElevationProvider.h"
#include "diagnostics/FixActionDispatcher.h"
#include "diagnostics/StartupClock.h"
#include "models/CompletedRecording.h"
#include "models/EditContextFactory.h"
#include "models/FrameRateLimits.h"
#include "models/HotkeyStartupConflicts.h"
#include "models/OutputSettingsModel.h"
#include "models/VideoSettingsModel.h"
#include "ui/CodecLabels.h"
#include "ui/theme/ExoSnapMetrics.h"
#include "visual_tests/RecordVisualStateNames.h"

#include "ExoSnapBuildInfo.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QQuickWindow>
#include <QScreen>
#include <QStandardPaths>
#include <QUrl>
#include <QWindow>
// The one Qt Widgets type the Quick frontend keeps (cutover plan §35): Qt has no
// Quick/QML equivalent for a system tray icon, and a native Shell_NotifyIcon
// replacement is post-1.0 work with no product benefit.
#include <QSystemTrayIcon>
#include <QThread>
#include <QVariant>
#include <QVariantMap>

#include <update/install_mode_detector.h>
// The private message the staged updater posts to ask this process to close for
// the swap. One header, both sides -- a second copy of the number would compile
// and never arrive.
#include <update/update_handoff.h>

#include <capability/capability_builder.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iterator>
#include <optional>
#include <utility>
#include <windows.h>

namespace exosnap::quick {
namespace {

models::AboutInfo buildAboutInfo(const PersistedAppSettings& settings) {
    const bool is_scoop = UpdateService::IsScoopManagedInstall(QCoreApplication::applicationDirPath());
    return models::BuildAboutInfo(settings.update_channel, exosnap::update::DetectInstallMode(), is_scoop);
}

// Labels come from ui/CodecLabels.h -- the documented single source of truth for
// codec/container spelling (feedback_codec_naming_canon). The local copies this
// replaces had already drifted: their audio fallback was "Opus" where the canon
// says "AAC", and the video labels were hardcoded strings rather than the shared
// capability::VisibleVideoCodecLabel spelling.
QString formatLabel(const RecordingPresetConfig& config) {
    const QString timing =
        config.video.frame_rate_den == 1
            ? QStringLiteral("%1 fps").arg(config.video.frame_rate_num)
            : QStringLiteral("%1/%2 fps").arg(config.video.frame_rate_num).arg(config.video.frame_rate_den);
    return QStringLiteral("%1 %2 · %3 · %4 · %5")
        .arg(timing, config.video.cfr ? QStringLiteral("CFR") : QStringLiteral("VFR"),
             ui::videoCodecLabel(config.output.video_codec), ui::audioCodecLabel(config.output.audio_codec),
             ui::containerLabel(config.output.container));
}

double dockLevel(float rms) {
    if (rms <= 0.0f)
        return 0.0;
    const double db = std::max(-60.0, 20.0 * std::log10(static_cast<double>(rms)));
    return std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
}

bool isSystemRow(const recorder_core::AudioSourceRow& row) {
    return row.kind == recorder_core::AudioSourceKind::Sys || row.kind == recorder_core::AudioSourceKind::SystemOutput;
}

bool locksCaptureTargets(UiRecordingState state) {
    return state == UiRecordingState::Countdown || state == UiRecordingState::Preparing ||
           state == UiRecordingState::Recording || state == UiRecordingState::Paused ||
           state == UiRecordingState::Stopping || state == UiRecordingState::Saving ||
           state == UiRecordingState::RegionSelecting;
}

#if defined(Q_OS_WIN)
// Routes WM_HOTKEY for the Quick window's HWND back to the composition root.
// Global hotkeys are a Win32 message-loop concern, so this stays native rather
// than becoming a QML type.
class QuickHotkeyEventFilter : public QAbstractNativeEventFilter {
  public:
    QuickHotkeyEventFilter(HWND hwnd, std::function<void(HotkeyAction)> on_action)
        : hwnd_(hwnd), on_action_(std::move(on_action)) {
    }

    bool nativeEventFilter(const QByteArray& event_type, void* message, qintptr* result) override {
        if (event_type != "windows_generic_MSG" || message == nullptr) {
            return false;
        }
        auto* msg = static_cast<MSG*>(message);
        if (msg->hwnd != hwnd_ || msg->message != WM_HOTKEY) {
            return false;
        }
        const int id = static_cast<int>(msg->wParam);
        for (int i = 0; i < kHotkeyActionCount; ++i) {
            const auto action = static_cast<HotkeyAction>(i);
            if (id == GlobalHotkeyService::Win32IdForAction(action)) {
                on_action_(action);
                if (result != nullptr) {
                    *result = 0;
                }
                return true;
            }
        }
        return false;
    }

  private:
    HWND hwnd_ = nullptr;
    std::function<void(HotkeyAction)> on_action_;
};

// The staged updater's marked close request, routed to the composition root.
//
// This is the only way the updater can ask the running application to release
// its own executable, and it is deliberately a PRIVATE message with a magic
// wParam rather than WM_CLOSE: WM_CLOSE means "the user closed the window",
// which for ExoSnap means close-to-tray, and a tray-resident process still holds
// its image locked. The updater targets the top-level window by owner pid AND
// exact title, so the strict hwnd check here is the same identity from the other
// side.
class QuickUpdaterHandoffFilter : public QAbstractNativeEventFilter {
  public:
    QuickUpdaterHandoffFilter(HWND hwnd, std::function<void()> on_handoff)
        : hwnd_(hwnd), on_handoff_(std::move(on_handoff)) {
    }

    bool nativeEventFilter(const QByteArray& event_type, void* message, qintptr* result) override {
        if (message == nullptr)
            return false;
        if (event_type != QByteArrayLiteral("windows_generic_MSG") &&
            event_type != QByteArrayLiteral("windows_dispatcher_MSG")) {
            return false;
        }
        auto* msg = static_cast<MSG*>(message);
        if (msg->hwnd != hwnd_ || msg->message != static_cast<UINT>(exosnap::update::kUpdaterHandoffMessage))
            return false;
        // The magic word is what makes this OURS. WM_APP-range values are only
        // private by convention, so a foreign process posting the same number
        // must not be able to end this one.
        if (msg->wParam != static_cast<WPARAM>(exosnap::update::kUpdaterHandoffMagic))
            return false;
        on_handoff_();
        if (result != nullptr)
            *result = 0;
        return true;
    }

  private:
    HWND hwnd_ = nullptr;
    std::function<void()> on_handoff_;
};
#endif

QString captureSourceUnavailableNotice() {
    return QStringLiteral("The selected capture source is no longer available. Choose another source.");
}

// First-launch window size, centred on the primary screen by
// ResolveWindowGeometry. Only used when nothing has been persisted yet; the
// number itself is a product decision and lives with the window minimum in
// ExoSnapMetrics rather than as a second literal here.
constexpr int kDefaultWindowWidth = ui::theme::ExoSnapMetrics::kPreferredWindowWidth;
constexpr int kDefaultWindowHeight = ui::theme::ExoSnapMetrics::kPreferredWindowHeight;

} // namespace

QuickApplication::QuickApplication()
    : settings_(settings_store_.Load()), recovery_service_(recovery_manifest_store_),
      about_view_model_(buildAboutInfo(settings_)), record_view_model_adapter_(&record_view_model_),
      overlay_adapter_(&record_view_model_), recording_coordinator_(std::make_unique<RecordingCoordinator>()),
      webcam_frame_provider_(new RecordWebcamFrameProvider), edit_tile_provider_(new EditTimelineTileProvider) {
    // QCR-201. Latched here for the same reason preset_store_repaired_ is: the
    // load runs in the constructor, long before initializeNotifications() exists
    // to report it. The write block itself needs no latch — it follows from
    // settings_.load_outcome and takes effect immediately, which matters because
    // the first incidental write (a startup reconciliation, window geometry) can
    // happen well before the notification manager is up.
    settings_load_failed_pending_ = settings_.load_outcome == SettingsLoadOutcome::ReadFailed;
    const PersistedPresetState persisted = preset_store_.Load();
    preset_registry_.LoadState(persisted.user_presets,
                               persisted.selected_id.empty() ? std::string(kDefaultPresetId) : persisted.selected_id);
    live_config_ = persisted.live.has_value() ? SanitizePresetConfig(*persisted.live) : MakeDefaultPreset().config;
    // Latched rather than raised here: the notification manager is not wired up
    // until initializeNotifications(). Silently repairing a user's saved
    // configuration and never saying so is exactly the behaviour the product
    // rules out -- the Widgets shell surfaces it, and this frontend was dropping
    // the store's `repaired` flag on the floor.
    preset_store_repaired_ = persisted.repaired;
    // AppLog::init() ran in the bootstrap with the "record everything" default,
    // so early-startup entries are unaffected; this narrows the filter now that
    // the persisted level is known.
    applyDeveloperLogLevel();
    initializeCrashSession();
    initializeRecordWorkflow();
    initializeSettingsArea();
    // Device only needs the static, system-wide capability declarations up
    // front; its per-adapter scan stays lazy and starts on first navigation.
    device_adapter_.setCapabilitySet(capabilities_);
    initializeDiagnosticsArea();
    initializeEditArea();
    initializeShell();
    initializeNotifications();
    // After notifications: the scan raises a standing "Recover last session?"
    // entry, so the manager has to exist first. Before the window loads, so the
    // surface is already up on the first frame rather than appearing over an
    // application the user has started using.
    // Before recovery: the scan below can raise the recovery surface, and the
    // arbiter has to own that decision by then.
    initializeBlockingSurfaces();
    initializeRecovery();
    initializeRecordingError();
    // After recovery: a crash mid-recording produces BOTH a recovery candidate
    // and a crash prompt, and the two must not stack. Recovery goes first — it
    // is about the user's work, the crash report is about ours.
    initializeCrashReport();
    // After notifications: an available update publishes a hub entry, so the
    // manager has to be wired before the first check can complete.
    initializeUpdates();
}

QuickApplication::~QuickApplication() {
    // Same hazard, same shape as the hotkey filter below: QCoreApplication holds
    // a raw pointer to it. A run that never produced a frame (a load failure, a
    // --smoke-test that quits first) never reached the probe that normally stops
    // it, so it is unhooked here rather than left for static destruction.
    StopStartupMessageTrace();
#if defined(Q_OS_WIN)
    // FIRST, before anything else can be freed. QCoreApplication holds a raw
    // pointer to this filter, and the destruction order here is the wrong way
    // round: QGuiApplication in main() outlives QuickApplication, so a Win32
    // message dispatched during Qt's own teardown -- or by the ShellExecuteEx in
    // ProductionBootstrap's pending elevated relaunch, which pumps -- would reach
    // a freed filter. Same shape QuickWindowChrome::detach() already uses.
    if (hotkey_event_filter_)
        QCoreApplication::instance()->removeNativeEventFilter(hotkey_event_filter_.get());
    if (updater_handoff_filter_)
        QCoreApplication::instance()->removeNativeEventFilter(updater_handoff_filter_.get());
    // The service keeps a bare pointer to the registrar, which is a member below
    // it; unhook before either can be destroyed.
    (void)hotkey_service_.SetRegistrar(nullptr);
#endif
    recording_coordinator_->SetReadyFrameRequester({});
    // Clearing the requester only refuses NEW requests. A Ready-frame worker that
    // is already running holds a pointer into RecordingCoordinator::snapshot_pool_,
    // and the coordinator is destroyed before record_preview_adapter_ -- so the
    // join has to happen here, not in the adapter's own destructor.
    record_preview_adapter_.waitForPendingReadyFrames();
    // Stop the camera before dropping the callback. This is no longer a safety
    // requirement -- WebcamService publishes its registration as an immutable
    // snapshot, so clearing it can no longer destroy a closure a live reader is
    // holding -- but it stays the right order: a camera nobody listens to has no
    // reason to keep running for the rest of teardown.
    recording_coordinator_->SetWebcamPreviewActive(false);
    recording_coordinator_->SetWebcamFrameCallback({});
    if (webcam_overlay_persist_timer_.isActive())
        persistLiveConfig();
    // A plain close never reaches closeApproved -- ShellAdapter::requestClose
    // returns true and the window goes away without the guard chain running --
    // so the catch-all flush has to be here. Reads a cached rect, not the
    // window, and is therefore safe after the engine is gone.
    if (window_geometry_)
        window_geometry_->flush();
    if (!webcam_provider_registered_)
        delete webcam_frame_provider_;
    if (!edit_tile_provider_registered_)
        delete edit_tile_provider_;
}

void QuickApplication::applyDeveloperLogLevel() {
    diagnostics::AppLog::setMinSeverity(diagnostics::DeveloperLogLevelFromString(settings_.developer_log_level));
}

void QuickApplication::applyCrashReportPolicy() {
#if defined(Q_OS_WIN)
    // The three policies map onto the three persisted SDK consent states. This
    // has to run before anything in the process could offer to send a report,
    // otherwise an AlwaysSend user silently uploads nothing and a NeverSend user
    // keeps whatever consent an earlier session left behind.
    switch (settings_.crash_report_policy) {
    case CrashReportPolicy::AskEveryTime:
        crash_capture::ResetUserConsent();
        break;
    case CrashReportPolicy::AlwaysSend:
        crash_capture::GiveUserConsent();
        break;
    case CrashReportPolicy::NeverSend:
        crash_capture::RevokeUserConsent();
        break;
    }
#endif
}

void QuickApplication::initializeCrashSession() {
#if defined(Q_OS_WIN)
    // Reconcile the SDK-wide persisted consent with the explicit app policy
    // before any report path is reachable in this process.
    applyCrashReportPolicy();
    // ADR 0017. crash_capture::Initialize() already ran in the bootstrap; this
    // owns the session sidecar.
    //
    // ORDER IS CRITICAL: read the previous session's crash context BEFORE
    // BeginSession overwrites the sidecar with this session's marker. Honest
    // crash detection is "the previous session never marked a clean exit", which
    // works even in the OFF/stub build with no Crashpad behind it.
    crash_dir_ = crash_capture::ResolveCrashDir();
    if (crash_dir_.empty()) {
        diagnostics::AppLog::warning(QStringLiteral("crash"),
                                     QStringLiteral("Crash dir unavailable — session tracking disabled"));
        return;
    }
    pending_crash_ = crash_capture::ReadPreviousCrashContext(crash_dir_);
    if (pending_crash_) {
        diagnostics::AppLog::warning(QStringLiteral("crash"), QStringLiteral("Previous session did not exit cleanly"));
    }
    crash_capture::BeginSession(crash_dir_, currentCrashSessionContext());
    refreshCrashSessionContext();
#endif
}

crash_capture::SessionContext QuickApplication::currentCrashSessionContext() const {
    return diagnostics::MakeCrashSessionContext(live_config_.output.container, live_config_.output.video_codec,
                                                live_config_.output.audio_codec);
}

void QuickApplication::refreshCrashSessionContext() {
#if defined(Q_OS_WIN)
    if (crash_dir_.empty())
        return;
    // A dump is triaged by grouping on these facts, so they have to follow the
    // live configuration rather than the one that happened to be loaded at
    // startup — a crash after a container switch must not report the old one.
    const crash_capture::SessionContext ctx = currentCrashSessionContext();
    crash_capture::UpdateSessionContext(crash_dir_, ctx);
    crash_capture::SetEncoderContext(ctx.encoder_backend, ctx.container, ctx.video_codec, ctx.audio_codec);
#endif
}

void QuickApplication::initializeRecordWorkflow() {
    // 150 ms: long enough that a page switch has painted and a Record → Settings
    // → Record detour coalesces into one start, short enough that a user who
    // lands on Record and looks straight at the level bars sees them fill within
    // the same glance. Restarted (not merely armed) on every request, so the
    // interval measures the time since the LAST decision, not the first.
    meter_service_start_timer_.setSingleShot(true);
    meter_service_start_timer_.setInterval(150);
    meter_service_start_timer_.setTimerType(Qt::CoarseTimer);
    QObject::connect(&meter_service_start_timer_, &QTimer::timeout, &record_view_model_adapter_,
                     [this]() { startMeterServices(); });

    meter_update_timer_.setSingleShot(true);
    meter_update_timer_.setInterval(33);
    QObject::connect(&meter_update_timer_, &QTimer::timeout, &record_view_model_adapter_, [this]() { updateMeters(); });
    webcam_frame_delivery_timer_.setSingleShot(true);
    webcam_frame_delivery_timer_.setInterval(33);
    QObject::connect(&webcam_frame_delivery_timer_, &QTimer::timeout, &record_view_model_adapter_, [this]() {
        if (!record_view_model_adapter_.active())
            return;
        ++webcam_frame_revision_;
        record_view_model_adapter_.setWebcamFrameSource(
            QStringLiteral("image://record-webcam/frame?revision=%1").arg(webcam_frame_revision_));
    });
    webcam_overlay_persist_timer_.setSingleShot(true);
    webcam_overlay_persist_timer_.setInterval(250);
    QObject::connect(&webcam_overlay_persist_timer_, &QTimer::timeout, &record_view_model_adapter_,
                     [this]() { persistLiveConfig(); });

    capture_evidence_timer_.setInterval(1000);
    capture_evidence_timer_.setTimerType(Qt::CoarseTimer);
    QObject::connect(&capture_evidence_timer_, &QTimer::timeout, &record_view_model_adapter_,
                     [this]() { refreshCaptureWindowEvidence(); });

    // QCR-110: the admission gate's evidence producer. Read on the UI thread
    // inside StartRecording; the call takes only the probe's own mutex and copies
    // a plain struct, so it never blocks on the native probe and never touches
    // GUI state from the recording worker.
    recording_coordinator_->SetWindowExclusiveEvidenceProvider(
        [this](const recorder_core::CaptureTarget& target) { return resolveWindowExclusiveEvidence(target); });

    recording_coordinator_->SetRecoveryManifestStore(&recovery_manifest_store_);
    recording_coordinator_->SetOutputSettings(live_config_.output);
    recording_coordinator_->SetVideoSettings(live_config_.video);
    recording_coordinator_->SetWebcamSettings(live_config_.webcam);
    recording_coordinator_->SetSplitSettings(
        {SplitDurationMs(live_config_.output.split), SplitSizeBytes(live_config_.output.split)});

    record_preview_adapter_.bindRecordingCoordinator(recording_coordinator_.get());
    recording_coordinator_->SetReadyFrameRequester([this](RecordingCoordinator::ReadyFrameCallback callback) {
        ReadyFrameComposition composition;
        composition.normalized_source_rect = record_view_model_adapter_.normalizedSourceRect();
        composition.video = live_config_.video;
        composition.webcam = live_config_.webcam;
        if (webcam_frame_provider_ != nullptr)
            composition.webcam_frame = webcam_frame_provider_->latestFrame();
        record_preview_adapter_.requestReadyFrame(std::move(composition), std::move(callback));
    });
    wireRecordCommands();

    recording_coordinator_->SetStateChangedCallback([this](UiRecordingState state) {
        record_preview_adapter_.observeRecordingState(state);
        const UiRecordingState previous = record_view_model_.state;
        // A seeded visual scenario owns the state for the rest of the process.
        // Only the assignment is suppressed, not the bookkeeping below it: this
        // is the coordinator reporting reality, and the harness is overriding
        // only what the capture is supposed to show.
        if (!visualScenarioLatched())
            record_view_model_.SetState(state);
        record_view_model_.capability_status_text = recording_coordinator_->CapabilityStatusText();
        // A fresh start (not a resume) invalidates the previous session's
        // post-flight numbers. They feed the Edit surface's report badge, so
        // carrying them over would attribute one recording's drops to the next.
        if (state == UiRecordingState::Recording && previous != UiRecordingState::Paused) {
            peak_av_drift_ms_ = 0.0;
            av_drift_ever_available_ = false;
            last_completed_snapshot_ = {};
            // QCR-804: same reason. A stall belongs to the session that had it —
            // neither the latch nor a leftover toast may cross into this one.
            capture_stall_monitor_.Reset();
            clearWindowCaptureStallWarning();
            // ADR 0046: same reason. A previous recording's audio outage may not
            // arrive standing over this one.
            audio_degradation_monitor_.Reset();
            clearAudioSourceDegradedWarning();
        }
        // The standing stall notice says "the recording is still running". Once
        // the session leaves Recording/Paused that is no longer true, so the toast
        // goes even though the hub keeps the record. The audio-degradation notice
        // makes the same claim ("Recording continues") and clears on the same
        // edge — which is also product-spec's "clears ... or the recording ends".
        if (state != UiRecordingState::Recording && state != UiRecordingState::Paused) {
            clearWindowCaptureStallWarning();
            clearAudioSourceDegradedWarning();
        }
        // Keyed off the state the UI is actually showing, not the one just
        // reported. In production the two are the same value — SetState assigned
        // it one line up. Under a latched visual scenario they are not, and
        // reading the coordinator's here zeroed the remaining seconds out from
        // under a Countdown the capture was told to render.
        if (record_view_model_.state != UiRecordingState::Countdown) {
            countdown_timer_.stop();
            countdown_.reset();
            countdown_remaining_ = 0;
            countdown_progress_ = 0.0;
        }
        updateMeterServices();
        synchronizeRecordState();
        if (!locksCaptureTargets(state) && capture_target_refresh_pending_) {
            capture_target_refresh_pending_ = false;
            refreshCaptureTargets(capture_target_notifier_.currentSnapshot(), DiscoveryReason::Rescan);
        }
    });
    recording_coordinator_->SetStatsUpdatedCallback([this](const recorder_core::SessionStats& stats) {
        record_view_model_.UpdateStats(stats);
        record_preview_adapter_.observeRecordingStats(stats);
        updateMeters();
        synchronizeRecordState();
    });
    recording_coordinator_->SetDiagnosticsCallback([this](const recorder_core::RecordingDiagnosticsSnapshot& snapshot) {
        record_view_model_.av_drift_available =
            snapshot.av_drift_availability == recorder_core::MetricAvailability::Available;
        record_view_model_.av_drift_ms = record_view_model_.av_drift_available ? snapshot.av_drift_ms : 0.0;
        record_view_model_.dropped_frames = snapshot.capture.frames_dropped_problem();
        // Peak A/V drift is accumulated in the engine aggregator (one source of
        // truth shared with the session report); latch the availability so a
        // metric that stopped being reported before the last frame still counts.
        if (snapshot.peak_av_drift_availability == recorder_core::MetricAvailability::Available) {
            av_drift_ever_available_ = true;
            peak_av_drift_ms_ = snapshot.peak_av_drift_ms;
        }
        if (snapshot.lifecycle == recorder_core::DiagnosticsLifecycle::Completed)
            last_completed_snapshot_ = snapshot;
        record_preview_adapter_.observeRecordingDiagnostics(snapshot);
        // The coordinator holds exactly one diagnostics callback, so every
        // consumer has to be fanned out from this single registration. The
        // Diagnostics area used to install its own, which silently replaced this
        // one and left dropped_frames, av_drift and the Edit report badge at zero
        // for the whole session.
        diagnostics_adapter_.applyLiveDiagnostics(snapshot);
        observeWindowCaptureStall(snapshot);
        observeAudioSourceDegradation(snapshot);
        synchronizeRecordState();
    });
    recording_coordinator_->SetResultReadyCallback([this](const UiRecordingResult& result) {
        record_view_model_.SetResult(result);
        // Only a failure earns a page banner. A SUCCESSFUL stop used to add a
        // full-width "Recording saved · name.mkv" notice above the Preview
        // Surface, and because the preview is the page's fill-height element
        // that banner came straight out of its height — and, through the
        // aspect-ratio fit, out of its width as well. The user had been watching
        // that frame for the whole recording and the reward for finishing was
        // the entire composition jumping to announce something four other
        // things already say: the shell's status pill reads Completed, the
        // transport's recommended action becomes Edit, a "Recording saved" toast
        // carries the path and the Show-in-folder action, and the file is on
        // disk. A failure is different — it is unresolved, it is not stated
        // anywhere else on the page, and it is exactly what a persistent notice
        // is for.
        record_view_model_adapter_.setNoticeText(
            result.succeeded ? QString{} : QString::fromStdWString(record_view_model_.result_user_message),
            result.succeeded ? QStringLiteral("info") : QStringLiteral("error"));
        synchronizeRecordState();
        publishRecordingResultNotification(result);
        // One authoritative failure state: the result itself. The policy decides
        // whether this result deserves the surface at all (a disk-space auto-stop
        // does not — it already has its own actionable notification).
        if (const auto failure = models::BuildRecordingFailureReport(result)) {
            // Only an official build with a compiled-in DSN and active crash
            // capture can send anything, so only there is the action offered.
            presentRecordingFailure(*failure, crash_capture::IsActive());
        }
        // "Open editor when finished" (PersistedAppSettings): the overlay opening
        // by itself IS the post-recording feedback. The Widgets shell drove this
        // off its SAVED chrome transition; here the result callback is the same
        // edge, and openEditorForCurrentRecording() re-checks every gate.
        if (result.succeeded && settings_.open_editor_when_finished)
            openEditorForCurrentRecording();
    });
    QPointer<RecordViewModelAdapter> safe_record_adapter(&record_view_model_adapter_);
    recording_coordinator_->SetFrameCapturedCallback([safe_record_adapter](bool success, const QString& path,
                                                                           const QString& error) {
        if (safe_record_adapter == nullptr)
            return;
        safe_record_adapter->setNoticeText(success ? QStringLiteral("Frame saved · %1").arg(QFileInfo(path).fileName())
                                                   : (error.isEmpty() ? QStringLiteral("Frame capture failed") : error),
                                           success ? QStringLiteral("success") : QStringLiteral("error"));
    });
    recording_coordinator_->SetSplitFeedbackCallback([this](bool accepted, const QString& message) {
        if (!accepted)
            record_view_model_adapter_.setNoticeText(message);
        synchronizeRecordState();
    });
    // A recovery-manifest write that did not reach disk. The recording keeps
    // running — only the crash-recovery entry is missing — so this is reported
    // like any other completed local-write failure, never as a recording error.
    recording_coordinator_->SetRecoveryProtectionLostCallback([this](const QString& detail) {
        notifications::NotificationEvent event;
        event.type = notifications::NotificationType::RecoveryProtectionUnavailable;
        event.title = QStringLiteral("Recovery protection unavailable");
        event.body = QStringLiteral("This recording has no crash-recovery entry: %1. The recording itself is "
                                    "unaffected, but it cannot be recovered if ExoSnap is interrupted.")
                         .arg(detail);
        notifications_adapter_.manager().Enqueue(std::move(event));
    });
    recording_coordinator_->SetMicMeterUpdatedCallback([this](float rms) {
        preflight_microphone_rms_ = std::clamp(rms, 0.0f, 1.0f);
        scheduleMeterUpdate();
    });
    recording_coordinator_->SetSysMeterUpdatedCallback([this](float rms) {
        preflight_system_rms_ = std::clamp(rms, 0.0f, 1.0f);
        scheduleMeterUpdate();
    });
    recording_coordinator_->SetAppMeterUpdatedCallback([this](float rms) {
        preflight_app_rms_ = std::clamp(rms, 0.0f, 1.0f);
        scheduleMeterUpdate();
    });
    recording_coordinator_->SetRecordingMeterCallback([this](const std::array<float, 3>& rms) {
        record_view_model_.UpdateMeterRms(rms);
        scheduleMeterUpdate();
    });
    recording_coordinator_->SetWebcamStatusCallback(&record_view_model_adapter_,
                                                    [this](bool ok, const QString& reason) {
                                                        webcam_error_ = ok ? QString{} : reason;
                                                        synchronizeRecordState();
                                                    });
    recording_coordinator_->SetWebcamFrameCallback(&record_view_model_adapter_, [this](QImage frame) {
        if (webcam_frame_provider_ == nullptr)
            return;
        webcam_frame_provider_->submitFrame(std::move(frame));
        if (record_view_model_adapter_.active() && !webcam_frame_delivery_timer_.isActive())
            webcam_frame_delivery_timer_.start();
    });

    capture_target_notifier_.setEnumerator(
        [this]() { return CaptureTargetSnapshot{recording_coordinator_->EnumerateTargets()}; });
    capture_target_notifier_.start();
    record_view_model_.targets = capture_target_notifier_.currentSnapshot().targets;
    ++record_view_model_.targets_revision;
    record_view_model_.target_display_names.reserve(record_view_model_.targets.size());
    for (const auto& target : record_view_model_.targets)
        record_view_model_.target_display_names.push_back(
            QString::fromStdString(RecordViewModel::TargetLabelFromCaptureTarget(target)).toStdWString());

    record_view_model_.audio_ui_state = live_config_.audio;
    CaptureMode initial_mode = CaptureMode::Monitor;
    if (live_config_.capture.kind == PresetCaptureKind::Window)
        initial_mode = CaptureMode::Window;
    else if (live_config_.capture.kind == PresetCaptureKind::Region)
        initial_mode = CaptureMode::Region;

    int initial_index = -1;
    const std::vector<EnumeratedDisplayIdentity> displays =
        initial_mode == CaptureMode::Window ? std::vector<EnumeratedDisplayIdentity>{} : EnumerateDisplayIdentities();
    uintptr_t restored_monitor = 0;
    if (initial_mode != CaptureMode::Window && !live_config_.capture.display_id.empty()) {
        if (const auto match = ResolveStableDisplay(live_config_.capture.display_id, displays); match.has_value())
            restored_monitor = displays[match->index].hmonitor;
    }
    for (int index = 0; index < static_cast<int>(record_view_model_.targets.size()); ++index) {
        const auto& target = record_view_model_.targets[static_cast<std::size_t>(index)];
        const bool kind_matches = initial_mode == CaptureMode::Window
                                      ? target.kind == recorder_core::CaptureTarget::Kind::Window
                                      : target.kind == recorder_core::CaptureTarget::Kind::Monitor;
        if (!kind_matches)
            continue;
        if (initial_mode != CaptureMode::Window && !live_config_.capture.display_id.empty() &&
            target.native_id != restored_monitor) {
            continue;
        }
        if (initial_mode == CaptureMode::Window && !live_config_.capture.window_key.empty() &&
            RecordViewModel::TargetLabelFromCaptureTarget(target) != live_config_.capture.window_key) {
            continue;
        }
        initial_index = index;
        break;
    }
    const bool saved_display_missing =
        initial_mode != CaptureMode::Window && !live_config_.capture.display_id.empty() && restored_monitor == 0;
    if (initial_index < 0 && !saved_display_missing) {
        initial_mode = CaptureMode::Monitor;
        for (int index = 0; index < static_cast<int>(record_view_model_.targets.size()); ++index) {
            if (record_view_model_.targets[static_cast<std::size_t>(index)].kind ==
                recorder_core::CaptureTarget::Kind::Monitor) {
                initial_index = index;
                break;
            }
        }
    }
    if (saved_display_missing) {
        record_view_model_adapter_.setNoticeText(
            QStringLiteral("The saved display is not connected. Choose another capture source."));
    }
    const bool restore_region = initial_mode == CaptureMode::Region && live_config_.capture.has_region;
    const QRectF restored_region(live_config_.capture.region_x_norm, live_config_.capture.region_y_norm,
                                 live_config_.capture.region_w_norm, live_config_.capture.region_h_norm);
    selectTarget(initial_index, initial_mode);
    if (restore_region)
        selectRegion(restored_region);

    QObject::connect(&capture_target_notifier_, &CaptureTargetNotifier::snapshotChanged, &record_view_model_adapter_,
                     [this](const CaptureTargetSnapshot& snapshot, DiscoveryReason reason) {
                         refreshCaptureTargets(snapshot, reason);
                     });

    countdown_timer_.setInterval(100);
    countdown_timer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&countdown_timer_, &QTimer::timeout, &record_view_model_adapter_, [this]() { updateCountdown(); });

    QObject::connect(&record_preview_adapter_, &RecordPreviewAdapter::frameReadyChanged, &record_view_model_adapter_,
                     [this]() { synchronizeRecordState(); });
    QObject::connect(&audio_notifier_, &AudioDeviceNotifier::snapshotChanged, &record_view_model_adapter_,
                     [this](const AudioDeviceSnapshot& snapshot, DiscoveryReason) {
                         microphone_available_ = !snapshot.inputs.isEmpty();
                         synchronizeRecordState();
                         updateMeterServices();
                     });
    QObject::connect(&webcam_notifier_, &WebcamDeviceNotifier::snapshotChanged, &record_view_model_adapter_,
                     [this](const WebcamDeviceSnapshot& snapshot, DiscoveryReason) {
                         webcam_available_ = !snapshot.devices.isEmpty();
                         if (webcam_available_ && live_config_.webcam.device_id.empty()) {
                             live_config_.webcam.device_id = snapshot.devices.front().id;
                             recording_coordinator_->SetWebcamSettings(live_config_.webcam);
                         }
                         synchronizeRecordState();
                     });
    audio_notifier_.start();
    webcam_notifier_.start();
    audio_notifier_.rescan();
    webcam_notifier_.rescan();

    startCapabilityProbe();
    // The coordinator is armed in LoadingCapabilities until the probe lands; the
    // Record surface renders that as its own loading state rather than a Ready
    // one that would let a start be attempted against unknown hardware.
    if (!visualScenarioLatched())
        record_view_model_.SetState(recording_coordinator_->State());
    record_view_model_.capability_status_text = recording_coordinator_->CapabilityStatusText();
    synchronizeRecordState();
}

// The hardware capability query enumerates DXGI adapters and probes encoders --
// hundreds of milliseconds of blocking work. Running it inline stalls the first
// frame, so it goes to a worker and the result is posted back to the GUI thread,
// matching the deliberate off-thread design the Widgets shell already had.
void QuickApplication::startCapabilityProbe() {
    diagnostics::AppLog::info(QStringLiteral("perf"),
                              QStringLiteral("caps-probe-start %1 ms").arg(diagnostics::StartupClock().elapsed()));
    // Guards against a probe that outlives this object: the adapter is a member,
    // so a QPointer to it tracks the whole QuickApplication's lifetime, and the
    // queued lambda drops silently if teardown won the race.
    QPointer<RecordViewModelAdapter> alive(&record_view_model_adapter_);
    capability_probe_pool_.start([this, alive]() {
        // Exception barrier: the probe allocates and offers no top-level noexcept
        // guarantee. An escaped throw would abort the QThread and std::terminate
        // the process, so failures are posted to the GUI thread instead — the
        // coordinator then resolves to a failure state rather than hanging armed.
        try {
            capability::CapabilitySet caps = capability::CapabilityBuilder::BuildFromHardwareQuery();
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [this, alive, caps]() {
                    if (alive.isNull())
                        return;
                    onCapabilitiesReady(caps);
                },
                Qt::QueuedConnection);
        } catch (const std::exception& error) {
            const QString reason = QString::fromUtf8(error.what());
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [this, alive, reason]() {
                    if (alive.isNull())
                        return;
                    onCapabilityProbeFailed(reason);
                },
                Qt::QueuedConnection);
        } catch (...) {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [this, alive]() {
                    if (alive.isNull())
                        return;
                    onCapabilityProbeFailed(QStringLiteral("Unknown error"));
                },
                Qt::QueuedConnection);
        }
    });
}

void QuickApplication::onCapabilitiesReady(const capability::CapabilitySet& capabilities) {
    diagnostics::AppLog::info(QStringLiteral("perf"),
                              QStringLiteral("caps-probe-end %1 ms").arg(diagnostics::StartupClock().elapsed()));
    capabilities_ = capabilities;
    recording_coordinator_->OnCapabilitiesReady(capabilities_);
    // Everything downstream reads the capability set: the Device matrix, the
    // Settings codec lists and the Diagnostics recommendations were all built
    // against the empty set while the probe ran, so they are rebuilt here.
    device_adapter_.setCapabilitySet(capabilities_);
    settings_adapter_.setCapabilities(capabilities_);
    // Product-spec §6: the HDR-handling row exists only once a display actively
    // reports an HDR colour space. Nothing set this before, so the row's gate was
    // stuck at its `false` default and an HDR user could never reach the setting.
    const auto& displays = capabilities_.runtime.displays;
    settings_adapter_.setHdrDisplayPresent(
        std::any_of(displays.begin(), displays.end(),
                    [](const capability::DisplayHdrFacts& display) { return display.hdr_active; }));
    refreshDiagnosticsData();
    if (!visualScenarioLatched())
        record_view_model_.SetState(recording_coordinator_->State());
    record_view_model_.capability_status_text = recording_coordinator_->CapabilityStatusText();
    synchronizeRecordState();
    reapplyVisualScenarios();
}

void QuickApplication::onCapabilityProbeFailed(const QString& reason) {
    diagnostics::AppLog::warning(QStringLiteral("caps"), QStringLiteral("capability probe failed: %1").arg(reason));
    recording_coordinator_->OnCapabilityFailure(reason.toStdWString());
    if (!visualScenarioLatched())
        record_view_model_.SetState(recording_coordinator_->State());
    record_view_model_.capability_status_text = recording_coordinator_->CapabilityStatusText();
    synchronizeRecordState();
    reapplyVisualScenarios();
}

// The capability probe runs off-thread and lands hundreds of milliseconds after
// startup, and both of its completion paths push the coordinator's own state
// back into the view model. A --record-visual-state or --overlay-visual-state
// applied at singleShot(0) is therefore overwritten before the capture fires:
// the seeded STATS survived (the probe does not touch them) while the seeded
// STATE silently reverted to Ready, which is how a "recording" scenario could
// photograph a 12:34 timer above an idle Record button.
//
// Re-applied here rather than delayed to just before the capture, because the
// probe has no fixed duration and any chosen delay would be a race.
bool QuickApplication::visualScenarioLatched() const noexcept {
    return !pending_record_visual_state_.isEmpty() || !pending_overlay_visual_state_.isEmpty();
}

void QuickApplication::reapplyVisualScenarios() {
    if (reapplying_visual_scenarios_)
        return;
    reapplying_visual_scenarios_ = true;
    if (!pending_record_visual_state_.isEmpty())
        (void)applyRecordVisualScenario(pending_record_visual_state_);
    if (!pending_overlay_visual_state_.isEmpty())
        (void)applyOverlayVisualScenario(pending_overlay_visual_state_);
    reapplying_visual_scenarios_ = false;
}

void QuickApplication::wireRecordCommands() {
    QObject::connect(&record_view_model_adapter_, &RecordViewModelAdapter::startRequested, &record_view_model_adapter_,
                     [this]() { startRequested(); });
    QObject::connect(&record_view_model_adapter_, &RecordViewModelAdapter::stopRequested, &record_view_model_adapter_,
                     [this]() { recording_coordinator_->StopRecording(); });
    QObject::connect(&record_view_model_adapter_, &RecordViewModelAdapter::pauseRequested, &record_view_model_adapter_,
                     [this]() { recording_coordinator_->PauseRecording(); });
    QObject::connect(&record_view_model_adapter_, &RecordViewModelAdapter::resumeRequested, &record_view_model_adapter_,
                     [this]() { recording_coordinator_->ResumeRecording(); });
    QObject::connect(&record_view_model_adapter_, &RecordViewModelAdapter::captureFrameRequested,
                     &record_view_model_adapter_, [this]() { recording_coordinator_->CaptureFrame(); });
    QObject::connect(&record_view_model_adapter_, &RecordViewModelAdapter::addMarkerRequested,
                     &record_view_model_adapter_, [this]() { recording_coordinator_->AddMarker(); });
    QObject::connect(&record_view_model_adapter_, &RecordViewModelAdapter::openEditorRequested,
                     &record_view_model_adapter_, [this]() { openEditorForCurrentRecording(); });
    QObject::connect(&record_view_model_adapter_, &RecordViewModelAdapter::splitRequested, &record_view_model_adapter_,
                     [this]() {
                         recording_coordinator_->RequestSplit(recorder_core::SplitTriggerSource::ManualButton);
                         synchronizeRecordState();
                     });
    QObject::connect(&record_view_model_adapter_, &RecordViewModelAdapter::selectTargetRequested,
                     &record_view_model_adapter_, [this](int index, int mode) {
                         if (mode < static_cast<int>(CaptureMode::Monitor) ||
                             mode > static_cast<int>(CaptureMode::Region))
                             return;
                         selectTarget(index, static_cast<CaptureMode>(mode));
                     });
    QObject::connect(&record_view_model_adapter_, &RecordViewModelAdapter::selectRegionRequested,
                     &record_view_model_adapter_, [this](const QRectF& rect) { selectRegion(rect); });
    QObject::connect(&record_view_model_adapter_, &RecordViewModelAdapter::toggleSourceRequested,
                     &record_view_model_adapter_, [this](const QString& key) { toggleSource(key); });
    QObject::connect(&record_view_model_adapter_, &RecordViewModelAdapter::countdownSecondsRequested,
                     &record_view_model_adapter_, [this](int seconds) {
                         static constexpr int choices[] = {0, 3, 5, 10};
                         const auto nearest =
                             std::min_element(std::begin(choices), std::end(choices), [seconds](int lhs, int rhs) {
                                 return std::abs(lhs - seconds) < std::abs(rhs - seconds);
                             });
                         live_config_.countdown_seconds = *nearest;
                         persistLiveConfig();
                         synchronizeRecordState();
                     });
    QObject::connect(&record_view_model_adapter_, &RecordViewModelAdapter::activeChanged, &record_view_model_adapter_,
                     [this]() { updateMeterServices(); });
    QObject::connect(&record_view_model_adapter_, &RecordViewModelAdapter::webcamOverlayRectRequested,
                     &record_view_model_adapter_, [this](const QRectF& rect) { updateWebcamOverlay(rect); });
}

void QuickApplication::synchronizeRecordState() {
    const bool mkv = live_config_.output.container == capability::Container::Matroska ||
                     live_config_.output.container == capability::Container::WebM;
    record_view_model_adapter_.setFormatText(formatLabel(live_config_));
    record_view_model_adapter_.setDeviceState(microphone_available_, webcam_available_, live_config_.webcam.enabled,
                                              webcam_error_);
    record_view_model_adapter_.setWebcamPresentation(
        QRectF(live_config_.webcam.overlay.x_norm, live_config_.webcam.overlay.y_norm,
               live_config_.webcam.overlay.w_norm, live_config_.webcam.overlay.h_norm),
        live_config_.webcam.mirror, live_config_.webcam.opacity);
    const WebcamChromaKeySettings::ActiveRgb key_color = live_config_.webcam.chroma_key.active_color();
    webcam_frame_provider_->setChromaKey(live_config_.webcam.chroma_key.enabled, key_color.r, key_color.g, key_color.b,
                                         live_config_.webcam.chroma_key.tolerance,
                                         live_config_.webcam.chroma_key.softness,
                                         live_config_.webcam.chroma_key.spill_reduction);
    record_view_model_adapter_.setCountdownState(live_config_.countdown_seconds, countdown_remaining_,
                                                 countdown_progress_);
    record_view_model_adapter_.setPreviewFrameReady(record_preview_adapter_.frameReady());
    record_view_model_adapter_.setSplitEnabled(mkv && !recording_coordinator_->IsSplitPending());
    // Recording configuration must not be editable while a capture is in flight:
    // container and codec are fixed for the session once the encoder is up.
    // Sampling this once during initialization left every Settings row unlocked
    // for the whole run, because nothing re-evaluated it on a state change.
    settings_adapter_.setControlsLocked(recording_coordinator_->State() != UiRecordingState::Ready);
    // The single edge that re-points the exclusive-fullscreen probe: every
    // selection change and every recording-state change already lands here, and
    // the unchanged case costs one comparison.
    updateCaptureEvidenceTarget();
    record_view_model_adapter_.synchronize();
    // Same cadence and the same reason as the tray: the on-screen overlays are
    // presence surfaces and must never lag the state the window is showing.
    overlay_adapter_.synchronize();
    // After synchronize(), so the tray reads the state the window is about to
    // show rather than the one it is replacing.
    refreshTrayState();
}

void QuickApplication::selectTarget(int target_index, CaptureMode mode) {
    if (!record_view_model_adapter_.canSelectSource() && record_view_model_.selected_target_index >= 0)
        return;
    if (target_index < 0 || target_index >= static_cast<int>(record_view_model_.targets.size())) {
        record_view_model_.selected_target_index = -1;
        record_preview_adapter_.clearPreviewTarget();
        synchronizeRecordState();
        return;
    }
    const auto& target = record_view_model_.targets[static_cast<std::size_t>(target_index)];
    if ((mode == CaptureMode::Window) != (target.kind == recorder_core::CaptureTarget::Kind::Window))
        return;

    record_view_model_.selected_target_index = target_index;
    record_view_model_.capture_mode = mode;
    const capability::CaptureTargetKind target_kind =
        mode == CaptureMode::Window ? capability::CaptureTargetKind::Window : capability::CaptureTargetKind::Display;
    record_view_model_.ApplyTargetKindPreservingAudio(target_kind);
    if (mode == CaptureMode::Window) {
        DWORD process_id = 0;
        GetWindowThreadProcessId(reinterpret_cast<HWND>(target.native_id), &process_id);
        record_view_model_.audio_ui_state.selected_window_pid = process_id;
    } else {
        record_view_model_.audio_ui_state.selected_window_pid.reset();
    }
    live_config_.audio = record_view_model_.audio_ui_state;
    live_config_.capture.kind = mode == CaptureMode::Window   ? PresetCaptureKind::Window
                                : mode == CaptureMode::Region ? PresetCaptureKind::Region
                                                              : PresetCaptureKind::Display;
    live_config_.capture.window_key =
        mode == CaptureMode::Window ? RecordViewModel::TargetLabelFromCaptureTarget(target) : std::string{};
    live_config_.capture.display_id = {};
    if (mode != CaptureMode::Window) {
        const std::vector<EnumeratedDisplayIdentity> displays = EnumerateDisplayIdentities();
        const auto identity = std::find_if(displays.begin(), displays.end(), [&target](const auto& display) {
            return display.hmonitor != 0 && display.hmonitor == target.native_id;
        });
        if (identity != displays.end())
            live_config_.capture.display_id = identity->id;
        else
            live_config_.capture.display_id.gdi_name = target.description;
    }
    if (mode != CaptureMode::Region) {
        record_view_model_.has_region = false;
        live_config_.capture.has_region = false;
        live_config_.capture.region_display_id = {};
        record_view_model_adapter_.setRegionState(QRectF(0.0, 0.0, 1.0, 1.0), false);
    } else {
        record_view_model_.has_region = false;
        live_config_.capture.has_region = false;
        live_config_.capture.region_display_id = live_config_.capture.display_id;
        record_view_model_adapter_.setRegionState(QRectF(0.0, 0.0, 1.0, 1.0), true);
    }
    record_preview_adapter_.setPreviewTarget(target);
    updateOutputTargetContext(target);
    persistLiveConfig();
    // The target kind decides whether the APP audio row is offered at all, so
    // the Settings mirror has to see the new capture before its next edit.
    syncConfigMirrors();
    updateMeterServices();
    synchronizeRecordState();
}

void QuickApplication::refreshCaptureTargets(const CaptureTargetSnapshot& snapshot, DiscoveryReason reason) {
    if (locksCaptureTargets(record_view_model_.state)) {
        capture_target_refresh_pending_ = true;
        return;
    }

    std::optional<recorder_core::CaptureTarget> previous;
    if (record_view_model_.selected_target_index >= 0 &&
        record_view_model_.selected_target_index < static_cast<int>(record_view_model_.targets.size())) {
        previous = record_view_model_.targets[static_cast<size_t>(record_view_model_.selected_target_index)];
    }
    record_view_model_.targets = snapshot.targets;
    ++record_view_model_.targets_revision;
    record_view_model_.target_display_names.clear();
    record_view_model_.target_display_names.reserve(record_view_model_.targets.size());
    for (const auto& target : record_view_model_.targets) {
        record_view_model_.target_display_names.push_back(
            QString::fromStdString(RecordViewModel::TargetLabelFromCaptureTarget(target)).toStdWString());
    }

    int resolved_index = -1;
    if (previous.has_value()) {
        for (int index = 0; index < static_cast<int>(record_view_model_.targets.size()); ++index) {
            const auto& candidate = record_view_model_.targets[static_cast<size_t>(index)];
            if (candidate.kind == previous->kind && candidate.native_id == previous->native_id) {
                resolved_index = index;
                break;
            }
        }
    }
    if (resolved_index < 0 && record_view_model_.capture_mode == CaptureMode::Window &&
        !live_config_.capture.window_key.empty()) {
        for (int index = 0; index < static_cast<int>(record_view_model_.targets.size()); ++index) {
            const auto& candidate = record_view_model_.targets[static_cast<size_t>(index)];
            if (candidate.kind == recorder_core::CaptureTarget::Kind::Window &&
                RecordViewModel::TargetLabelFromCaptureTarget(candidate) == live_config_.capture.window_key) {
                resolved_index = index;
                break;
            }
        }
    }
    if (resolved_index < 0 && record_view_model_.capture_mode != CaptureMode::Window &&
        !live_config_.capture.display_id.empty()) {
        const auto displays = EnumerateDisplayIdentities();
        if (const auto match = ResolveStableDisplay(live_config_.capture.display_id, displays); match.has_value()) {
            const uintptr_t monitor = displays[match->index].hmonitor;
            for (int index = 0; index < static_cast<int>(record_view_model_.targets.size()); ++index) {
                const auto& candidate = record_view_model_.targets[static_cast<size_t>(index)];
                if (candidate.kind == recorder_core::CaptureTarget::Kind::Monitor && candidate.native_id == monitor) {
                    resolved_index = index;
                    break;
                }
            }
        }
    }

    record_view_model_.selected_target_index = resolved_index;
    if (resolved_index < 0) {
        record_preview_adapter_.clearPreviewTarget();
        if (record_view_model_.capture_mode == CaptureMode::Region) {
            record_view_model_.has_region = false;
            record_view_model_adapter_.setRegionState(QRectF(0.0, 0.0, 1.0, 1.0), false);
        }
        record_view_model_adapter_.setNoticeText(captureSourceUnavailableNotice());
    } else {
        const auto& target = record_view_model_.targets[static_cast<size_t>(resolved_index)];
        record_preview_adapter_.setPreviewTarget(target);
        updateOutputTargetContext(target);
        if (target.kind == recorder_core::CaptureTarget::Kind::Window) {
            DWORD process_id = 0;
            GetWindowThreadProcessId(reinterpret_cast<HWND>(target.native_id), &process_id);
            record_view_model_.audio_ui_state.selected_window_pid = process_id;
        } else {
            record_view_model_.audio_ui_state.selected_window_pid.reset();
        }
        if (record_view_model_.capture_mode == CaptureMode::Region && live_config_.capture.has_region) {
            const QRectF restored(live_config_.capture.region_x_norm, live_config_.capture.region_y_norm,
                                  live_config_.capture.region_w_norm, live_config_.capture.region_h_norm);
            selectRegion(restored);
        }
        if (record_view_model_adapter_.noticeText() == captureSourceUnavailableNotice())
            record_view_model_adapter_.setNoticeText({});
    }
    diagnostics::AppLog::info(QStringLiteral("CaptureTargetDiscovery"),
                              QStringLiteral("Quick target model refreshed — targets:%1 selected:%2 reason:%3")
                                  .arg(record_view_model_.targets.size())
                                  .arg(resolved_index)
                                  .arg(QLatin1StringView(DiscoveryReasonName(reason))));
    updateMeterServices();
    synchronizeRecordState();
}

void QuickApplication::updateOutputTargetContext(const recorder_core::CaptureTarget& target) {
    FilenameTargetContext context = RecordViewModel::FilenameContextFromCaptureTarget(target);
    context.video_codec = live_config_.output.video_codec;
    context.audio_codec = live_config_.output.audio_codec;
    recording_coordinator_->SetOutputTargetContext(context);
}

void QuickApplication::selectRegion(const QRectF& normalized_rect) {
    if (record_view_model_.capture_mode != CaptureMode::Region || record_view_model_.selected_target_index < 0)
        return;
    const auto& target = record_view_model_.targets[static_cast<std::size_t>(record_view_model_.selected_target_index)];
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (target.kind != recorder_core::CaptureTarget::Kind::Monitor ||
        GetMonitorInfoW(reinterpret_cast<HMONITOR>(target.native_id), &monitor) == FALSE) {
        record_view_model_adapter_.setNoticeText(QStringLiteral("The selected display is no longer available."));
        return;
    }
    const QRectF bounded = normalized_rect.normalized().intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    const int monitor_width = monitor.rcMonitor.right - monitor.rcMonitor.left;
    const int monitor_height = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
    recorder_core::CaptureRegion region;
    region.x = monitor.rcMonitor.left + static_cast<int32_t>(std::lround(bounded.x() * monitor_width));
    region.y = monitor.rcMonitor.top + static_cast<int32_t>(std::lround(bounded.y() * monitor_height));
    region.width = static_cast<int32_t>(std::lround(bounded.width() * monitor_width));
    region.height = static_cast<int32_t>(std::lround(bounded.height() * monitor_height));
    if (!region.IsValid()) {
        record_view_model_adapter_.setNoticeText(QStringLiteral("Select a region at least 64 × 64 pixels."));
        return;
    }
    record_view_model_.region = region;
    record_view_model_.has_region = true;
    live_config_.capture.has_region = true;
    live_config_.capture.region_display_id = live_config_.capture.display_id;
    live_config_.capture.region_x_norm = static_cast<float>(bounded.x());
    live_config_.capture.region_y_norm = static_cast<float>(bounded.y());
    live_config_.capture.region_w_norm = static_cast<float>(bounded.width());
    live_config_.capture.region_h_norm = static_cast<float>(bounded.height());
    record_view_model_adapter_.setRegionState(bounded, false);
    persistLiveConfig();
    synchronizeRecordState();
}

// The one recording-start policy. Every trigger runs through it -- the transport
// button, the global start hotkey, the notification actions and the Live Verify
// control channel -- and the outcome is latched so a caller that needs to know
// whether the request was honoured can read it instead of re-deriving it. The
// control channel does exactly that; it does NOT have preconditions of its own.
void QuickApplication::startRequested() {
    if (record_view_model_.state == UiRecordingState::Countdown) {
        cancelCountdown();
        last_start_admission_ = StartAdmission::CountdownCancelled;
        return;
    }
    // QCR-415. The only way to get here while a blocking surface is up is the
    // global start hotkey — the surfaces cover the transport, and the desktop is
    // deliberately still reachable. Starting anyway would put the user in a
    // session whose transport is behind a scrim, and it would silently invalidate
    // the very offer an open recovery surface is making: ArmFromRecovery is
    // refused once the coordinator has left Ready. Stop, pause and resume are NOT
    // gated here; a recording that cannot be stopped is the worse state.
    if (surface_arbiter_.anySurfaceUp()) {
        diagnostics::AppLog::info(QStringLiteral("record"),
                                  QStringLiteral("Start refused: a blocking surface is open — answer it first"));
        last_start_admission_ = StartAdmission::RefusedByBlockingSurface;
        return;
    }
    if (!record_view_model_adapter_.canStart()) {
        last_start_admission_ = StartAdmission::RefusedByState;
        return;
    }
    if (live_config_.countdown_seconds > 0) {
        countdown_clock_.restart();
        if (!countdown_.start(live_config_.countdown_seconds, 0)) {
            last_start_admission_ = StartAdmission::RefusedByState;
            return;
        }
        countdown_remaining_ = live_config_.countdown_seconds;
        countdown_progress_ = 1.0;
        record_view_model_.SetState(UiRecordingState::Countdown);
        countdown_timer_.start();
        synchronizeRecordState();
        last_start_admission_ = StartAdmission::Accepted;
        return;
    }
    last_start_admission_ = startRecordingNow() ? StartAdmission::Accepted : StartAdmission::RefusedNoTarget;
}

bool QuickApplication::startRecordingNow() {
    if (record_view_model_.selected_target_index < 0 ||
        record_view_model_.selected_target_index >= static_cast<int>(record_view_model_.targets.size()))
        return false;
    const auto& target = record_view_model_.targets[static_cast<std::size_t>(record_view_model_.selected_target_index)];
    std::optional<recorder_core::CaptureRegion> crop;
    if (record_view_model_.capture_mode == CaptureMode::Region) {
        if (!record_view_model_.has_region || !record_view_model_.region.IsValid())
            return false;
        crop = record_view_model_.region;
    }
    FilenameTargetContext context = RecordViewModel::FilenameContextFromCaptureTarget(target);
    context.video_codec = live_config_.output.video_codec;
    context.audio_codec = live_config_.output.audio_codec;
    recording_coordinator_->SetOutputTargetContext(context);
    recording_coordinator_->SetOutputSettings(live_config_.output);
    recording_coordinator_->SetVideoSettings(live_config_.video);
    recording_coordinator_->SetWebcamSettings(live_config_.webcam);
    record_view_model_.ResetStats();
    recording_coordinator_->StartRecording(target, record_view_model_.audio_ui_state, crop);
    return true;
}

void QuickApplication::cancelCountdown() {
    countdown_timer_.stop();
    countdown_.cancel();
    countdown_remaining_ = 0;
    countdown_progress_ = 0.0;
    record_view_model_.SetState(recording_coordinator_->State());
    synchronizeRecordState();
}

void QuickApplication::updateCountdown() {
    if (!countdown_.isRunning())
        return;
    const int64_t elapsed = countdown_clock_.isValid() ? countdown_clock_.elapsed() : 0;
    countdown_remaining_ = countdown_.remainingSeconds(elapsed);
    countdown_progress_ = countdown_.remainingFraction(elapsed);
    if (countdown_.hasReachedZero(elapsed)) {
        countdown_timer_.stop();
        countdown_.complete();
        countdown_remaining_ = 0;
        countdown_progress_ = 0.0;
        (void)startRecordingNow();
        return;
    }
    synchronizeRecordState();
}

void QuickApplication::toggleSource(const QString& key) {
    if (key == QStringLiteral("webcam")) {
        if (!webcam_available_)
            return;
        live_config_.webcam.enabled = !live_config_.webcam.enabled;
        webcam_error_.clear();
        recording_coordinator_->SetWebcamSettings(live_config_.webcam);
        persistLiveConfig();
        // Webcam enable is also a Settings row; both surfaces write the same flag.
        syncConfigMirrors();
        synchronizeRecordState();
        return;
    }
    if (!record_view_model_adapter_.canSelectSource())
        return;
    for (auto& row : record_view_model_.audio_ui_state.source_rows) {
        const bool match = (key == QStringLiteral("system") && isSystemRow(row)) ||
                           (key == QStringLiteral("app") && row.kind == recorder_core::AudioSourceKind::App) ||
                           (key == QStringLiteral("microphone") && row.kind == recorder_core::AudioSourceKind::Mic);
        if (match) {
            if (key == QStringLiteral("microphone") && !microphone_available_)
                return;
            row.enabled = !row.enabled;
            break;
        }
    }
    record_view_model_.RebuildAudioPlan();
    live_config_.audio = record_view_model_.audio_ui_state;
    persistLiveConfig();
    // The same three rows are switches on the Settings audio section.
    syncConfigMirrors();
    updateMeterServices();
    synchronizeRecordState();
}

void QuickApplication::updateMeters() {
    const bool session = record_view_model_.state == UiRecordingState::Recording ||
                         record_view_model_.state == UiRecordingState::Paused ||
                         record_view_model_.state == UiRecordingState::Stopping;
    const float system = recording_coordinator_->IsSysMeterRunning() ? preflight_system_rms_
                         : session                                   ? record_view_model_.audio_rms_sys
                                                                     : 0.0f;
    const float app = recording_coordinator_->IsAppMeterRunning() ? preflight_app_rms_
                      : session                                   ? record_view_model_.audio_rms_app
                                                                  : 0.0f;
    const float microphone = recording_coordinator_->IsMicMeterRunning()
                                 ? preflight_microphone_rms_ * record_view_model_.audio_ui_state.mic_gain_linear
                             : session ? record_view_model_.audio_rms_mic
                                       : 0.0f;
    record_view_model_adapter_.setMeters(dockLevel(system), dockLevel(app), dockLevel(microphone));
    settings_adapter_.setMeters(dockLevel(system), dockLevel(app), dockLevel(microphone));
}

void QuickApplication::updateWebcamOverlay(const QRectF& normalized_rect) {
    if (!record_view_model_adapter_.webcamOverlayEditable())
        return;
    WebcamOverlayRect overlay;
    overlay.x_norm = static_cast<float>(normalized_rect.x());
    overlay.y_norm = static_cast<float>(normalized_rect.y());
    overlay.w_norm = static_cast<float>(normalized_rect.width());
    overlay.h_norm = static_cast<float>(normalized_rect.height());
    live_config_.webcam.overlay = SanitizeWebcamOverlayRect(overlay);
    recording_coordinator_->SetWebcamSettings(live_config_.webcam);
    webcam_overlay_persist_timer_.start();
    synchronizeRecordState();
}

void QuickApplication::scheduleMeterUpdate() {
    if (!meter_update_timer_.isActive())
        meter_update_timer_.start();
}

// Stops happen now, starts happen after a short debounce.
//
// The asymmetry is deliberate. A stop is a statement about what must NOT be
// running — a session is beginning, the page was left — and delaying it would
// leave a preflight meter holding an endpoint into a state that says it does
// not. A start is decoration: three level bars a user is not yet looking at.
//
// The debounce is what makes a Record → Settings → Record detour cost one start
// instead of a start, a stop and a start; the profiler measured that round trip
// at ~43 ms of GUI-thread time per return. Together with
// MeterStartMode::Deferred (the endpoint open no longer blocks the caller) the
// page-activation binding does no WASAPI work at all.
void QuickApplication::updateMeterServices() {
    const bool visible = record_view_model_adapter_.active();
    const bool session = record_view_model_.state == UiRecordingState::Recording ||
                         record_view_model_.state == UiRecordingState::Paused ||
                         record_view_model_.state == UiRecordingState::Stopping;
    if (!visible || session) {
        meter_service_start_timer_.stop();
        recording_coordinator_->StopSysMeter();
        recording_coordinator_->StopAppMeter();
        recording_coordinator_->StopMicMeter();
        recording_coordinator_->SetWebcamPreviewActive(false);
        return;
    }
    meter_service_start_timer_.start();
}

void QuickApplication::startMeterServices() {
    const bool visible = record_view_model_adapter_.active();
    const bool session = record_view_model_.state == UiRecordingState::Recording ||
                         record_view_model_.state == UiRecordingState::Paused ||
                         record_view_model_.state == UiRecordingState::Stopping;
    // The state can have moved on inside the debounce window through a path that
    // does not call updateMeterServices() at all, so the condition is re-read
    // rather than assumed from whoever scheduled this.
    if (!visible || session)
        return;
    recording_coordinator_->StartSysMeter();
    if (microphone_available_)
        recording_coordinator_->StartMicMeter(record_view_model_.audio_ui_state.selected_mic_device_id,
                                              record_view_model_.audio_ui_state.mic_channel_mode);
    else
        recording_coordinator_->StopMicMeter();
    if (record_view_model_.audio_ui_state.target_kind == capability::CaptureTargetKind::Window &&
        record_view_model_.audio_ui_state.selected_window_pid.has_value()) {
        recording_coordinator_->StartAppMeter(*record_view_model_.audio_ui_state.selected_window_pid);
    } else {
        recording_coordinator_->StopAppMeter();
    }
    const bool idle =
        record_view_model_.state == UiRecordingState::Ready || record_view_model_.state == UiRecordingState::Completed;
    recording_coordinator_->SetWebcamPreviewActive(live_config_.webcam.enabled && idle);
}

// ---------------------------------------------------------------------------
// Settings area
// ---------------------------------------------------------------------------

void QuickApplication::initializeSettingsArea() {
    settings_adapter_.setAppSettings(settings_);
    // Seeded here as well as in saveAndPublishAppSettings(): the adapter is
    // constructed before settings_ has been handed to anyone, so without this a
    // first run would show the built-in defaults until the first settings edit.
    overlay_adapter_.setAppSettings(settings_);
    settings_adapter_.setCapabilities(capabilities_);
    settings_adapter_.setConfig(live_config_);
    settings_adapter_.setControlsLocked(recording_coordinator_->State() != UiRecordingState::Ready);

    int max_fps = 0;
    for (const QScreen* screen : QGuiApplication::screens()) {
        max_fps = std::max(max_fps, static_cast<int>(std::lround(screen->refreshRate())));
    }
    settings_adapter_.setMaxFrameRate(max_fps > 0 ? max_fps : kFallbackMaxFrameRate);

    QObject::connect(&audio_notifier_, &AudioDeviceNotifier::snapshotChanged, &settings_adapter_,
                     [this](const AudioDeviceSnapshot& snapshot, DiscoveryReason) {
                         QVariantList devices;
                         for (const recorder_core::AudioInputDeviceInfo& device : snapshot.inputs) {
                             QVariantMap entry;
                             entry.insert(QStringLiteral("value"), QString::fromStdString(device.device_id));
                             entry.insert(QStringLiteral("label"), QString::fromStdString(device.display_name));
                             entry.insert(QStringLiteral("selectable"), true);
                             entry.insert(QStringLiteral("reason"), QString());
                             devices.append(entry);
                         }
                         settings_adapter_.setMicrophoneDevices(std::move(devices));
                     });

    QObject::connect(&webcam_notifier_, &WebcamDeviceNotifier::snapshotChanged, &settings_adapter_,
                     [this](const WebcamDeviceSnapshot& snapshot, DiscoveryReason) {
                         QVariantList devices;
                         for (const auto& device : snapshot.devices) {
                             QVariantMap entry;
                             entry.insert(QStringLiteral("value"), QString::fromStdString(device.id));
                             entry.insert(QStringLiteral("label"), QString::fromStdString(device.name));
                             entry.insert(QStringLiteral("selectable"), true);
                             entry.insert(QStringLiteral("reason"), QString());
                             devices.append(entry);
                         }
                         settings_adapter_.setWebcamDevices(std::move(devices));
                     });

    refreshPresetState();
    wireSettingsCommands();
    initializeHotkeys();
}

// ---------------------------------------------------------------------------
// Diagnostics + Logs area
// ---------------------------------------------------------------------------

void QuickApplication::initializeDiagnosticsArea() {
    diagnostics_adapter_.setCapabilitySet(capabilities_);
    diagnostics_adapter_.setExpertMode(settings_.expert_mode_enabled);
    {
        // The elevation fact is a one-shot process property, not a per-refresh probe.
        diagnostics::Win32ElevationProvider elevation;
        diagnostics_adapter_.setElevated(elevation.IsElevated());
    }

    // Single global Expert state, shared with Settings (AppSettingsStore).
    QObject::connect(&diagnostics_adapter_, &DiagnosticsAdapter::expertModeChanged, &diagnostics_adapter_,
                     [this](bool enabled) {
                         // A harness override is in-memory only: rendering the Expert
                         // taxonomy for review must never rewrite the developer's own
                         // persisted Expert setting.
                         if (visual_expert_override_ || settings_.expert_mode_enabled == enabled)
                             return;
                         settings_.expert_mode_enabled = enabled;
                         settings_adapter_.setAppSettings(settings_);
                         persistAppSettings(SettingsWriteIntent::UserEdit);
                     });

    QObject::connect(&diagnostics_adapter_, &DiagnosticsAdapter::applyFixAccepted, &diagnostics_adapter_,
                     [this](const QString& fix_id) { applyDiagnosticsFix(fix_id); });
    // Assisted fixes resolve to a Settings section; the Quick Settings page has no
    // section anchor yet, so the navigation lands on Settings and the resolved
    // section is recorded rather than silently dropped.
    QObject::connect(
        &diagnostics_adapter_, &DiagnosticsAdapter::assistedFixRequested, &diagnostics_adapter_,
        [this](const QString& fix_id) {
            const diagnostics::FixResult result = diagnostics::ResolveAssistedFix(fix_id.toStdString());
            const std::string_view section = diagnostics::SettingsSectionFor(result.outcome);
            diagnostics_adapter_.requestSettingsNavigation();
            diagnostics::AppLog::info(
                QStringLiteral("diagnostics"),
                QStringLiteral("Opened assisted fix %1 -> %2")
                    .arg(fix_id, QString::fromUtf8(section.data(), static_cast<qsizetype>(section.size()))));
        });

    // Both support-bundle entry points share the one code path the Diagnostics
    // adapter owns, exactly as MainWindow's single createSupportBundle did.
    QObject::connect(&logs_adapter_, &LogsAdapter::createSupportBundleRequested, &diagnostics_adapter_,
                     [this](const QUrl& destination) { diagnostics_adapter_.createSupportBundle(destination); });

    // The live pipeline telemetry the Diagnostics surface renders is the same
    // snapshot stream the recording coordinator already produces.
    // No SetDiagnosticsCallback here: the coordinator's slot is single-occupancy
    // and initializeRecordWorkflow already owns it, fanning the snapshot out to
    // this adapter as well. Registering a second one only looked correct because
    // whichever initializer ran last won.

    refreshDiagnosticsData();
}

void QuickApplication::refreshDiagnosticsData() {
    // A seeded visual scenario owns the Diagnostics state for the life of the
    // process. Without this the async capability probe's completion replaced it
    // with the real environment a second or two after load, so the scenario was
    // set, then quietly undone, and the capture showed a healthy machine. Only a
    // --visual-test run can ever set this flag.
    if (diagnostics_visual_scenario_active_)
        return;

    diagnostics::DiagnosticsController::Config config;
    config.caps = capabilities_;
    config.audio = record_view_model_.audio_ui_state;
    config.user_config = diagnostics::UserConfigFromSettings(live_config_.output, live_config_.video);
    capability::SettingsResolver resolver(capabilities_);
    config.profile_validation = resolver.ValidateConfig(config.user_config);
    config.cap_summary = diagnostics::CapabilitySummary::FromCapabilitySet(capabilities_);
    config.config_summary = diagnostics::ConfigSummary::FromCurrentSettings(
        live_config_.output, live_config_.video, config.audio,
        std::filesystem::path(settings_store_.SettingsFilePath().toStdWString()),
        preset_registry_.SelectedPreset().name, std::string());
    config.output_folder = live_config_.output.output_folder.string();
    config.hotkeys_ok = true;
    config.hotkeys_summary = "None configured";
    diagnostics_adapter_.setDiagnosticConfig(std::move(config));
    diagnostics_adapter_.setCapabilitySet(capabilities_);
}

std::optional<recorder_core::CaptureTarget> QuickApplication::selectedCaptureTarget() const {
    const int selected = record_view_model_.selected_target_index;
    if (selected < 0 || selected >= static_cast<int>(record_view_model_.targets.size()))
        return std::nullopt;
    return record_view_model_.targets[static_cast<std::size_t>(selected)];
}

// QCR-110. The production producer of exclusive-fullscreen evidence, and the one
// place the selected target's HDR fact reaches Diagnostics.
//
// Before this, WindowEvidenceProbe existed only in the removed Widgets shell:
// the coordinator's evidence provider was never installed, so
// ResolveWindowExclusiveEvidence() always returned None and the
// rec.capture.exclusive_window blocker could not fire in the shipping binary —
// while the spec promised it would. Nothing called setCaptureWindowEvidence() or
// setCaptureTargetHdrActive() either, so neither card could ever appear.
void QuickApplication::updateCaptureEvidenceTarget() {
    // A --record-visual-state capture photographs a seeded target that has no
    // live HWND behind it. Subscribing WGC to it would spend a thread and a D3D11
    // device per harness process and measure nothing.
    if (visualScenarioLatched())
        return;

    const std::optional<recorder_core::CaptureTarget> target = selectedCaptureTarget();
    uintptr_t hwnd = 0;
    if (target.has_value() && target->kind == recorder_core::CaptureTarget::Kind::Window)
        hwnd = target->native_id;
    // The engine owns the capture from the countdown to the last saved byte; the
    // probe keeps its subscription but stops pumping so the source is not copied
    // twice. Same predicate the capture-target model uses for the same reason.
    const bool paused = locksCaptureTargets(record_view_model_.state);

    // Diagnostics reads the target itself for the capture-source readiness tile
    // and the HDR blocker card. Both are resolved here rather than in the probe:
    // they are facts about the SELECTION, not measurements of the window.
    const auto same_target = [](const std::optional<recorder_core::CaptureTarget>& lhs,
                                const std::optional<recorder_core::CaptureTarget>& rhs) {
        if (lhs.has_value() != rhs.has_value())
            return false;
        if (!lhs.has_value())
            return true;
        return lhs->kind == rhs->kind && lhs->native_id == rhs->native_id && lhs->description == rhs->description;
    };
    if (!same_target(pushed_selected_target_, target)) {
        pushed_selected_target_ = target;
        diagnostics_adapter_.setSelectedCaptureTarget(target);
    }
    // Same resolver the coordinator's own native-HDR10 decision uses
    // (FindTargetDisplayFacts over the probed display facts), so the blocker and
    // the card that explains it can never disagree about whether HDR is on.
    bool hdr_active = false;
    if (target.has_value()) {
        const capability::DisplayHdrFacts* facts = FindTargetDisplayFacts(*target, capabilities_.runtime.displays);
        hdr_active = facts != nullptr && facts->hdr_active;
    }
    if (pushed_capture_target_hdr_active_ != hdr_active) {
        pushed_capture_target_hdr_active_ = hdr_active;
        diagnostics_adapter_.setCaptureTargetHdrActive(hdr_active);
    }

    if (hwnd == evidence_target_hwnd_ && paused == evidence_paused_)
        return;

    if (hwnd != 0 && !window_evidence_probe_)
        window_evidence_probe_ = std::make_unique<WindowEvidenceProbe>();

    evidence_target_hwnd_ = hwnd;
    evidence_paused_ = paused;
    if (window_evidence_probe_) {
        window_evidence_probe_->SetWindowTarget(hwnd);
        window_evidence_probe_->SetPaused(paused);
    }

    if (hwnd == 0) {
        capture_evidence_timer_.stop();
        // Retargeting away from a window drops the old window's evidence at once
        // rather than leaving the card up until the next tick.
        if (pushed_window_evidence_.has_value()) {
            pushed_window_evidence_.reset();
            diagnostics_adapter_.setCaptureWindowEvidence(std::nullopt, {});
        }
        return;
    }
    // A new window starts from nothing measured. Publishing that immediately is
    // what keeps a card raised for the previous target from describing this one.
    if (pushed_window_evidence_.has_value()) {
        pushed_window_evidence_.reset();
        diagnostics_adapter_.setCaptureWindowEvidence(std::nullopt, {});
    }
    capture_evidence_timer_.start();
}

void QuickApplication::refreshCaptureWindowEvidence() {
    if (!window_evidence_probe_)
        return;
    const WindowEvidenceProbe::Snapshot snapshot = window_evidence_probe_->CurrentSnapshot();
    if (!snapshot.active || snapshot.hwnd != evidence_target_hwnd_ || snapshot.hwnd == 0)
        return; // the worker has not caught up with the current target yet

    // Push only on a verdict change. The engine derives nothing else from these
    // two structs, and every push re-runs the whole recommendation checklist.
    const diagnostics::ExclusiveEvidence next =
        diagnostics::ResolveSnapshotEvidence(snapshot, evidence_target_hwnd_, false);
    if (pushed_window_evidence_.has_value()) {
        const diagnostics::ExclusiveEvidence current =
            diagnostics::ResolveSnapshotEvidence(*pushed_window_evidence_, evidence_target_hwnd_, false);
        if (pushed_window_evidence_->hwnd == snapshot.hwnd && current == next)
            return;
    }
    pushed_window_evidence_ = snapshot;
    diagnostics_adapter_.setCaptureWindowEvidence(snapshot.facts, snapshot.evidence);
}

diagnostics::ExclusiveEvidence
QuickApplication::resolveWindowExclusiveEvidence(const recorder_core::CaptureTarget& target) const {
    if (!window_evidence_probe_ || target.kind != recorder_core::CaptureTarget::Kind::Window)
        return diagnostics::ExclusiveEvidence::None;
    return diagnostics::ResolveSnapshotEvidence(window_evidence_probe_->CurrentSnapshot(), target.native_id, false);
}

// QCR-804. The mid-recording capture-stall path, driven entirely by the
// diagnostics snapshots the pipeline already publishes at ~5 Hz.
//
// Before this, a window that stopped producing frames mid-recording was
// completely silent: WGC delivered nothing, the CFR pacer duplicated the last
// frame, the transport stayed green, the session finalized successfully and the
// user found a video frozen from the stall onwards. The pure predicate that was
// supposed to catch it (WindowCaptureStall) had no consumer at all — and could
// not have worked anyway, because it gated on capture.actual_fps, which the
// aggregator derives from EMITTED frames and which therefore sits at the target
// rate for exactly the whole duration of this failure.
//
// Threading: everything here runs on the Qt main thread. The snapshot arrives
// through RecordingCoordinator::PostDiagnostics's queued connection (already
// session-generation filtered), the monitor is a plain main-thread member, and
// the only thing handed back across a thread boundary is an atomic increment on
// the coordinator. No capture-thread state is dereferenced.
void QuickApplication::observeWindowCaptureStall(const recorder_core::RecordingDiagnosticsSnapshot& snapshot) {
    diagnostics::WindowStallSample sample;
    sample.session_generation = snapshot.session_generation;
    sample.is_window_target = snapshot.capture.source_type == recorder_core::CaptureSourceType::Window;
    sample.capture_expected = diagnostics::CaptureProgressExpected(snapshot.lifecycle);
    sample.frames_captured = snapshot.capture.frames_captured;
    sample.elapsed_seconds = snapshot.elapsed_seconds;

    switch (capture_stall_monitor_.Observe(sample)) {
    case diagnostics::WindowStallSignal::None:
        return;

    case diagnostics::WindowStallSignal::Recovered:
        clearWindowCaptureStallWarning();
        diagnostics::AppLog::info(QStringLiteral("capture"),
                                  QStringLiteral("window capture resumed after a reported stall"));
        return;

    case diagnostics::WindowStallSignal::Starved:
        break;
    }

    // Confirmed starvation. This is the ONLY point at which any Win32 fact is
    // read — once per episode, never on a healthy recording.
    const double starved_for = capture_stall_monitor_.seconds_without_progress();
    const diagnostics::WindowTargetFacts facts =
        diagnostics::GatherWindowTargetFacts(reinterpret_cast<HWND>(evidence_target_hwnd_));
    // PresentMon's verdict when it happens to be available (elevation- and
    // opt-in-gated). It only ever refines the cause; it never decides whether the
    // stall is reported.
    const bool present_fse =
        snapshot.capture.present_mode_availability == recorder_core::MetricAvailability::Available &&
        snapshot.capture.source_present_mode == recorder_core::PresentMode::ExclusiveFullscreen;

    const diagnostics::WindowStallVerdict verdict = diagnostics::ClassifyConfirmedStall(facts, present_fse);
    capture_stall_monitor_.ApplyVerdict(verdict);
    if (verdict != diagnostics::WindowStallVerdict::Stalled) {
        // Legitimate (minimized/cloaked/gone) or indistinguishable from idle
        // content. Logged so a support bundle shows the check ran and chose
        // silence — the user is told nothing.
        const QString reason = verdict == diagnostics::WindowStallVerdict::Legitimate
                                   ? QStringLiteral("window is minimized, hidden or gone")
                                   : QStringLiteral("ordinary window - indistinguishable from static content");
        diagnostics::AppLog::info(QStringLiteral("capture"),
                                  QStringLiteral("window capture produced no frame for %1 s; not reported (%2)")
                                      .arg(starved_for, 0, 'f', 1)
                                      .arg(reason));
        return;
    }

    const bool fullscreen_hint =
        diagnostics::EvaluateWindowStall(facts, present_fse) == diagnostics::WindowStallCause::ExclusiveFullscreen;
    if (recording_coordinator_)
        recording_coordinator_->NoteWindowCaptureStall();
    diagnostics::AppLog::warning(
        QStringLiteral("capture"),
        QStringLiteral("window capture stalled: no frame for %1 s, recording continues%2")
            .arg(starved_for, 0, 'f', 1)
            .arg(fullscreen_hint ? QStringLiteral(" (a fullscreen signal corroborates exclusive fullscreen)")
                                 : QString()));
    // Replaces any earlier stall toast rather than stacking a second one.
    clearWindowCaptureStallWarning();
    capture_stall_toast_sequence_ = notifications_adapter_.manager().Enqueue(
        notifications::MakeWindowCaptureStalledEvent(starved_for, fullscreen_hint));
}

void QuickApplication::clearWindowCaptureStallWarning() {
    if (capture_stall_toast_sequence_ == 0)
        return;
    notifications_adapter_.manager().Dismiss(capture_stall_toast_sequence_);
    capture_stall_toast_sequence_ = 0;
}

// ADR 0046. The mid-recording audio-degradation notice, driven entirely by the
// AudioDiagnostics health facts the pipeline already publishes at ~5 Hz.
//
// This is a restored producer, not a new feature. The Widgets frontend raised
// exactly this notification from exactly these facts
// (MainWindow::updateAudioSourceDegradedNotification, a second tee of
// RecordPage::diagnosticsUpdated). The Qt Quick cutover carried the event, the
// resolver, the standing-toast dwell rule, the hub key and the tests across, but
// not the tee — so from that commit on, an unplugged microphone degraded to
// honest silence, said so in Diagnostics and in the session report, and told the
// user nothing while it was happening. Same defect class as the lost
// exclusive-fullscreen producer.
//
// Threading: everything here runs on the Qt main thread. The snapshot arrives
// through RecordingCoordinator::PostDiagnostics's queued connection (already
// session-generation filtered), and the latch is a plain main-thread member.
void QuickApplication::observeAudioSourceDegradation(const recorder_core::RecordingDiagnosticsSnapshot& snapshot) {
    diagnostics::AudioDegradationSample sample;
    sample.session_generation = snapshot.session_generation;
    sample.valid = snapshot.valid;
    sample.lifecycle = snapshot.lifecycle;
    sample.source_degraded = snapshot.audio.source_degraded;
    sample.degraded_sources = snapshot.audio.degraded_sources;

    switch (audio_degradation_monitor_.Observe(sample)) {
    case diagnostics::AudioDegradationSignal::None:
        return;

    case diagnostics::AudioDegradationSignal::Clear:
        clearAudioSourceDegradedWarning();
        diagnostics::AppLog::info(QStringLiteral("audio"),
                                  QStringLiteral("every audio source is capturing again after a reported outage"));
        return;

    case diagnostics::AudioDegradationSignal::Raise:
        break;
    }

    const uint32_t degraded = audio_degradation_monitor_.degraded_sources();
    diagnostics::AppLog::warning(
        QStringLiteral("audio"),
        QStringLiteral("%1 audio capture source(s) lost their device and are contributing silence; recording continues")
            .arg(degraded));
    // Replaces any earlier notice for this outage rather than stacking a second
    // one — the manager has no update API, so the standing toast is replaced by
    // dismissing the tracked sequence and enqueueing the new body.
    clearAudioSourceDegradedWarning();
    audio_degraded_toast_sequence_ =
        notifications_adapter_.manager().Enqueue(notifications::MakeAudioSourceDegradedEvent(degraded));
}

void QuickApplication::clearAudioSourceDegradedWarning() {
    if (audio_degraded_toast_sequence_ == 0)
        return;
    notifications_adapter_.manager().Dismiss(audio_degraded_toast_sequence_);
    audio_degraded_toast_sequence_ = 0;
}

// rec.capture.exclusive_window's "record the monitor instead" fix: resolve the
// selected window's hosting monitor via MonitorFromWindow and select that monitor
// target exactly like a manual pick. A user-confirmed retarget drops the APP audio
// row, which the confirm's changes_summary already stated.
void QuickApplication::selectHostingMonitorForSelectedWindow() {
    const int selected = record_view_model_.selected_target_index;
    if (selected < 0 || selected >= static_cast<int>(record_view_model_.targets.size()))
        return;
    const auto& current = record_view_model_.targets[static_cast<std::size_t>(selected)];
    if (current.kind != recorder_core::CaptureTarget::Kind::Window || current.native_id == 0) {
        diagnostics::AppLog::info(QStringLiteral("target"),
                                  QStringLiteral("record-the-monitor-instead: no window target selected - no-op"));
        return;
    }

    const HMONITOR monitor = ::MonitorFromWindow(reinterpret_cast<HWND>(current.native_id), MONITOR_DEFAULTTONEAREST);
    const auto monitor_id = reinterpret_cast<uintptr_t>(monitor);
    for (std::size_t index = 0; index < record_view_model_.targets.size(); ++index) {
        const auto& candidate = record_view_model_.targets[index];
        if (candidate.kind == recorder_core::CaptureTarget::Kind::Monitor && candidate.native_id == monitor_id) {
            selectTarget(static_cast<int>(index), CaptureMode::Monitor);
            diagnostics::AppLog::info(
                QStringLiteral("target"),
                QStringLiteral("record-the-monitor-instead: retargeted window capture to its monitor"));
            return;
        }
    }
    diagnostics::AppLog::info(
        QStringLiteral("target"),
        QStringLiteral("record-the-monitor-instead: hosting monitor has no capture target - no-op"));
}

void QuickApplication::applyDiagnosticsFix(const QString& fix_id) {
    const diagnostics::FixResult result =
        diagnostics::ApplyAutoFix(fix_id.toStdString(), capabilities_, live_config_.output, live_config_.video);
    if (!result.handled())
        return; // unknown fix id -- no-op, exactly as the Widgets dispatcher did

    if (result.outcome == diagnostics::FixOutcome::RetargetToHostingMonitor) {
        selectHostingMonitorForSelectedWindow();
    } else if (result.outcome == diagnostics::FixOutcome::SettingsChanged) {
        settings_adapter_.setConfig(live_config_);
        recording_coordinator_->SetOutputSettings(live_config_.output);
        recording_coordinator_->SetVideoSettings(live_config_.video);
        persistLiveConfig();
        refreshPresetState();
        synchronizeRecordState();
    }
    refreshDiagnosticsData();
    diagnostics::AppLog::info(QStringLiteral("diagnostics"), QStringLiteral("Applied fix %1").arg(fix_id));
}

void QuickApplication::wireSettingsCommands() {
    QObject::connect(&settings_adapter_, &SettingsAdapter::configEdited, &settings_adapter_,
                     [this]() { applySettingsConfigEdit(); });
    QObject::connect(&settings_adapter_, &SettingsAdapter::appSettingsEdited, &settings_adapter_, [this]() {
        const QString previous_update_channel = settings_.update_channel;
        settings_ = settings_adapter_.appSettings();
        persistAppSettings(SettingsWriteIntent::UserEdit);
        if (settings_.update_channel != previous_update_channel)
            applyUpdateChannel();
        applyThemeFromSettings();
        // Both of these were persisted-and-displayed but never applied: the
        // developer log level left AppLog recording everything regardless of the
        // choice, and the crash-report policy never reached the SDK consent gate.
        applyDeveloperLogLevel();
        applyCrashReportPolicy();
    });

    QObject::connect(&settings_adapter_, &SettingsAdapter::presetSelected, &settings_adapter_,
                     [this](const QString& id) {
                         if (!preset_registry_.SetSelected(id.toStdString()))
                             return;
                         // A preset never overrides the live environment (capture
                         // identity, bit depth, HDR handling) -- that is a machine
                         // fact, not a saved user intent.
                         applyPresetConfig(WithEnvironmentFields(preset_registry_.SelectedSavedConfig(), live_config_));
                     });
    QObject::connect(&settings_adapter_, &SettingsAdapter::savePresetAsRequested, &settings_adapter_,
                     [this](const QString& name) {
                         preset_registry_.AddPreset(live_config_, name.toStdString());
                         persistLiveConfig();
                         refreshPresetState();
                     });
    QObject::connect(&settings_adapter_, &SettingsAdapter::renamePresetRequested, &settings_adapter_,
                     [this](const QString& name) {
                         if (preset_registry_.RenameSelected(name.toStdString())) {
                             persistLiveConfig();
                             refreshPresetState();
                         }
                     });
    QObject::connect(&settings_adapter_, &SettingsAdapter::deletePresetRequested, &settings_adapter_, [this]() {
        if (!preset_registry_.DeleteSelected())
            return;
        applyPresetConfig(WithEnvironmentFields(preset_registry_.SelectedSavedConfig(), live_config_));
    });
    QObject::connect(&settings_adapter_, &SettingsAdapter::resetChangesRequested, &settings_adapter_, [this]() {
        applyPresetConfig(WithEnvironmentFields(preset_registry_.SelectedSavedConfig(), live_config_));
    });
    QObject::connect(&settings_adapter_, &SettingsAdapter::exportPresetRequested, &settings_adapter_,
                     [this](const QString& path) { exportSelectedPreset(path); });
    QObject::connect(&settings_adapter_, &SettingsAdapter::importPresetsRequested, &settings_adapter_,
                     [this](const QString& path) { importPresetsFromFile(path); });
    QObject::connect(&settings_adapter_, &SettingsAdapter::audioRescanRequested, &settings_adapter_,
                     [this]() { audio_notifier_.rescan(); });
}

void QuickApplication::syncConfigMirrors() {
    // Settings mirror. Cheap enough for a user click; deliberately NOT called
    // from the webcam-overlay drag, which is per-frame -- that field is not on
    // the Settings surface at all, so applySettingsConfigEdit restores it from
    // the owner instead of relying on this push.
    settings_adapter_.setConfig(live_config_);
    // Record mirror. The audio rows are editable from BOTH surfaces (Record's
    // source toggles and the Settings audio section write the same enable/
    // separate flags), so a Settings edit has to land back in the view model or
    // the Record page keeps showing -- and recording with -- the old plan.
    record_view_model_.audio_ui_state = live_config_.audio;
    record_view_model_.RebuildAudioPlan();
}

bool QuickApplication::persistAppSettings(SettingsWriteIntent intent) {
    // QCR-201. `settings_` after a failed load is the built-in defaults, not the
    // user's configuration. Persisting it would turn "we could not read your
    // settings" into "your settings are gone" — and the write that does it is
    // almost never one the user asked for: the window-geometry debounce fires on
    // every move and on close, so simply launching and quitting the app used to
    // be enough. The decision itself is the pure ResolveSettingsWrite; this
    // function only carries it out.
    switch (ResolveSettingsWrite(settings_.load_outcome, settings_load_failure_superseded_, intent)) {
    case SettingsWriteDecision::Refuse:
        if (!settings_block_logged_) {
            settings_block_logged_ = true;
            diagnostics::AppLog::warning(
                QStringLiteral("settings"),
                QStringLiteral("Settings were not written: the existing settings file could not be read, so the "
                               "built-in defaults are not being saved over it. Change any setting to start a "
                               "fresh file."));
        }
        return false;
    case SettingsWriteDecision::PreserveThenWrite: {
        // The user is deliberately authoring settings now, so their intent wins
        // over an unreadable file — but the file itself is preserved rather than
        // overwritten, so nothing the user had is destroyed by this decision.
        QString backup_path;
        if (settings_store_.BackupUnreadableFile(&backup_path)) {
            diagnostics::AppLog::info(
                QStringLiteral("settings"),
                QStringLiteral("Unreadable settings file kept as %1; writing a fresh one.").arg(backup_path));
        }
        settings_load_failure_superseded_ = true;
        break;
    }
    case SettingsWriteDecision::Write:
        break;
    }

    if (settings_store_.Save(settings_))
        return true;

    // Same class as a failed preset write, and the same report: the change the
    // user just made may be gone on restart, and saying nothing is not an option.
    diagnostics::AppLog::warning(QStringLiteral("settings"),
                                 QStringLiteral("Failed to write %1").arg(settings_store_.SettingsFilePath()));
    notifications::NotificationEvent event;
    event.type = notifications::NotificationType::SettingsSaveFailed;
    event.title = QStringLiteral("Settings could not be saved");
    event.body = QStringLiteral("The change may be lost when ExoSnap restarts.");
    notifications_adapter_.manager().Enqueue(std::move(event));
    return false;
}

void QuickApplication::saveAndPublishAppSettings(SettingsWriteIntent intent) {
    persistAppSettings(intent);
    settings_adapter_.setAppSettings(settings_);
    // The overlay windows read their enable gates and their content set from the
    // same persisted struct. Published here rather than polled, which is what
    // turns show_recording_overlay / show_diagnostics_overlay from stored
    // preferences into settings that take effect the moment they are toggled.
    overlay_adapter_.setAppSettings(settings_);
}

void QuickApplication::applySettingsConfigEdit() {
    RecordingPresetConfig edited = settings_adapter_.config();
    // The adapter mirrors the whole struct but the Settings surface exposes only
    // part of it. Capture target/region, the countdown and the webcam overlay
    // rect are Record-owned and are never edited here, so the adapter's copies
    // of them are as old as the last push -- taking them wholesale is what
    // reverted the user's capture source the moment they changed a codec.
    edited.capture = live_config_.capture;
    edited.countdown_seconds = live_config_.countdown_seconds;
    edited.webcam.overlay = live_config_.webcam.overlay;
    live_config_ = std::move(edited);
    // Audio rows and the webcam enable/mirror flags ARE editable here, so the
    // Record-side mirror has to follow.
    record_view_model_.audio_ui_state = live_config_.audio;
    record_view_model_.RebuildAudioPlan();
    refreshDiagnosticsData();
    recording_coordinator_->SetOutputSettings(live_config_.output);
    recording_coordinator_->SetVideoSettings(live_config_.video);
    recording_coordinator_->SetWebcamSettings(live_config_.webcam);
    recording_coordinator_->SetSplitSettings(
        {SplitDurationMs(live_config_.output.split), SplitSizeBytes(live_config_.output.split)});
    persistLiveConfig();
    refreshPresetState();
    refreshCrashSessionContext();
    synchronizeRecordState();
}

void QuickApplication::applyPresetConfig(RecordingPresetConfig config) {
    live_config_ = SanitizePresetConfig(std::move(config));
    settings_adapter_.setConfig(live_config_);
    recording_coordinator_->SetOutputSettings(live_config_.output);
    recording_coordinator_->SetVideoSettings(live_config_.video);
    recording_coordinator_->SetWebcamSettings(live_config_.webcam);
    recording_coordinator_->SetSplitSettings(
        {SplitDurationMs(live_config_.output.split), SplitSizeBytes(live_config_.output.split)});
    persistLiveConfig();
    refreshPresetState();
    refreshCrashSessionContext();
    synchronizeRecordState();
}

void QuickApplication::refreshPresetState() {
    QVariantList options;
    for (const RecordingPreset& preset : preset_registry_.Presets()) {
        QVariantMap entry;
        entry.insert(QStringLiteral("value"), QString::fromStdString(preset.id));
        entry.insert(QStringLiteral("label"), QString::fromStdString(preset.name));
        entry.insert(QStringLiteral("selectable"), true);
        entry.insert(QStringLiteral("reason"), QString());
        options.append(entry);
    }
    settings_adapter_.setPresetState(std::move(options), QString::fromStdString(preset_registry_.SelectedId()),
                                     preset_registry_.IsSelectedDirty(live_config_));
}

void QuickApplication::exportSelectedPreset(const QString& path) {
    if (path.isEmpty())
        return;
    RecordingPreset preset;
    preset.id = preset_registry_.SelectedId();
    preset.name = preset_registry_.SelectedPreset().name;
    preset.config = StripEnvironmentFields(live_config_);
    QString error;
    if (!RecordingPresetStore::ExportPresetToFile(preset, path, &error))
        record_view_model_adapter_.setNoticeText(QStringLiteral("Preset export failed · %1").arg(error),
                                                 QStringLiteral("error"));
}

void QuickApplication::importPresetsFromFile(const QString& path) {
    if (path.isEmpty())
        return;
    std::vector<std::string> existing_ids;
    existing_ids.reserve(preset_registry_.Count());
    for (const RecordingPreset& preset : preset_registry_.Presets())
        existing_ids.push_back(preset.id);

    QString error;
    const QVector<RecordingPreset> imported = RecordingPresetStore::ImportPresetsFromFile(path, existing_ids, &error);
    if (imported.isEmpty()) {
        record_view_model_adapter_.setNoticeText(QStringLiteral("Preset import failed · %1").arg(error),
                                                 QStringLiteral("error"));
        return;
    }
    for (const RecordingPreset& preset : imported)
        preset_registry_.ImportPreset(preset);
    persistLiveConfig();
    refreshPresetState();
}

void QuickApplication::initializeHotkeys() {
    hotkey_service_.LoadFromStrings(settings_.hotkey_bindings);
    refreshHotkeyRows();

    QObject::connect(&settings_adapter_, &SettingsAdapter::hotkeyRebindRequested, &settings_adapter_,
                     [this](int action, int key, int modifiers) {
                         const auto hotkey_action = static_cast<HotkeyAction>(action);
                         const RebindResult result =
                             hotkey_service_.TrySetBinding(hotkey_action, QKeySequence(key | modifiers));
                         if (!result.success) {
                             settings_adapter_.setHotkeyError(action, result.error_message);
                             return;
                         }
                         hotkey_service_.SaveToStrings(settings_.hotkey_bindings);
                         saveAndPublishAppSettings(SettingsWriteIntent::UserEdit);
                         refreshHotkeyRows();
                     });
    QObject::connect(&settings_adapter_, &SettingsAdapter::hotkeyClearRequested, &settings_adapter_,
                     [this](int action) {
                         hotkey_service_.UnsetBinding(static_cast<HotkeyAction>(action));
                         hotkey_service_.SaveToStrings(settings_.hotkey_bindings);
                         saveAndPublishAppSettings(SettingsWriteIntent::UserEdit);
                         refreshHotkeyRows();
                     });
    QObject::connect(&settings_adapter_, &SettingsAdapter::hotkeyResetRequested, &settings_adapter_,
                     [this](int action) {
                         const RebindResult result = hotkey_service_.ResetToDefault(static_cast<HotkeyAction>(action));
                         if (!result.success) {
                             settings_adapter_.setHotkeyError(action, result.error_message);
                             return;
                         }
                         hotkey_service_.SaveToStrings(settings_.hotkey_bindings);
                         saveAndPublishAppSettings(SettingsWriteIntent::UserEdit);
                         refreshHotkeyRows();
                     });
}

void QuickApplication::refreshHotkeyRows() {
    QVariantList rows;
    for (int i = 0; i < kHotkeyActionCount; ++i) {
        const auto action = static_cast<HotkeyAction>(i);
        QVariantMap row;
        row.insert(QStringLiteral("action"), i);
        row.insert(QStringLiteral("label"), GlobalHotkeyService::ActionDisplayName(action));
        row.insert(QStringLiteral("binding"), hotkey_service_.GetBinding(action).toString(QKeySequence::NativeText));
        row.insert(QStringLiteral("isDefault"), hotkey_service_.IsAtDefault(action));
        rows.append(row);
    }
    settings_adapter_.setHotkeyRows(std::move(rows));
}

void QuickApplication::triggerHotkeyAction(HotkeyAction action) {
    switch (action) {
    case HotkeyAction::ToggleRecording:
        if (record_view_model_adapter_.recording() || record_view_model_adapter_.paused())
            recording_coordinator_->StopRecording();
        else if (record_view_model_adapter_.countdownActive())
            cancelCountdown();
        else if (record_view_model_adapter_.canStart())
            startRequested();
        break;
    case HotkeyAction::TogglePause:
        if (record_view_model_adapter_.canPause())
            recording_coordinator_->PauseRecording();
        else if (record_view_model_adapter_.canResume())
            recording_coordinator_->ResumeRecording();
        break;
    case HotkeyAction::CaptureFrame:
        record_view_model_adapter_.requestCaptureFrame();
        break;
    case HotkeyAction::AddMarker:
        record_view_model_adapter_.requestAddMarker();
        break;
    case HotkeyAction::SplitRecording:
        record_view_model_adapter_.requestSplit();
        break;
    }
}

void QuickApplication::applyThemeFromSettings() {
    // The token singleton is engine-owned; resolving it here keeps QML free of
    // any theme-selection logic beyond binding to the resolved colours.
    if (auto* tokens = engine_.singletonInstance<QuickThemeTokens*>(QStringLiteral("ExoSnap.Quick"),
                                                                    QStringLiteral("QuickThemeTokens"))) {
        tokens->setAppearance(settings_.appearance_id, settings_.accent_id);
    }
}

void QuickApplication::persistLiveConfig() {
    QString error;
    if (preset_store_.Save(preset_registry_.Presets(), preset_registry_.SelectedId(),
                           SanitizePresetConfig(live_config_), &error)) {
        return;
    }
    // A failed write means the change the user just made may be lost on restart.
    // The Record-page notice only reaches whoever is looking at Record, so this
    // also goes to the hub, where it stays until acknowledged.
    record_view_model_adapter_.setNoticeText(QStringLiteral("Settings could not be saved · %1").arg(error),
                                             QStringLiteral("error"));
    notifications::NotificationEvent event;
    event.type = notifications::NotificationType::SettingsSaveFailed;
    event.title = QStringLiteral("Settings could not be saved");
    event.body = error.isEmpty() ? QStringLiteral("The change may be lost when ExoSnap restarts.") : error;
    notifications_adapter_.manager().Enqueue(std::move(event));
}

// Harness-only, env-configured (never mouse/keyboard synthesis, never a window):
// seeds deterministic Diagnostics/Logs content so a --visual-test capture shows a
// stated state instead of whatever this machine happened to be doing.
void QuickApplication::applyDiagnosticsVisualScenarios() {
    const QByteArray log_scenario = qgetenv("EXOSNAP_VISUAL_LOG_SCENARIO");
    if (!log_scenario.isEmpty()) {
        const QDateTime base(QDate(2026, 6, 8), QTime(14, 22, 31, 123));
        const auto entry = [&base](int index, diagnostics::LogSeverity severity, const char* category,
                                   const char* message) {
            return diagnostics::LogEntry{static_cast<quint64>(index + 1), base.addMSecs(index * 137), severity,
                                         QString::fromUtf8(category), QString::fromUtf8(message)};
        };
        QVector<diagnostics::LogEntry> entries;
        if (log_scenario == "empty") {
            // deliberately left empty
        } else if (log_scenario == "long-message") {
            entries.push_back(entry(0, diagnostics::LogSeverity::Warning, "Output",
                                    "Output folder validation returned a long recoverable warning with enough detail "
                                    // A placeholder root rather than a real one: the fixture exists to make the
                                    // surface wrap a long unbroken run, and it must not bake anybody's disk
                                    // layout into the source to do it.
                                    "to wrap across the log surface while remaining selectable and copyable: "
                                    "<output folder>/Very/Long/Path/That/Still/Needs/To/Be/Readable"));
        } else {
            entries.push_back(entry(0, diagnostics::LogSeverity::Debug, "Preview", "DXGI preview crop resolved"));
            entries.push_back(entry(1, diagnostics::LogSeverity::Info, "Record", "Recording profile loaded"));
            entries.push_back(
                entry(2, diagnostics::LogSeverity::Info, "Capture", "Monitor 1 selected as capture target"));
            entries.push_back(entry(3, diagnostics::LogSeverity::Warning, "Webcam", "Device disconnected; PiP hidden"));
            entries.push_back(
                entry(4, diagnostics::LogSeverity::Error, "Encoder", "AV1 encoder initialization failed"));
            entries.push_back(entry(5, diagnostics::LogSeverity::Info, "Mux", "Matroska writer opened"));
            entries.push_back(entry(6, diagnostics::LogSeverity::Debug, "Audio", "System loopback meter started"));
        }
        logs_adapter_.setSyntheticEntries(std::move(entries));
    }

    // Reviewing the Expert taxonomy must not mean flipping the developer's own
    // persisted Expert setting, so the harness overrides it in-memory only.
    if (qgetenv("EXOSNAP_VISUAL_DIAG_EXPERT") == "1") {
        visual_expert_override_ = true;
        diagnostics_adapter_.setExpertMode(true);
    }

    const QByteArray diag_scenario = qgetenv("EXOSNAP_VISUAL_DIAG_SCENARIO");
    if (diag_scenario == "issues") {
        diagnostics::DiagnosticsController::Config config;
        config.caps = capabilities_;
        config.audio = record_view_model_.audio_ui_state;
        config.user_config = diagnostics::UserConfigFromSettings(live_config_.output, live_config_.video);
        config.cap_summary = diagnostics::CapabilitySummary::FromCapabilitySet(capabilities_);
        config.config_summary = diagnostics::ConfigSummary::FromCurrentSettings(
            live_config_.output, live_config_.video, config.audio,
            std::filesystem::path(settings_store_.SettingsFilePath().toStdWString()),
            preset_registry_.SelectedPreset().name, std::string());
        config.output_folder = live_config_.output.output_folder.string();
        config.profile_validation.succeeded = false;
        config.profile_validation.invalidity.push_back(
            {"video_codec", "AV1 encoding is not available on the selected adapter."});
        config.profile_validation.warnings.push_back(
            {"cfg.quality.untested", "This quality target has not been validated on this machine yet."});
        config.hotkeys_ok = false;
        config.hotkeys_summary = "Ctrl+Shift+R";
        diagnostics_adapter_.setDiagnosticConfig(std::move(config));
        diagnostics_visual_scenario_active_ = true;
    }
}

// Harness-only, env-configured. A real Edit surface needs a finished recording
// on this machine; a --visual-test capture must not depend on one, and must
// never start a real decode or a real export. The fixture context deliberately
// leaves mkv_master_path empty: nothing is opened, nothing is remuxed, and the
// timeline strip comes from the deterministic placeholder fixture.
void QuickApplication::applyEditVisualScenario() {
    const QByteArray raw = qgetenv("EXOSNAP_VISUAL_EDIT_SCENARIO");
    if (raw.isEmpty())
        return;
    const QString scenario = QString::fromUtf8(raw).trimmed().toLower();

    // The fixture's directory is resolved at runtime, never written into the
    // source. A hard-coded absolute path bakes one machine's disk layout into
    // the repository and then shows it to every reviewer in a screenshot.
    const QString fixture_directory =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/exosnap-visual-fixtures");

    EditContext context;
    context.output_path = fixture_directory + QStringLiteral("/2026-08-10 21-14-08.mkv");
    context.duration = QStringLiteral("2:34");
    context.size = QStringLiteral("412 MB");
    context.resolution = QStringLiteral("2560x1440");
    context.fps = QStringLiteral("60 fps CFR");
    context.video_codec = QStringLiteral("AV1 (NVENC)");
    context.audio_codec = QStringLiteral("Opus");
    context.container = QStringLiteral("MKV");
    context.duration_seconds = 154.0;
    context.av_drift_available = true;
    context.peak_av_drift_ms = 3.0;
    context.completed_snapshot.valid = true;
    context.completed_snapshot.session_generation = 7;
    context.completed_snapshot.capture.frames_emitted = 9240;
    context.completed_snapshot.health = recorder_core::PipelineHealth::Good;
    context.markers = {
        RecordingMarker{18000, RecordingMarkerType::General, "Intro"},
        RecordingMarker{47500, RecordingMarkerType::Highlight, "Highlight"},
        RecordingMarker{96000, RecordingMarkerType::Cut, "Cut here"},
        RecordingMarker{131000, RecordingMarkerType::General, "Outro"},
    };

    if (scenario == QStringLiteral("edit-report-warning")) {
        context.completed_snapshot.health = recorder_core::PipelineHealth::Warning;
        context.completed_snapshot.capture.frames_dropped_backpressure = 122;
        context.peak_av_drift_ms = 41.0;
    } else if (scenario == QStringLiteral("edit-long-filename")) {
        // The header's width budget, exercised. Back on one end and the report
        // status on the other are fixed; the clip name is the only element that
        // may give, and at the 860 px minimum window there is very little to
        // give. A name this long is not hypothetical — a window-capture
        // recording is named after the window title.
        context.output_path = fixture_directory + QStringLiteral("/2026-08-10 21-14-08 - Sprint demo, full "
                                                                 "walkthrough with the diagnostics overlay "
                                                                 "enabled (take 3).mkv");
    }

    edit_session_adapter_.setEditContext(context);
    // Keyframes every two seconds, so a trim snap in the harness lands where a
    // real cue table would put it.
    std::vector<int64_t> keyframes;
    for (int64_t us = 0; us <= 154'000'000; us += 2'000'000)
        keyframes.push_back(us);
    edit_session_adapter_.setKeyframeTimestampsForTest(std::move(keyframes));

    const bool multitrack = scenario == QStringLiteral("edit-timeline-multitrack");
    const QStringList audio_rows =
        multitrack ? QStringList{QStringLiteral("Game"), QStringLiteral("System"), QStringLiteral("Microphone")}
                   : QStringList{QStringLiteral("System")};
    const bool unavailable_previews = scenario == QStringLiteral("edit-timeline-unavailable");
    const int tile_count = scenario == QStringLiteral("edit-timeline-loading") ? 4 : unavailable_previews ? 0 : -1;
    edit_timeline_adapter_.setFixture(audio_rows, tile_count);
    // QCR-307: the terminal state, which no fixture can reach on its own — the
    // fixture path never touches the decoder whose failure produces it.
    if (unavailable_previews)
        edit_timeline_adapter_.applyUnavailablePreviewsForHarness();

    if (scenario == QStringLiteral("edit-trimmed"))
        edit_session_adapter_.requestTrim(22000, 118000);

    if (scenario == QStringLiteral("edit-export-running")) {
        edit_export_adapter_.applyVisualState(EditExportAdapter::Running, 42, QString(), QString());
    } else if (scenario == QStringLiteral("edit-export-done")) {
        edit_export_adapter_.applyVisualState(EditExportAdapter::Done, 100,
                                              fixture_directory + QStringLiteral("/2026-08-10 21-14-08_edit.mkv"),
                                              QString());
    } else if (scenario == QStringLiteral("edit-export-failed")) {
        edit_export_adapter_.applyVisualState(EditExportAdapter::Failed, 63, QString(),
                                              QStringLiteral("Failed to save output file: disk full"));
    }
}

// The Edit surface is a leaf of the session adapter: the timeline, the player
// and the export panel all hang off it and never talk to each other. Nothing
// here touches RecordingCoordinator or a service registry -- the surface is
// handed a finished recording's context, not a live pipeline.
void QuickApplication::initializeEditArea() {
    edit_timeline_adapter_.setTileProvider(edit_tile_provider_);
    edit_timeline_adapter_.setSession(&edit_session_adapter_);
    edit_player_adapter_.setSession(&edit_session_adapter_);
    edit_export_adapter_.setSession(&edit_session_adapter_);
}

// ---------------------------------------------------------------------------
// Updates (ADR 0012)
// ---------------------------------------------------------------------------
//
// Before this existed the Settings updates card was the worst kind of unfinished
// surface: it rendered a state machine nobody drove, its status/action labels
// were empty strings, and its button emitted a signal with no listener. The user
// saw a blank, clickable control that did nothing.

void QuickApplication::initializeUpdates() {
    update_service_ = std::make_unique<UpdateService>(recording_coordinator_.get());
    update_service_->SetChannel(UpdateChannelFromString(settings_.update_channel));

    // Handoff truth belongs to the process that accepted the updater's marked
    // close request. A fresh process must never reconstruct "Restart pending"
    // from that stale stamp.
    const QString reconciled = ReconcileAppliedVersionOnStartup(settings_.applied_version);
    if (settings_.applied_version != reconciled) {
        settings_.applied_version = reconciled;
        saveAndPublishAppSettings();
    }

    QObject::connect(update_service_.get(), &UpdateService::updateCheckComplete, &settings_adapter_,
                     [this](const exosnap::update::UpdateCheckResult& result) { onUpdateCheckComplete(result); });

    // Launch and close-handoff are separate states: the download can still fail
    // or be cancelled while this process is alive, so spawning the detached
    // updater must never claim a restart is pending.
    QObject::connect(update_service_.get(), &UpdateService::updaterLaunched, &settings_adapter_, [this]() {
        update_handoff_phase_ = UpdateHandoffPhase::UpdaterRunning;
        settings_adapter_.setUpdateStatus(QStringLiteral("updater-running"), last_available_version_, QString());
        diagnostics::AppLog::info(
            QStringLiteral("update"),
            QStringLiteral("Updater launched for %1; waiting for close handoff").arg(last_available_version_));
    });
    QObject::connect(update_service_.get(), &UpdateService::updaterExited, &settings_adapter_,
                     [this](qint64 process_id, quint32 exit_code) {
                         if (update_handoff_phase_ != UpdateHandoffPhase::UpdaterRunning)
                             return;
                         update_handoff_phase_ = UpdateHandoffPhase::Idle;
                         settings_adapter_.setUpdateStatus(verify_update_reinstall_ ? QStringLiteral("verify-reinstall")
                                                                                    : QStringLiteral("available"),
                                                           last_available_version_, QString());
                         diagnostics::AppLog::warning(
                             QStringLiteral("update"),
                             QStringLiteral("Updater process %1 exited before close handoff (code %2); card re-armed")
                                 .arg(process_id)
                                 .arg(exit_code));
                     });
    QObject::connect(update_service_.get(), &UpdateService::updateError, &settings_adapter_,
                     [this](exosnap::update::VerifyResult, const QString& detail) {
                         update_handoff_phase_ = UpdateHandoffPhase::Idle;
                         settings_adapter_.setUpdateStatus(QStringLiteral("error"), QString(), QString(), detail);
                         diagnostics::AppLog::warning(QStringLiteral("update"),
                                                      QStringLiteral("Updater launch failed: %1").arg(detail));
                     });

    QObject::connect(&settings_adapter_, &SettingsAdapter::checkForUpdatesRequested, &settings_adapter_,
                     [this]() { triggerUpdateCheck(/*manual=*/true); });
    QObject::connect(&settings_adapter_, &SettingsAdapter::updatePrimaryActionRequested, &settings_adapter_,
                     [this]() { runUpdatePrimaryAction(); });
    QObject::connect(&settings_adapter_, &SettingsAdapter::diagnosticsRequested, &settings_adapter_,
                     [this]() { emit shell_adapter_.navigateToPageRequested(ShellAdapter::DiagnosticsPage); });

    // Startup state: no check has run yet, so the card says exactly that rather
    // than claiming "Up to date" on the strength of nothing.
    settings_adapter_.setUpdateStatus(QStringLiteral("unchecked"), QString(), QString());
    if (settings_.check_updates_on_start)
        triggerUpdateCheck(/*manual=*/false);
}

// QCR-202. The selected channel used to reach UpdateService exactly once, in
// initializeUpdates(): picking Preview persisted the choice and changed the
// About page, but every check in that session still queried Stable, and only the
// next launch honoured the selection. Applying it here also invalidates the
// card, because "Update available — <ver>" was an answer about the feed the user
// just left. The card returns to the same "unchecked" state a fresh launch
// shows; no automatic network check is started, since a check is the user's
// explicit action (ADR 0045) and the card's own button is right there.
void QuickApplication::applyUpdateChannel() {
    if (!update_service_)
        return;
    update_service_->SetChannel(UpdateChannelFromString(settings_.update_channel));
    last_available_version_.clear();
    update_handoff_phase_ = UpdateHandoffPhase::Idle;
    settings_adapter_.setUpdateStatus(QStringLiteral("unchecked"), QString(), QString());
    diagnostics::AppLog::info(QStringLiteral("update"),
                              QStringLiteral("Update channel set to %1").arg(settings_.update_channel));
}

void QuickApplication::closeForUpdaterHandoff() {
    // Idempotent: the updater posts once, but a message loop is not a promise of
    // exactly-once delivery, and quitting twice would race the teardown.
    if (update_handoff_phase_ == UpdateHandoffPhase::ClosingForHandoff)
        return;

    // The one refusal. The recording guard that blocked STARTING this update is
    // not weakened by the handoff: a capture or remux can have begun between the
    // launch and this request, and ending the process then would lose the
    // recording. The updater's own answer for a parent that will not close is
    // appWontClose (B1) with the installation untouched -- which is the truth.
    const UiRecordingState state = record_view_model_.state;
    if (state != UiRecordingState::Ready && state != UiRecordingState::Completed && state != UiRecordingState::Failed &&
        state != UiRecordingState::Blocked) {
        diagnostics::AppLog::warning(
            QStringLiteral("update"),
            QStringLiteral("The updater asked ExoSnap to close for the swap, but a recording is in flight; "
                           "refusing. The update will report that the app would not close."));
        return;
    }

    update_handoff_phase_ = UpdateHandoffPhase::ClosingForHandoff;
    settings_adapter_.setUpdateStatus(QStringLiteral("pending"), last_available_version_, QString());
    diagnostics::AppLog::info(
        QStringLiteral("update"),
        QStringLiteral("The updater has verified version %1 and asked ExoSnap to close for the swap")
            .arg(last_available_version_.isEmpty() ? QStringLiteral("the update") : last_available_version_));

    // quit(), NOT the window close the tray's "Quit" drives. Closing the window
    // runs the close-guard chain and the hide-to-tray decision, and a
    // tray-resident process still holds exosnap.exe locked -- which is exactly
    // the file the updater is about to rename. The one guard that must still
    // apply, a recording in flight, is checked above, explicitly, rather than
    // inherited from a chain that can also decide to keep the process alive.
    //
    // Ending the event loop is enough: QuickApplication's destructor performs
    // the same geometry/persist flush every other exit relies on.
    QCoreApplication::quit();
}

QString QuickApplication::updateBlockerReason() const {
    // App-layer recording guard: never contact the update server while a capture
    // or remux is in flight.
    const UiRecordingState state = record_view_model_.state;
    if (state == UiRecordingState::Saving || state == UiRecordingState::Stopping)
        return QStringLiteral("finalizing");
    if (state != UiRecordingState::Ready && state != UiRecordingState::Completed && state != UiRecordingState::Failed &&
        state != UiRecordingState::Blocked)
        return QStringLiteral("recording");
    // A handoff in flight owns the update area: the card's action is disabled
    // while the updater runs, and starting a second one would be a second swap.
    if (update_handoff_phase_ != UpdateHandoffPhase::Idle)
        return QStringLiteral("updaterRunning");
    return {};
}

void QuickApplication::triggerUpdateCheck(bool manual) {
    if (!update_service_)
        return;
    if (const QString blocker = updateBlockerReason(); !blocker.isEmpty()) {
        // "updaterRunning" deliberately writes nothing: the card is already
        // showing "Updater running…" / "Restart pending", and replacing that
        // with an error would describe the handoff as a fault.
        if (blocker != QLatin1String("updaterRunning")) {
            settings_adapter_.setUpdateStatus(
                QStringLiteral("error"), QString(), QString(),
                QStringLiteral("Update checks are paused while a recording is in progress."));
        }
        return;
    }
    if (manual) {
        // A manual check clears the loop guard so a still-applicable version that
        // got stuck on "Restart pending" can resolve to "available" again.
        update_handoff_phase_ = UpdateHandoffPhase::Idle;
        if (!settings_.applied_version.isEmpty()) {
            settings_.applied_version.clear();
            saveAndPublishAppSettings();
        }
    }
    settings_adapter_.setUpdateStatus(QStringLiteral("checking"), QString(), QString());
    update_service_->RequestUpdateCheck();
}

void QuickApplication::onUpdateCheckComplete(const exosnap::update::UpdateCheckResult& result) {
    const QString current_version = QString::fromLatin1(exosnap::build::kVersion);
    const QString last_checked = QDateTime::currentDateTime().toString(QStringLiteral("MMM d, h:mm AP"));

    if (result.check_failed || result.error_message) {
        const QString error_message = result.error_message ? QString::fromStdString(*result.error_message)
                                                           : QStringLiteral("Couldn't reach the update server.");
        settings_adapter_.setUpdateStatus(QStringLiteral("error"), QString(), QString(), error_message);
        // "disabled (unofficial build)" is an expected condition, not a failure.
        if (error_message.contains(QStringLiteral("disabled"), Qt::CaseInsensitive))
            diagnostics::AppLog::info(QStringLiteral("update"),
                                      QStringLiteral("Update check skipped: %1").arg(error_message));
        else
            diagnostics::AppLog::warning(QStringLiteral("update"),
                                         QStringLiteral("Update check failed: %1").arg(error_message));
        return;
    }

    // A verification reinstall was granted on byte-identical version STRINGS, so
    // the running version string is the truthful label for it.
    // The release tag verbatim, not SemVer::ToString(): this string is what the
    // card offers, what the loop guard remembers and -- through
    // --target-version -- what the updater is pinned to install, so all three
    // have to be the same bytes the signed manifest carries.
    last_available_version_ =
        result.verification_reinstall ? current_version : QString::fromStdString(result.available_version_raw);

    const bool is_scoop = UpdateService::IsScoopManagedInstall(QCoreApplication::applicationDirPath());
    const QString card_state = exosnap::ResolveUpdateCardState(
        result.update_available, is_scoop, settings_.applied_version, last_available_version_, verify_update_reinstall_,
        current_version, update_handoff_phase_);
    settings_adapter_.setUpdateStatus(card_state, last_available_version_, last_checked);

    diagnostics::AppLog::info(
        QStringLiteral("update"),
        result.verification_reinstall
            ? QStringLiteral("Verification reinstall offered for %1").arg(last_available_version_)
        : result.update_available
            ? QStringLiteral("Update available: %1 → %2").arg(current_version, last_available_version_)
            : QStringLiteral("Up to date (%1)").arg(current_version));

    // Notify-on-available. A verification reinstall is not an available update
    // and must not be advertised as one -- the user asked for it explicitly.
    if (result.update_available && !result.verification_reinstall && !is_scoop) {
        notifications::NotificationEvent event;
        event.type = notifications::NotificationType::UpdateAvailable;
        event.title = QStringLiteral("Update available");
        event.body = QStringLiteral("Version %1 is ready to install.").arg(last_available_version_);
        event.action = notifications::NotificationAction::OpenUpdate;
        notifications_adapter_.manager().Enqueue(std::move(event));
    }
}

void QuickApplication::runUpdatePrimaryAction() {
    if (!update_service_)
        return;
    const QString state = settings_adapter_.updateState();
    if (state == QLatin1String("available") || state == QLatin1String("verify-reinstall")) {
        update_service_->LaunchUpdater();
        return;
    }
    if (state == QLatin1String("scoop")) {
        // Notify-only: a Scoop tree is never touched by the staged swap.
        record_view_model_adapter_.setNoticeText(
            QStringLiteral("This install is managed by Scoop — run `scoop update exosnap`."));
        return;
    }
    // Every other state's action is "check again".
    triggerUpdateCheck(/*manual=*/true);
}

void QuickApplication::applyStartupRelaunchHandoff(const QString& page_name, bool reenable_present_diag) {
    // ADR 0033. Land on the page the pre-elevation instance was showing.
    static const std::array<std::pair<const char*, ShellAdapter::Page>, 5> kNavLabels{{
        {"Record", ShellAdapter::RecordPage},
        {"Settings", ShellAdapter::SettingsPage},
        {"Diagnostics", ShellAdapter::DiagnosticsPage},
        {"Logs", ShellAdapter::LogsPage},
        {"About", ShellAdapter::AboutPage},
    }};
    for (const auto& [label, page] : kNavLabels) {
        if (page_name.compare(QLatin1StringView(label), Qt::CaseInsensitive) == 0) {
            pending_landing_page_ = page;
            break;
        }
    }

    // The relaunch succeeded (we are running), so it is now safe to persist the
    // opt-in the user toggled before the restart. A UAC decline never gets here,
    // which is the whole point of deferring the write.
    if (reenable_present_diag && !settings_.present_diagnostics_optin) {
        settings_.present_diagnostics_optin = true;
        saveAndPublishAppSettings(SettingsWriteIntent::UserEdit);
        diagnostics::AppLog::info(QStringLiteral("diagnostics"),
                                  QStringLiteral("Present-diagnostics opt-in re-enabled after elevated relaunch."));
    }
}

void QuickApplication::applyTraySuppression(bool suppressed) {
    tray_suppressed_ = suppressed;
}

void QuickApplication::applyUpdateFeedOverride(const QString& base_url) {
    if (update_service_)
        update_service_->SetDevFeedOverride(base_url);
}

void QuickApplication::applyUpdaterAutomationRunId(const QString& run_id) {
    if (update_service_)
        update_service_->SetUpdaterAutomationRunId(run_id);
}

void QuickApplication::requestUpdateCheck() {
    // Manual: the same call the card's button makes, so the loop-guard reset and
    // the app-layer recording guard both apply.
    triggerUpdateCheck(/*manual=*/true);
}

void QuickApplication::requestUpdatePrimaryAction() {
    runUpdatePrimaryAction();
}

const UpdateService* QuickApplication::updateService() const noexcept {
    return update_service_.get();
}

UpdateService* QuickApplication::updateService() noexcept {
    return update_service_.get();
}

void QuickApplication::applyVerifyUpdateReinstallMode(bool enabled) {
    verify_update_reinstall_ = enabled;
    if (!enabled)
        return;
    if (update_service_)
        update_service_->SetVerifyReinstallMode(true);
}

void QuickApplication::initializeNotifications() {
    QObject::connect(&notifications_adapter_, &NotificationsAdapter::actionTriggered, &notifications_adapter_,
                     [this](notifications::NotificationAction action, const QString& payload) {
                         dispatchNotificationAction(action, payload);
                     });

    // QCR-201. Not the same report as SettingsRepaired: nothing was recovered
    // and nothing was written. The body says both facts the user needs — the
    // session is running on defaults, and their file is still there.
    if (settings_load_failed_pending_) {
        settings_load_failed_pending_ = false;
        diagnostics::AppLog::warning(
            QStringLiteral("settings"),
            QStringLiteral("Settings file could not be read (%1); running on defaults without overwriting it")
                .arg(settings_store_.SettingsFilePath()));
        notifications::NotificationEvent event;
        event.type = notifications::NotificationType::SettingsLoadFailed;
        event.title = QStringLiteral("Settings could not be read");
        event.body = QStringLiteral("ExoSnap is running with default settings. Your settings file is left "
                                    "untouched — changing any setting starts a fresh one and keeps the old "
                                    "file as settings.ini.corrupt.");
        notifications_adapter_.manager().Enqueue(std::move(event));
    }

    if (preset_store_repaired_) {
        preset_store_repaired_ = false;
        diagnostics::AppLog::warning(QStringLiteral("preset"),
                                     QStringLiteral("Preset store repaired field-wise on load"));
        notifications::NotificationEvent event;
        event.type = notifications::NotificationType::SettingsRepaired;
        event.title = QStringLiteral("Settings repaired");
        event.body = QStringLiteral("Some saved settings were invalid and have been repaired.");
        notifications_adapter_.manager().Enqueue(std::move(event));
    }
}

void QuickApplication::initializeDisplayGeometryWatch() {
    // Everything that can move the recorded monitor's rectangle without moving
    // the capture target. The overlays are frameless top-level windows placed
    // against the desktop in virtual-screen coordinates, and the adapter caches
    // that rectangle per HMONITOR — a resolution switch, a scale change or a
    // display being rearranged all keep the handle and change the rectangle, so
    // each of them has to say so.
    const auto invalidate = [this]() {
        overlay_adapter_.invalidateMonitorGeometry();
        // Applied now rather than at the next tick: with no recording running
        // nothing else drives synchronize(), and the overlays would carry the
        // stale rectangle into the recording that starts after the change.
        overlay_adapter_.synchronize();
    };

    const auto watch_screen = [this, invalidate](QScreen* screen) {
        if (screen == nullptr)
            return;
        QObject::connect(screen, &QScreen::geometryChanged, &overlay_adapter_,
                         [invalidate](const QRect&) { invalidate(); });
        QObject::connect(screen, &QScreen::availableGeometryChanged, &overlay_adapter_,
                         [invalidate](const QRect&) { invalidate(); });
        // Both DPI signals: a scale change moves the virtual-screen rectangle of
        // every display to the right of the one that changed, not only its own.
        QObject::connect(screen, &QScreen::logicalDotsPerInchChanged, &overlay_adapter_,
                         [invalidate](qreal) { invalidate(); });
        QObject::connect(screen, &QScreen::physicalDotsPerInchChanged, &overlay_adapter_,
                         [invalidate](qreal) { invalidate(); });
    };

    for (QScreen* screen : QGuiApplication::screens())
        watch_screen(screen);

    QObject::connect(qGuiApp, &QGuiApplication::screenAdded, &overlay_adapter_,
                     [watch_screen, invalidate](QScreen* screen) {
                         watch_screen(screen);
                         invalidate();
                     });
    QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, &overlay_adapter_,
                     [invalidate](QScreen*) { invalidate(); });
    QObject::connect(qGuiApp, &QGuiApplication::primaryScreenChanged, &overlay_adapter_,
                     [invalidate](QScreen*) { invalidate(); });
}

void QuickApplication::initializeBlockingSurfaces() {
    surface_arbiter_.setSurfaces(&recovery_adapter_, &crash_report_adapter_, &recording_error_adapter_);
    // The arbiter decides WHEN each surface may come up; only this class can
    // build what they show.
    QObject::connect(&surface_arbiter_, &BlockingSurfaceArbiter::crashSurfaceRequested, &crash_report_adapter_,
                     [this]() { showCrashReportSurface(); });
    QObject::connect(&surface_arbiter_, &BlockingSurfaceArbiter::recordingErrorSurfaceRequested,
                     &recording_error_adapter_, [this]() {
                         if (!pending_recording_failure_)
                             return;
                         const models::RecordingFailureReport report = *pending_recording_failure_;
                         const bool can_send = pending_recording_failure_can_send_;
                         pending_recording_failure_.reset();
                         recording_error_adapter_.present(report, can_send);
                     });
}

// QCR-415. The recording-error surface used to be raised straight from the
// result callback with no reference to the other two, so a failure delivered
// while recovery or the crash prompt was up produced two active modal loaders —
// the covered one still holding focusable controls, which is exactly what
// QCR-403 centralized the other two to prevent. The path is reachable because
// the start hotkey is deliberately desktop-wide and a modal scrim inside the
// shell does not reach it.
void QuickApplication::presentRecordingFailure(const models::RecordingFailureReport& report, bool can_send_report) {
    // A later failure replaces an earlier one that is still waiting: the surface's
    // own rule is that the newest attempt is the one the user just made, and that
    // has to hold whether it was raised or queued.
    pending_recording_failure_ = report;
    pending_recording_failure_can_send_ = can_send_report;
    surface_arbiter_.requestRecordingError();
}

void QuickApplication::initializeRecovery() {
    // The stored destination folder may be gone by the time a recovery runs (an
    // unplugged drive, a folder the user has since moved). The service falls
    // back to the currently configured output folder rather than failing.
    recovery_service_.SetFallbackOutputFolder(QString::fromStdString(live_config_.output.output_folder.string()));
    recovery_adapter_.setService(&recovery_service_);

    // "Continue" is the coordinator's business, not the adapter's — same
    // contract every other adapter here follows. The manifest does not record
    // the original capture target, so the session is armed as requiring
    // re-selection, exactly as RecordPage::armFromRecovery does on the Widgets
    // side.
    QObject::connect(&recovery_adapter_, &RecoveryAdapter::continueRequested, &recovery_adapter_,
                     [this](const RecoveryManifestEntry& entry) {
                         RecordingCoordinator::RecoverySessionInfo info;
                         info.manifest_entry = entry;
                         info.target_valid = false;
                         if (recording_coordinator_->ArmFromRecovery(info)) {
                             emit shell_adapter_.navigateToPageRequested(ShellAdapter::RecordPage);
                         } else {
                             diagnostics::AppLog::warning(
                                 QStringLiteral("recovery"),
                                 QStringLiteral("Continue refused by the coordinator in its current state"));
                         }
                     });

    const int candidates = recovery_adapter_.scan();
    if (candidates <= 0)
        return;

    diagnostics::AppLog::info(
        QStringLiteral("recovery"),
        QStringLiteral("Found %1 interrupted recording(s) — showing recovery surface").arg(candidates));

    // Standing hub entry, so the surface is reachable again after "Decide later"
    // without restarting the application.
    notifications::NotificationEvent event;
    event.type = notifications::NotificationType::RecoveryAvailable;
    event.title = QStringLiteral("Recover last session?");
    event.body = (candidates == 1)
                     ? QStringLiteral("A recording from the last session wasn’t finalized.")
                     : QStringLiteral("%1 recordings from the last session weren’t finalized.").arg(candidates);
    event.action = notifications::NotificationAction::OpenRecovery;
    event.secondary_action = notifications::NotificationAction::Discard;
    notifications_adapter_.manager().Enqueue(std::move(event));

    surface_arbiter_.requestRecovery();
}

void QuickApplication::initializeRecordingError() {
    QObject::connect(
        &recording_error_adapter_, &RecordingErrorAdapter::sendReportRequested, &recording_error_adapter_, [this]() {
            const models::RecordingFailureReport& report = recording_error_adapter_.report();
            // Clicking Send IS the consent: granted here, attached
            // with allow-listed codec context, then forwarded as a
            // scrubbed non-fatal report. Paths inside `detail` are
            // stripped inside crash_capture, never here.
            crash_capture::GiveUserConsent();
            crash_capture::SetEncoderContext("nvenc", report.container.toStdString(), report.video_codec.toStdString(),
                                             report.audio_codec.toStdString());
            crash_capture::ReportNonFatalError(report.phase.toStdString(), report.detail.toStdString());
            diagnostics::AppLog::info(QStringLiteral("record.failure"),
                                      QStringLiteral("user sent error report phase=%1").arg(report.phase));
        });

    QObject::connect(&recording_error_adapter_, &RecordingErrorAdapter::openLogsRequested, &recording_error_adapter_,
                     [this]() { emit shell_adapter_.navigateToPageRequested(ShellAdapter::LogsPage); });
}

bool QuickApplication::applyOverlayVisualScenario(const QString& scenario) {
    pending_overlay_visual_state_ = scenario;
    if (scenario == QLatin1String("recovery") || scenario == QLatin1String("recovery-multi")) {
        QVector<RecoveryCandidate> candidates;
        RecoveryCandidate crashed;
        crashed.entry.id = QStringLiteral("visual-1");
        crashed.entry.artefact_path = QStringLiteral("C:/Recordings/Session 2026-08-10 21-14.mkv.tmp");
        crashed.entry.final_output_path = QStringLiteral("C:/Recordings/Session 2026-08-10 21-14.mkv");
        crashed.entry.intended_container = QStringLiteral("mkv");
        crashed.entry.started_at = QStringLiteral("2026-08-10T21:14:03Z");
        crashed.entry.finalized = false;
        crashed.artefact_size_bytes = 1387266048;
        candidates.append(crashed);

        if (scenario == QLatin1String("recovery-multi")) {
            // A finalized entry: a deliberate stop whose remux failed, so it
            // offers Finish and Delete but no Continue.
            RecoveryCandidate stopped;
            stopped.entry.id = QStringLiteral("visual-2");
            stopped.entry.artefact_path = QStringLiteral("C:/Recordings/Clip 2026-08-09 18-02.mkv");
            stopped.entry.final_output_path = QStringLiteral("C:/Recordings/Clip 2026-08-09 18-02.mp4");
            stopped.entry.intended_container = QStringLiteral("mp4");
            stopped.entry.started_at = QStringLiteral("2026-08-09T18:02:41Z");
            stopped.entry.finalized = true;
            stopped.artefact_size_bytes = 264241152;
            candidates.append(stopped);
        }

        recovery_adapter_.seedCandidatesForVisualHarness(std::move(candidates));
        recovery_adapter_.openSurface();
        return true;
    }

    if (scenario == QLatin1String("recording-error")) {
        UiRecordingResult result;
        result.succeeded = false;
        result.error_phase = L"Mux";
        result.hresult_text = L"0x80004005";
        result.error_detail = L"Container::Matroska requires VideoCodec::Av1, VideoCodec::H264, or VideoCodec::Hevc";
        result.output_file_bytes = 0;
        result.container = recorder_core::Container::Matroska;
        result.video_codec = recorder_core::VideoCodec::Av1;
        result.audio_codec = recorder_core::AudioCodec::Opus;
        const auto report = models::BuildRecordingFailureReport(result);
        if (!report)
            return false;
        presentRecordingFailure(*report, /*can_send_report=*/true);
        return true;
    }

    // The notification hub, seeded with one entry per severity and its hub
    // opened. Toast cards are otherwise only reachable through a real save, a
    // real disk-pressure event or a real failed hotkey registration, none of
    // which a visual pass can produce on demand — which is why the toast
    // treatment had never been photographed next to the reference at all.
    //
    // Real Enqueue calls on the real manager, so what gets rendered is what the
    // notification policy actually produces for these events, not a hand-built
    // set of cards.
    if (scenario == QLatin1String("notifications")) {
        struct Seed {
            notifications::NotificationType type;
            QString title;
            QString body;
            notifications::NotificationAction action;
            notifications::NotificationAction secondary;
        };
        const Seed seeds[] = {
            {notifications::NotificationType::Saved, QStringLiteral("Recording saved"),
             QStringLiteral("2026-08-10_22-31-22_Desktop_Display 1.mkv · 1.4 GB"),
             notifications::NotificationAction::Edit, notifications::NotificationAction::OpenFolder},
            {notifications::NotificationType::LowStorage, QStringLiteral("Low disk space"),
             QStringLiteral("Recording stopped — C: has less than 2 GB free."),
             notifications::NotificationAction::ChangeFolder, notifications::NotificationAction::None},
            {notifications::NotificationType::UnexpectedStop, QStringLiteral("Recording stopped unexpectedly"),
             QStringLiteral("The encoder reported an error. The partial file was kept."),
             notifications::NotificationAction::ShowFile, notifications::NotificationAction::None},
            {notifications::NotificationType::UpdateAvailable, QStringLiteral("Update available"),
             QStringLiteral("ExoSnap 0.9.1 is ready to install."), notifications::NotificationAction::OpenUpdate,
             notifications::NotificationAction::None},
        };
        for (const Seed& seed : seeds) {
            notifications::NotificationEvent event;
            event.type = seed.type;
            event.title = seed.title;
            event.body = seed.body;
            event.action = seed.action;
            event.secondary_action = seed.secondary;
            notifications_adapter_.manager().Enqueue(std::move(event));
        }
        // QCR-804's standing capture-stall caution, built by the very resolver the
        // live path calls — so what is photographed is the real wording, including
        // the conditional fullscreen sentence, and not a hand-written stand-in.
        notifications_adapter_.manager().Enqueue(notifications::MakeWindowCaptureStalledEvent(
            diagnostics::kStallStarveSeconds, /*exclusive_fullscreen_hint=*/true));
        notifications_adapter_.openHub();
        return true;
    }

    // The capture-excluded HUDs. Seeds the real INPUTS -- view-model state,
    // live stats, persisted settings -- and lets the ordinary resolution path
    // run, rather than forcing the adapter's outputs. A harness that wrote the
    // resolved state directly could photograph a combination the policy cannot
    // actually produce.
    if (scenario.startsWith(QLatin1String("hud-"))) {
        const QStringView variant = QStringView(scenario).mid(4);

        record_view_model_.elapsed_text = L"0:14:22";
        record_view_model_.output_size_text = L"812 MB";
        record_view_model_.live_stats_available = true;
        record_view_model_.elapsed_seconds = 862.0;
        record_view_model_.frames_captured = 51720;
        record_view_model_.av_drift_available = true;
        record_view_model_.av_drift_ms = -8.0;
        record_view_model_.dropped_frames = 0;

        if (variant == QLatin1String("paused")) {
            record_view_model_.SetState(UiRecordingState::Paused);
        } else if (variant == QLatin1String("warning")) {
            record_view_model_.dropped_frames = 3;
            record_view_model_.SetState(UiRecordingState::Recording);
        } else if (variant == QLatin1String("recording") || variant == QLatin1String("diagnostics") ||
                   variant == QLatin1String("diagnostics-technical")) {
            record_view_model_.SetState(UiRecordingState::Recording);
        } else {
            return false;
        }

        settings_.show_recording_overlay = true;
        settings_.show_diagnostics_overlay =
            variant == QLatin1String("diagnostics") || variant == QLatin1String("diagnostics-technical");
        if (variant == QLatin1String("diagnostics-technical")) {
            settings_.diagnostics_overlay_preset = models::TokenFor(models::DiagnosticsOverlayPreset::Technical);
        }
        // Deliberately NOT saveAndPublishAppSettings(): a harness run must not
        // write the developer's settings file. Published to the two adapters
        // that need it and no further.
        settings_adapter_.setAppSettings(settings_);
        overlay_adapter_.setAppSettings(settings_);

        // There is deliberately no "hud-error" variant. Nothing activates the
        // HUD window in the Error state, so a scenario for it would have to
        // force the window on by hand -- photographing a state the product
        // cannot reach and presenting it as evidence.
        record_view_model_adapter_.synchronize();
        overlay_adapter_.synchronize();
        return true;
    }

    // The crash consent surface. Every variant below is a real combination of the
    // three inputs present() takes -- whether a dump landed, whether a recording
    // was running, whether the crash folder can be opened -- plus the two pieces
    // of surface state the user can change (the remember tick, the disclosure).
    // Nothing here forces a rendering the adapter could not produce.
    if (scenario.startsWith(QLatin1String("crash-report"))) {
        const QStringView variant = QStringView(scenario).mid(12);
        const bool known = variant.isEmpty() || variant == QLatin1String("-recording") ||
                           variant == QLatin1String("-no-dump") || variant == QLatin1String("-no-folder") ||
                           variant == QLatin1String("-remember") || variant == QLatin1String("-expanded") ||
                           variant == QLatin1String("-long");
        if (!known)
            return false;

        CrashReportContext context;
        context.dump_available = variant != QLatin1String("-no-dump");
        context.recording_was_active = variant == QLatin1String("-recording");
        if (variant == QLatin1String("-long")) {
            // The longest values the sidecar can actually carry: the version is
            // whatever ExoSnapBuildInfo reports for a tagged pre-release build,
            // and the encoder line names every leg of the pipeline.
            context.version =
                QStringLiteral("0.9.0-rc5+build.20260812.eba270a \xc2\xb7 build eba270a301201882e9ca4d80");
            context.encoder =
                QStringLiteral("NVENC AV1 (10-bit, HDR10 passthrough) \xe2\x86\x92 Matroska + Opus 320 kbps");
        } else {
            context.version = QStringLiteral("0.9.0 \xc2\xb7 build a5d55f1");
            context.encoder = QStringLiteral("NVENC AV1 \xe2\x86\x92 MKV");
        }
        crash_report_adapter_.present(context, /*crash_folder_available=*/variant != QLatin1String("-no-folder"));

        // Draft tick: the same value the checkbox writes, set before the capture
        // rather than by a synthesised click.
        if (variant == QLatin1String("-remember"))
            crash_report_adapter_.setRememberChoice(true);
        // The Loader above the overlay is synchronous, so the item exists as soon
        // as present() has flipped `active`.
        if (variant == QLatin1String("-expanded") && root_window_) {
            if (QObject* disclosure = root_window_->findChild<QObject*>(QStringLiteral("crashIncludedDisclosure")))
                disclosure->setProperty("expanded", true);
        }
        return true;
    }

    return false;
}

bool QuickApplication::applyCrashConsentAction(CrashConsentAction action) {
#if defined(Q_OS_WIN)
    switch (action) {
    case CrashConsentAction::None:
        return true;
    case CrashConsentAction::SendPendingOnce:
        return crash_capture::SendPendingReportOnce();
    case CrashConsentAction::GrantPersistent:
        crash_capture::GiveUserConsent();
        return true;
    case CrashConsentAction::ResetToAsk:
        crash_capture::ResetUserConsent();
        return true;
    case CrashConsentAction::Revoke:
        crash_capture::RevokeUserConsent();
        return true;
    }
    return false;
#else
    Q_UNUSED(action);
    return true;
#endif
}

void QuickApplication::initializeCrashReport() {
    QObject::connect(&crash_report_adapter_, &CrashReportAdapter::openCrashFolderRequested, &crash_report_adapter_,
                     [this]() { QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(crash_dir_))); });

    // One committing edge for both buttons: the decision already says what to
    // persist and which consent action to apply, so nothing here re-derives it.
    QObject::connect(
        &crash_report_adapter_, &CrashReportAdapter::decisionMade, &crash_report_adapter_,
        [this](const CrashReportDecision& decision, bool send) {
            if (decision.persisted_policy.has_value()) {
                settings_.crash_report_policy = *decision.persisted_policy;
                saveAndPublishAppSettings(SettingsWriteIntent::UserEdit);
            }
            const bool delivered = applyCrashConsentAction(decision.consent_action);
            if (decision.consent_action == CrashConsentAction::None)
                return;
            if (send) {
                diagnostics::AppLog::info(
                    QStringLiteral("crash"),
                    delivered ? QStringLiteral("User granted crash-report consent; pending report released")
                              : QStringLiteral("User granted one-shot crash-report consent, but the transport "
                                               "did not flush"));
            } else {
                diagnostics::AppLog::info(QStringLiteral("crash"),
                                          decision.persisted_policy.has_value()
                                              ? QStringLiteral("User declined and disabled future crash-report prompts")
                                              : QStringLiteral("User declined this crash report"));
            }
        });

    if (!pending_crash_)
        return;

    // A visual scenario photographs a named surface. The crash prompt is raised
    // by whatever the PREVIOUS session did on this machine, so left enabled it
    // covers every capture taken after an unclean exit — which is exactly what
    // happens while a developer is killing the application to test something
    // else. The prompt has its own deterministic scenario
    // (--overlay-visual-state), which is how it should be photographed.
    //
    // Keyed off argv rather than the pending scenario fields: this runs during
    // load(), and the scenarios are seeded after it.
    if (QCoreApplication::arguments().contains(QStringLiteral("--visual-test"))) {
        diagnostics::AppLog::info(QStringLiteral("crash"),
                                  QStringLiteral("Visual scenario active — crash dialog suppressed"));
        return;
    }

    // The persisted policy decides whether there is anything to ask at all.
    // Auto-send has already been granted by applyCrashReportPolicy(); never-send
    // has already been revoked there. Prompting in either case would ask a
    // question the user has answered once and for all.
    const CrashPromptDisposition disposition = ResolveCrashPromptDisposition(settings_.crash_report_policy);
    if (disposition == CrashPromptDisposition::SuppressAndSend) {
        diagnostics::AppLog::info(
            QStringLiteral("crash"),
            QStringLiteral("Auto-send enabled — consent granted silently; crash dialog suppressed"));
        return;
    }
    if (disposition == CrashPromptDisposition::SuppressWithoutSend) {
        diagnostics::AppLog::info(QStringLiteral("crash"),
                                  QStringLiteral("Crash-report policy is Never send; consent prompt suppressed"));
        return;
    }

    // Raised now, or queued behind an open recovery surface. Both directions of
    // that arbitration live in BlockingSurfaceArbiter: this one used to be a
    // single-shot connection here while the reverse — the standing recovery
    // notification's action, which stays clickable on the desktop toast while
    // the crash prompt is up — had no check at all and produced two active
    // modal loaders.
    surface_arbiter_.requestCrash();
}

void QuickApplication::showCrashReportSurface() {
    if (!pending_crash_)
        return;

    CrashReportContext context;
    // A crash mid-recording is exactly what leaves a recovery candidate behind,
    // so the recovery scan that already ran is the evidence for this banner.
    context.recording_was_active = recovery_adapter_.hasCandidates();
    // The PREVIOUS session's own facts, from the sidecar. Substituting this
    // run's build or a current-machine probe would present a guess as evidence.
    context.version = QString::fromStdString(pending_crash_->app_version);

    const QString backend = QString::fromStdString(pending_crash_->encoder_backend).toUpper();
    const QString video_codec = QString::fromStdString(pending_crash_->video_codec);
    const QString container = QString::fromStdString(pending_crash_->container);
    QStringList encoder_parts;
    if (!backend.isEmpty())
        encoder_parts << backend;
    if (!video_codec.isEmpty())
        encoder_parts << video_codec;
    QString encoder = encoder_parts.join(QStringLiteral(" "));
    if (!container.isEmpty())
        encoder += QStringLiteral(" \xe2\x86\x92 ") + container;
    context.encoder = encoder.trimmed();

    const QDir crash_dir(QString::fromStdString(crash_dir_));
    const bool folder_available = !crash_dir_.empty() && crash_dir.exists();
    if (folder_available) {
        context.dump_available = !crash_dir.entryInfoList({QStringLiteral("*.dmp")}, QDir::Files, QDir::Time).isEmpty();
    }

    crash_report_adapter_.present(context, folder_available);
}

void QuickApplication::dispatchNotificationAction(notifications::NotificationAction action, const QString& payload) {
    using notifications::NotificationAction;
    const QString path = payload.trimmed();
    switch (action) {
    case NotificationAction::Edit:
        // The toast carries only the output path. Recovering the full metadata
        // matters: a bare-path context leaves every detail row and the timeline
        // showing placeholders, which is what the Widgets shell used to do.
        if (record_view_model_.last_succeeded && record_view_model_.current_completed_recording.file_path == path) {
            openEditorForCurrentRecording();
        } else if (!path.isEmpty()) {
            edit_session_adapter_.setEditContext(MakeMinimalEditContext(path));
        }
        break;
    case NotificationAction::OpenFolder: {
        if (path.isEmpty())
            return;
        const QFileInfo info(path);
        const QString folder = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
        if (!folder.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
        break;
    }
    case NotificationAction::ShowFile: {
        if (path.isEmpty())
            return;
        // Reveal the partial file when it still exists, else its folder.
        const QFileInfo info(path);
        QDesktopServices::openUrl(QUrl::fromLocalFile(info.exists() ? path : info.absolutePath()));
        break;
    }
    case NotificationAction::OpenUpdate:
    case NotificationAction::ChangeFolder:
    case NotificationAction::OpenHotkeys:
        // All three land on Settings: updates, output folder and hotkeys are
        // embedded sections there, not pages of their own.
        emit shell_adapter_.navigateToPageRequested(ShellAdapter::SettingsPage);
        break;
    case NotificationAction::OpenDiagnostics:
        emit shell_adapter_.navigateToPageRequested(ShellAdapter::DiagnosticsPage);
        break;
    case NotificationAction::OpenRecovery:
        // Raises the surface again after "Decide later". A no-op once every
        // candidate has been resolved — the adapter refuses to open on an empty
        // set rather than showing an empty card. Routed through the arbiter
        // because this action is reachable while the crash prompt is up: the
        // desktop toast is its own always-on-top window and takes clicks that
        // the modal scrim inside the shell never sees.
        surface_arbiter_.requestRecovery();
        break;
    case NotificationAction::None:
    case NotificationAction::Discard:
    case NotificationAction::RelaunchElevated:
    case NotificationAction::UndoPresetSwitch:
        // Discard needs no work beyond the dismissal the manager already did.
        // Elevation and preset-undo depend on subsystems the Quick frontend does
        // not own yet. Left unhandled deliberately rather than half-wired: a
        // button that silently does the wrong thing is worse than one whose
        // backing subsystem is still missing.
        break;
    }
}

void QuickApplication::publishRecordingResultNotification(const UiRecordingResult& result) {
    notifications::NotificationEvent event;
    if (result.succeeded) {
        // "Open editor when finished" makes the editor opening itself the
        // post-recording feedback; a toast whose Edit action leads to the very
        // surface already on screen would be a redundant second path there.
        if (settings_.open_editor_when_finished)
            return;
        event.type = notifications::NotificationType::Saved;
        event.title = QStringLiteral("Recording saved");
        // The name, not the path. A full path is a single unbreakable token --
        // no spaces to wrap at -- so it overran the toast instead of eliding,
        // and it told the user the one thing they already know (where their
        // recordings go) at the cost of the one thing they want to read. The
        // path itself is not lost: Show in folder acts on it, and Edit carries
        // it as its payload.
        event.body = QFileInfo(QString::fromStdWString(result.output_path)).fileName();
        event.action = notifications::NotificationAction::Edit;
        event.action_payload = QString::fromStdWString(result.output_path);
        event.secondary_action = notifications::NotificationAction::OpenFolder;
    } else if (models::IsDiskSpaceAutoStop(result)) {
        // The one low-storage producer there is. The threshold, the monitoring
        // and the decision to stop all belong to the coordinator's disk guard
        // (diagnostics::ComputeHardStopThreshold); this only reports the stop it
        // already performed. Deliberately no second poller and no second
        // threshold — an independent one would eventually disagree with the
        // guard that actually acts.
        event.type = notifications::NotificationType::LowStorage;
        event.title = QStringLiteral("Storage running low");
        event.body = QStringLiteral("Recording stopped — output drive is critically low on disk space.");
        // Primary action lands on Settings → Output. Dismiss is the hub's own
        // affordance, so no secondary action is set.
        event.action = notifications::NotificationAction::ChangeFolder;
    } else {
        // Every other failure is carried by the modal recording-error surface,
        // which has the full detail and the opt-in report. A toast alongside it
        // would say less about the same event.
        return;
    }
    notifications_adapter_.manager().Enqueue(std::move(event));
}

void QuickApplication::initializeShell() {
    shell_adapter_.setStateProvider([this]() { return sampleCloseGuardState(); });
    QObject::connect(&shell_adapter_, &ShellAdapter::cancelRemuxRequested, &shell_adapter_,
                     [this]() { recording_coordinator_->CancelRemux(); });
    QObject::connect(&shell_adapter_, &ShellAdapter::cancelExportRequested, &shell_adapter_,
                     [this]() { edit_export_adapter_.cancel(); });
    QObject::connect(&shell_adapter_, &ShellAdapter::stopRecordingRequested, &shell_adapter_,
                     [this]() { recording_coordinator_->StopRecording(); });
    // The close is approved only after every guard cleared, so this is the last
    // moment anything can still be written to disk.
    QObject::connect(&shell_adapter_, &ShellAdapter::closeApproved, &shell_adapter_,
                     [this]() { flushPendingPersists(); });
    // A guard prompt is modal and lives inside the root window, so raising one
    // while that window is hidden asks a question nobody can see — and the app
    // then sits there waiting for an answer. Reached from the tray "Quit" during
    // a recording, but stated against the guard rather than against the tray:
    // any future path that closes a hidden window inherits the same rule.
    // Deliberately not in the tray handler, which cannot know in advance whether
    // requestClose() will prompt or close outright — restoring there would flash
    // the window on screen for the ordinary idle quit.
    QObject::connect(&shell_adapter_, &ShellAdapter::closeGuardChanged, &shell_adapter_, [this]() {
        if (shell_adapter_.closeGuardActive() && root_window_ != nullptr && !root_window_->isVisible())
            restoreWindowFromTray();
    });
}

void QuickApplication::initializeTray() {
    // No tray on this platform/session means no way back to a hidden window, so
    // there is deliberately no icon AND no close-to-tray: ShouldHideToTray reads
    // tray availability as its third input for exactly this reason.
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    tray_presence_ = std::make_unique<ui::tray::TrayPresence>();

    QObject::connect(tray_presence_.get(), &ui::tray::TrayPresence::activateWindowRequested, &shell_adapter_,
                     [this]() { restoreWindowFromTray(); });
    // Same entry point the global hotkey uses, so the tray cannot develop its own
    // idea of what "toggle recording" means.
    QObject::connect(tray_presence_.get(), &ui::tray::TrayPresence::recordToggleRequested, &shell_adapter_,
                     [this]() { triggerHotkeyAction(HotkeyAction::ToggleRecording); });
    // Routed through the window rather than QCoreApplication::quit() so the
    // attempt still passes onClosing -> requestClose() and therefore the guards:
    // "Quit" from the tray must still ask about a running recording.
    QObject::connect(tray_presence_.get(), &ui::tray::TrayPresence::quitRequested, &shell_adapter_, [this]() {
        force_quit_ = true;
        if (root_window_)
            root_window_->close();
    });

    shell_adapter_.setHideToTrayProvider([this]() {
        const bool hide =
            ui::tray::ShouldHideToTray(settings_.keep_running_in_tray, force_quit_, tray_presence_ != nullptr);
        // Consumed on the first close attempt after the tray "Quit", matching the
        // Widgets shell: leaving it latched would turn every later close into a
        // hard quit even after the user had cancelled at a guard.
        force_quit_ = false;
        return hide;
    });

    QObject::connect(&shell_adapter_, &ShellAdapter::hideToTrayRequested, &shell_adapter_, [this]() {
        if (!root_window_)
            return;
        // Geometry is read from a visible window only, so it has to be banked
        // before the hide — otherwise a session that ends from the tray persists
        // whatever the last debounced sample happened to be.
        if (window_geometry_)
            window_geometry_->flush();
        root_window_->hide();
        if (tray_presence_)
            tray_presence_->setWindowVisible(false);

        // One-time notice, so the first hide can never look like a crash. The
        // flag is persisted immediately: a user who then kills the process must
        // not be told again on the next run.
        if (!settings_.tray_close_notice_shown) {
            settings_.tray_close_notice_shown = true;
            saveAndPublishAppSettings();
            if (tray_presence_) {
                tray_presence_->showMessage(
                    QStringLiteral("ExoSnap is still running"),
                    QStringLiteral("ExoSnap is running in the tray. Right-click the tray icon to quit."),
                    QSystemTrayIcon::Information, 4000);
            }
        }
        diagnostics::AppLog::info(QStringLiteral("tray"), QStringLiteral("Window hidden to tray"));
    });

    // The unread badge mirrors the in-window bell: a toast raised while the
    // window is hidden is otherwise invisible.
    QObject::connect(&notifications_adapter_.manager(), &notifications::NotificationManager::eventRecorded,
                     tray_presence_.get(), [this](const notifications::NotificationEvent&) {
                         if (tray_presence_ && root_window_ && !root_window_->isVisible())
                             tray_presence_->incrementUnreadCount();
                     });

    if (root_window_) {
        tray_presence_->setWindowVisible(root_window_->isVisible());
        QObject::connect(root_window_, &QWindow::activeChanged, tray_presence_.get(), [this]() {
            if (tray_presence_ && root_window_ && root_window_->isActive())
                tray_presence_->clearUnreadCount();
        });
    }
    tray_presence_->show();
    refreshTrayState();
}

void QuickApplication::refreshTrayState() {
    if (!tray_presence_)
        return;
    // Derived from the view model's own booleans rather than by re-parsing the
    // status string: the label is presentation and may be localized, the state is
    // not. Countdown and Preparing read as Recording, matching the Widgets
    // mapping — the capture is committed from the user's point of view.
    const UiRecordingState state = record_view_model_.state;
    ui::tray::TrayIconState tray_state = ui::tray::TrayIconState::Idle;
    if (state == UiRecordingState::Paused) {
        tray_state = ui::tray::TrayIconState::Paused;
    } else if (state == UiRecordingState::Recording || state == UiRecordingState::Countdown ||
               state == UiRecordingState::Preparing) {
        tray_state = ui::tray::TrayIconState::Recording;
    }
    tray_presence_->applyState(tray_state, record_view_model_adapter_.stateText().toUpper(),
                               record_view_model_adapter_.elapsedText());
    tray_presence_->setRecordingBlocked(record_view_model_adapter_.blocked() &&
                                        tray_state == ui::tray::TrayIconState::Idle);
}

void QuickApplication::restoreWindowFromTray() {
    if (!root_window_)
        return;
    // showNormal() rather than show(): a window hidden while minimized comes back
    // minimized otherwise, which reads to the user as the tray click doing
    // nothing at all.
    if (root_window_->visibility() == QWindow::Minimized || !root_window_->isVisible())
        root_window_->showNormal();
    root_window_->raise();
    root_window_->requestActivate();
    if (tray_presence_) {
        tray_presence_->setWindowVisible(true);
        tray_presence_->clearUnreadCount();
    }
}

CloseGuardState QuickApplication::sampleCloseGuardState() const {
    CloseGuardState state;
    const UiRecordingState recording_state = record_view_model_.state;
    state.finalizing = recording_state == UiRecordingState::Stopping;
    state.remuxing = recording_state == UiRecordingState::Saving;
    state.exporting = edit_export_adapter_.running();
    // Countdown counts as recording: the capture is already committed from the
    // user's point of view, and closing mid-countdown would strand the armed
    // pipeline.
    state.recording = recording_state == UiRecordingState::Recording || recording_state == UiRecordingState::Paused ||
                      recording_state == UiRecordingState::Countdown;
    return state;
}

void QuickApplication::flushPendingPersists() {
    if (webcam_overlay_persist_timer_.isActive()) {
        webcam_overlay_persist_timer_.stop();
        persistLiveConfig();
    }
    // The window is still alive at this point (the close was only approved, not
    // executed), so this is the last moment its geometry can be read.
    if (window_geometry_)
        window_geometry_->flush();
}

// The single production entry point from Record into the Edit surface. Both
// triggers -- the user pressing Edit on the result row, and the automatic open
// when "Open editor when finished" is on -- route through here, so the gates
// below are stated once instead of at each call site.
//
// Handing the session adapter a context IS the request to show the overlay
// (AppShell binds editOverlayOpen to it), so every reason NOT to show it has to
// be decided before setEditContext, not after.
void QuickApplication::openEditorForCurrentRecording() {
    if (!canOpenEditorForCurrentRecording())
        return;
    edit_session_adapter_.setEditContext(
        MakeEditContextForCurrentSession(record_view_model_.current_completed_recording,
                                         QString::fromStdWString(record_view_model_.result_mkv_master_path),
                                         peak_av_drift_ms_, av_drift_ever_available_, last_completed_snapshot_));
}

bool QuickApplication::canOpenEditorForCurrentRecording() const {
    if (!record_view_model_.last_succeeded)
        return false;
    // A live capture owns the Record surface; opening the editor over a running
    // recording or a countdown makes no sense, and the automatic open would
    // otherwise fire on a segment boundary of a still-running split session.
    if (!AllowsEditorEntry(record_view_model_.state))
        return false;
    // A running export must never be clobbered by a new clip: the panel state,
    // the trim range and the remux thread all belong to the clip currently open.
    if (edit_export_adapter_.running())
        return false;
    // Split recordings have no single MKV edit master (CanOpenInEditor), and a
    // file that no longer exists would open an empty player.
    return CanOpenInEditor(record_view_model_.current_completed_recording);
}

bool QuickApplication::load(bool no_activate) {
    if (!webcam_provider_registered_) {
        engine_.addImageProvider(QStringLiteral("record-webcam"), webcam_frame_provider_);
        webcam_provider_registered_ = true;
    }
    if (!edit_tile_provider_registered_) {
        engine_.addImageProvider(QLatin1String(kEditTileProviderId), edit_tile_provider_);
        edit_tile_provider_registered_ = true;
    }
    // Resolved before the engine loads so the window is created at its final
    // placement rather than jumping there after the first frame.
    const QSize minimum_size(ui::theme::ExoSnapMetrics::kMinWindowWidth, ui::theme::ExoSnapMetrics::kMinWindowHeight);
    const ResolvedWindowGeometry restored = ResolveWindowGeometry(settings_.window_geometry, minimum_size,
                                                                  QSize(kDefaultWindowWidth, kDefaultWindowHeight));
    if (WindowGeometryTraceEnabled()) {
        const PersistedWindowGeometry& saved = settings_.window_geometry;
        qInfo("window-trace: persisted %d,%d %dx%d maximized=%d", saved.x, saved.y, saved.width, saved.height,
              saved.maximized ? 1 : 0);
        qInfo("window-trace: resolved %d,%d %dx%d maximized=%d", restored.rect.x(), restored.rect.y(),
              restored.rect.width(), restored.rect.height(), restored.maximized ? 1 : 0);
    }
    engine_.setInitialProperties({
        {QStringLiteral("initialGeometry"), QVariant::fromValue(QRectF(restored.rect))},
        {QStringLiteral("minimumWindowSize"), QVariant::fromValue(QSizeF(minimum_size))},
        {QStringLiteral("aboutViewModel"), QVariant::fromValue(&about_view_model_)},
        {QStringLiteral("recordViewModel"), QVariant::fromValue(&record_view_model_adapter_)},
        {QStringLiteral("previewAdapter"), QVariant::fromValue(&record_preview_adapter_)},
        {QStringLiteral("settingsAdapter"), QVariant::fromValue(&settings_adapter_)},
        {QStringLiteral("deviceAdapter"), QVariant::fromValue(&device_adapter_)},
        {QStringLiteral("diagnosticsAdapter"), QVariant::fromValue(&diagnostics_adapter_)},
        {QStringLiteral("logsAdapter"), QVariant::fromValue(&logs_adapter_)},
        {QStringLiteral("editSession"), QVariant::fromValue(&edit_session_adapter_)},
        {QStringLiteral("editTimeline"), QVariant::fromValue(&edit_timeline_adapter_)},
        {QStringLiteral("editPlayer"), QVariant::fromValue(&edit_player_adapter_)},
        {QStringLiteral("editExport"), QVariant::fromValue(&edit_export_adapter_)},
        {QStringLiteral("shell"), QVariant::fromValue(&shell_adapter_)},
        {QStringLiteral("notifications"), QVariant::fromValue(&notifications_adapter_)},
        {QStringLiteral("recovery"), QVariant::fromValue(&recovery_adapter_)},
        {QStringLiteral("recordingError"), QVariant::fromValue(&recording_error_adapter_)},
        {QStringLiteral("crashReport"), QVariant::fromValue(&crash_report_adapter_)},
        {QStringLiteral("overlays"), QVariant::fromValue(&overlay_adapter_)},
        {QStringLiteral("noActivate"), no_activate},
        // ADR 0033, and deliberately an initial property rather than a
        // navigation emitted once the engine has loaded. By that point a
        // recovery surface or a crash prompt raised during startup is already
        // up, and the single navigation edge (QCR-001) refuses a navigation
        // behind a blocking surface -- which would silently drop a restore that
        // is not a navigation at all.
        {QStringLiteral("landingPage"),
         QVariant::fromValue(static_cast<int>(pending_landing_page_.value_or(ShellAdapter::RecordPage)))},
    });
    applyThemeFromSettings();
    applyDiagnosticsVisualScenarios();
    applyEditVisualScenario();
    engine_.loadFromModule("ExoSnap.Quick", "Main");
    if (engine_.rootObjects().isEmpty()) {
        return false;
    }

    // Consumed by the initial property above; cleared so a second load() in the
    // same process cannot re-apply it.
    pending_landing_page_.reset();

    if (auto* root_window = qobject_cast<QQuickWindow*>(engine_.rootObjects().constFirst())) {
        root_window_ = root_window;
        InstallWindowGeometryTrace(root_window);
        TraceWindowGeometry("after-qml-load", root_window);

        // ── First show ──────────────────────────────────────────────────────
        //
        // Main.qml sets `visible: false` and this is the only place that undoes
        // it. The single owner is the fix, not a tidying-up: shown from QML, the
        // window appears part-way through engine load, before Qt has applied
        // Qt::FramelessWindowHint -- and while the HWND still carries the framed
        // style Qt creates it with, Qt offsets every geometry it is handed by
        // that frame. Measured, the persisted 400,120 1280x820 became a visible
        // 392,89 1296x820 and was put right one frame later, which is the jump.
        //
        // The window is therefore still hidden here, and the three steps below
        // run in the order that leaves nothing to correct afterwards:
        //
        //   1. the final native style, once Qt has stopped rewriting it,
        //   2. the final geometry, now that Qt's frame margins are zero,
        //   3. visible.
        //
        // Activation is not requested anywhere in this sequence. A window that
        // is shown takes focus because Windows gives it focus, and a --no-activate
        // start withholds it through Qt::WindowDoesNotAcceptFocus in the flags,
        // which is already set by the time we get here.
        if (auto* chrome = root_window->findChild<QuickWindowChrome*>())
            chrome->applyNativeWindowStyle();
        ApplyStartupWindowGeometry(root_window, restored.rect);
        TraceWindowGeometry("pre-show", root_window);
        // showMaximized() rather than an initial `visibility`, and after the rect
        // above: a maximized window still needs a restore rect, and the rect it
        // un-maximizes to is whatever it stood on when it was maximized.
        if (restored.maximized)
            root_window->showMaximized();
        else
            root_window->show();
        TraceWindowGeometry("post-show", root_window);

        // Seeded with the rect the window is ACTUALLY on -- the resolved one --
        // rather than the raw persisted value. The two differ whenever the clamp
        // moved the window (a disconnected monitor, a shrunken work area), and
        // seeding the unclamped value made everything downstream reason about a
        // rect that was never on screen.
        PersistedWindowGeometry placed = settings_.window_geometry;
        placed.x = restored.rect.x();
        placed.y = restored.rect.y();
        placed.width = restored.rect.width();
        placed.height = restored.rect.height();
        placed.maximized = restored.maximized;
        window_geometry_ =
            std::make_unique<QuickWindowGeometry>(root_window, placed, [this](const PersistedWindowGeometry& geometry) {
                if (settings_.window_geometry.x == geometry.x && settings_.window_geometry.y == geometry.y &&
                    settings_.window_geometry.width == geometry.width &&
                    settings_.window_geometry.height == geometry.height &&
                    settings_.window_geometry.maximized == geometry.maximized) {
                    return;
                }
                settings_.window_geometry = geometry;
                // Publishes to the Settings mirror as well: the adapter holds a
                // whole-struct copy, so a geometry write that only touched
                // settings_ would be undone by the next Settings toggle.
                saveAndPublishAppSettings();
            });
    }

    // Toasts anchor to the screen hosting the app window, not the primary one,
    // and follow it when the user drags the window to another display. Resolved
    // here rather than in QML: only QScreen exposes the available area's ORIGIN,
    // without which a top- or left-docked taskbar would push the stack off the
    // work area.
    if (root_window_) {
        const auto publish_anchor = [this]() {
            if (!root_window_)
                return;
            if (const QScreen* screen = root_window_->screen())
                notifications_adapter_.setToastAnchorGeometry(screen->availableGeometry());
        };
        publish_anchor();
        QObject::connect(root_window_, &QWindow::screenChanged, &notifications_adapter_,
                         [publish_anchor](QScreen*) { publish_anchor(); });
    }

    initializeDisplayGeometryWatch();

    // After the window exists — the tray reports its visibility and acts on it.
    // Suppression is decided by the entry point, not by no_activate: --smoke-test
    // deliberately keeps the tray so the QApplication + QSystemTrayIcon startup
    // and teardown path is exercised by an automated test rather than only by
    // hand. See applyTraySuppression().
    if (!tray_suppressed_)
        initializeTray();

#if defined(Q_OS_WIN)
    // Registration needs a live HWND, which only exists once the window has been
    // created -- same ordering constraint the Widgets frontend has in showEvent.
    if (auto* window = qobject_cast<QWindow*>(engine_.rootObjects().constFirst())) {
        const auto hwnd = reinterpret_cast<HWND>(window->winId());
        hotkey_registrar_ = std::make_unique<Win32HotkeyRegistrar>(hwnd);
        hotkey_event_filter_ = std::make_unique<QuickHotkeyEventFilter>(
            hwnd, [this](HotkeyAction action) { triggerHotkeyAction(action); });
        QCoreApplication::instance()->installNativeEventFilter(hotkey_event_filter_.get());
        // Same ordering constraint, same window: the updater finds this HWND by
        // owner pid and title, so the filter has to be armed as soon as the
        // window exists.
        updater_handoff_filter_ =
            std::make_unique<QuickUpdaterHandoffFilter>(hwnd, [this] { closeForUpdaterHandoff(); });
        QCoreApplication::instance()->installNativeEventFilter(updater_handoff_filter_.get());
        // The return value is the set of bindings Windows refused, usually
        // because another application already owns the combo. Discarding it left
        // the user with a shortcut that silently did nothing and no way to find
        // out why. Policy (which failures are noise, which are worth a
        // notification, and that a dead binding is dropped rather than kept)
        // lives in models/HotkeyStartupConflicts, shared with the Widgets shell.
        const std::vector<HotkeyAction> failed = hotkey_service_.SetRegistrar(hotkey_registrar_.get());
        if (!failed.empty()) {
            const models::HotkeyStartupConflicts conflicts =
                models::ClassifyHotkeyStartupConflicts(failed, hotkey_service_);
            QStringList all_names;
            for (const HotkeyAction action : failed)
                all_names << GlobalHotkeyService::ActionDisplayName(action);
            diagnostics::AppLog::warning(
                QStringLiteral("hotkeys"),
                QStringLiteral("Hotkey registration failed at startup (in use elsewhere): %1 (default: %2, "
                               "user-set: %3)")
                    .arg(all_names.join(QStringLiteral(", ")))
                    .arg(conflicts.default_failed.size())
                    .arg(conflicts.custom_failed.size()));

            for (const HotkeyAction action : failed)
                hotkey_service_.UnsetBinding(action);
            hotkey_service_.SaveToStrings(settings_.hotkey_bindings);
            saveAndPublishAppSettings();

            if (!conflicts.custom_failed.empty()) {
                QStringList custom_names;
                for (const HotkeyAction action : conflicts.custom_failed)
                    custom_names << GlobalHotkeyService::ActionDisplayName(action);
                notifications::NotificationEvent event;
                event.type = notifications::NotificationType::HotkeyConflict;
                event.title = QStringLiteral("Hotkey unavailable");
                event.body = models::HotkeyConflictNotificationBody(custom_names);
                event.action = notifications::NotificationAction::OpenHotkeys;
                notifications_adapter_.manager().Enqueue(std::move(event));
            }
        }
        refreshHotkeyRows();
    }
#endif
    return true;
}

AboutViewModelAdapter* QuickApplication::aboutViewModel() noexcept {
    return &about_view_model_;
}
RecordPreviewAdapter* QuickApplication::recordPreviewAdapter() noexcept {
    return &record_preview_adapter_;
}
const RecordViewModel& QuickApplication::recordViewModel() const noexcept {
    return record_view_model_;
}
RecordViewModelAdapter* QuickApplication::recordViewModelAdapter() noexcept {
    return &record_view_model_adapter_;
}
SettingsAdapter* QuickApplication::settingsAdapter() noexcept {
    return &settings_adapter_;
}
DeviceAdapter* QuickApplication::deviceAdapter() noexcept {
    return &device_adapter_;
}
DiagnosticsAdapter* QuickApplication::diagnosticsAdapter() noexcept {
    return &diagnostics_adapter_;
}
LogsAdapter* QuickApplication::logsAdapter() noexcept {
    return &logs_adapter_;
}
EditSessionAdapter* QuickApplication::editSessionAdapter() noexcept {
    return &edit_session_adapter_;
}
EditTimelineAdapter* QuickApplication::editTimelineAdapter() noexcept {
    return &edit_timeline_adapter_;
}
EditPlayerAdapter* QuickApplication::editPlayerAdapter() noexcept {
    return &edit_player_adapter_;
}
EditExportAdapter* QuickApplication::editExportAdapter() noexcept {
    return &edit_export_adapter_;
}
ShellAdapter* QuickApplication::shellAdapter() noexcept {
    return &shell_adapter_;
}
NotificationsAdapter* QuickApplication::notificationsAdapter() noexcept {
    return &notifications_adapter_;
}
RecoveryAdapter* QuickApplication::recoveryAdapter() noexcept {
    return &recovery_adapter_;
}
RecordingErrorAdapter* QuickApplication::recordingErrorAdapter() noexcept {
    return &recording_error_adapter_;
}
CrashReportAdapter* QuickApplication::crashReportAdapter() noexcept {
    return &crash_report_adapter_;
}
QQmlApplicationEngine& QuickApplication::engine() noexcept {
    return engine_;
}
RecordingCoordinator* QuickApplication::recordingCoordinator() noexcept {
    return recording_coordinator_.get();
}

bool QuickApplication::prepareRecordingBenchmark(uint32_t frame_rate, QString& error) {
    if (recording_coordinator_ == nullptr) {
        error = QStringLiteral("The Quick composition owner has no recording coordinator.");
        return false;
    }
    OutputSettingsModel output = OutputSettingsModel::Defaults();
    VideoSettingsModel video = VideoSettingsModel::Defaults();
    video.frame_rate_num = std::clamp(frame_rate, 1U, 240U);
    video.frame_rate_den = 1;
    recording_coordinator_->SetOutputSettings(output);
    recording_coordinator_->SetVideoSettings(video);
    recording_coordinator_->OnCapabilitiesReady(capability::CapabilityBuilder::BuildFromHardwareQuery());
    if (recording_coordinator_->State() != UiRecordingState::Ready) {
        error = QString::fromStdWString(recording_coordinator_->CapabilityStatusText());
        return false;
    }
    return true;
}

bool QuickApplication::selectCaptureTargetForAutomation(recorder_core::CaptureTarget::Kind kind,
                                                        const QString& title_filter) {
    for (std::size_t index = 0; index < record_view_model_.targets.size(); ++index) {
        const recorder_core::CaptureTarget& target = record_view_model_.targets[index];
        if (target.kind != kind)
            continue;
        if (kind == recorder_core::CaptureTarget::Kind::Window &&
            !QString::fromStdString(target.description).contains(title_filter, Qt::CaseInsensitive))
            continue;
        const CaptureMode mode =
            kind == recorder_core::CaptureTarget::Kind::Window ? CaptureMode::Window : CaptureMode::Monitor;
        selectTarget(static_cast<int>(index), mode);
        // selectTarget can decline (a running recording locks the source), so the
        // caller is told what actually happened rather than that a call was made.
        return record_view_model_.selected_target_index == static_cast<int>(index);
    }
    return false;
}

void QuickApplication::applyHarnessWindowSize(const QSize& size) {
    if (!size.isValid() || size.isEmpty())
        return;
    if (window_geometry_)
        window_geometry_->detach();
    if (root_window_)
        root_window_->resize(size);
}

bool QuickApplication::openEditorForAutomation() {
    if (!canOpenEditorForCurrentRecording())
        return false;
    openEditorForCurrentRecording();
    return edit_session_adapter_.open();
}

bool QuickApplication::canOpenEditor() const {
    return canOpenEditorForCurrentRecording();
}

// One name per product state, and no aliases at all. There used to be four —
// `warning` for Blocked, `error` for Failed, `idle` for Ready, `saved` for
// Completed — and the visual sweep captured every one of them, so the evidence
// directory carried record__warning.png and record__blocked.png as two files
// claiming to test a difference that does not exist: the same SetState call
// produced both. A scenario name that contradicts, or merely duplicates, the
// state it selects is worse than no scenario.
bool QuickApplication::applyRecordVisualScenario(const QString& scenario) {
    const QString normalized = scenario.trimmed().toLower();
    pending_record_visual_state_ = normalized;
    microphone_available_ = true;
    webcam_available_ = true;
    webcam_error_.clear();
    record_view_model_.ResetStats();
    // Re-seeding is idempotent: a scenario that raises no notice must not
    // inherit one from the scenario applied before it.
    record_view_model_adapter_.setNoticeText({});
    clearAudioSourceDegradedWarning();

    if (normalized == QLatin1String(visual::record_state::kReady)) {
        record_view_model_.SetState(UiRecordingState::Ready);
    } else if (normalized == QLatin1String(visual::record_state::kRecording) ||
               normalized == QLatin1String(visual::record_state::kRecordingAudioDegraded)) {
        record_view_model_.SetState(UiRecordingState::Recording);
        record_view_model_.live_stats_available = true;
        record_view_model_.elapsed_text = L"12:34";
        record_view_model_.elapsed_seconds = 754.0;
        record_view_model_.video_bytes = 450'000'000;
        record_view_model_.audio_bytes = 12'000'000;
        record_view_model_.output_size_text = L"440.6 MB";
        record_view_model_.dropped_frames = 0;
        record_view_model_.av_drift_available = true;
        record_view_model_.av_drift_ms = 1.0;
        if (normalized == QLatin1String(visual::record_state::kRecordingAudioDegraded)) {
            // The scenario the visual catalogue has always named
            // (record-recording-audio-degraded) and that nothing in this
            // frontend rendered: the Widgets harness read a struct field, the
            // Quick harness switches on the scenario string, and the field was
            // left without a consumer when the Widgets shell was removed. A
            // scenario whose name promises a state it does not produce is worse
            // than no scenario -- every capture taken under it was evidence of
            // an ordinary recording.
            //
            // Raised through the production Enqueue() with the production
            // resolver, exactly as observeAudioSourceDegradation() does when a
            // real source loses its device, so what is photographed is the real
            // standing notification and not a harness lookalike.
            audio_degraded_toast_sequence_ =
                notifications_adapter_.manager().Enqueue(notifications::MakeAudioSourceDegradedEvent(1));
        }
    } else if (normalized == QLatin1String(visual::record_state::kCountdown)) {
        // A held countdown: the state and the remaining seconds are set, but the
        // tick timer is not started, so the frame is deterministic. This is the
        // only Record state the deterministic suite could not photograph — the
        // transport looks materially different in it (the split button's main
        // face becomes Cancel, its chevron goes inactive, and the timer shows a
        // bare digit rather than a clock), which is exactly the kind of state a
        // visual pass has to be able to see.
        countdown_remaining_ = 3;
        countdown_progress_ = 1.0;
        live_config_.countdown_seconds = 3;
        record_view_model_.SetState(UiRecordingState::Countdown);
    } else if (normalized == QLatin1String(visual::record_state::kPaused)) {
        record_view_model_.SetState(UiRecordingState::Paused);
        record_view_model_.live_stats_available = true;
        record_view_model_.elapsed_text = L"12:34";
        record_view_model_.elapsed_seconds = 754.0;
        record_view_model_.output_size_text = L"440.6 MB";
    } else if (normalized == QLatin1String(visual::record_state::kCompleted)) {
        // The post-recording state, which had no deterministic scenario at all —
        // so the one arrangement in which the transport's recommended action is
        // Edit rather than Record, and the one in which the page carries a
        // success banner, could never be photographed. Both are exactly the
        // things a visual pass has to be able to see.
        // The Edit affordance is gated on the recording actually existing on
        // disk, so the scenario writes a placeholder rather than pretending. It
        // goes to a scratch directory, never to the user's output folder.
        const QString fixture_directory =
            QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/exosnap-visual-fixtures");
        QDir().mkpath(fixture_directory);
        const QString fixture_path = fixture_directory + QStringLiteral("/2026-08-12_06-21-07_Desktop_Display 1.mkv");
        if (QFile fixture(fixture_path); fixture.open(QIODevice::WriteOnly))
            fixture.write("visual scenario placeholder");

        record_view_model_.last_succeeded = true;
        record_view_model_.result_output_path = fixture_path.toStdWString();
        record_view_model_.result_duration_seconds = 754.0;
        record_view_model_.result_output_file_bytes = 462'000'000;
        record_view_model_.current_completed_recording.succeeded = true;
        record_view_model_.current_completed_recording.file_path = fixture_path;
        record_view_model_.elapsed_text = L"12:34";
        record_view_model_.elapsed_seconds = 754.0;
        record_view_model_.output_size_text = L"440.6 MB";
        record_view_model_.SetState(UiRecordingState::Completed);
        // Deliberately no page notice: a successful stop no longer raises one,
        // and the scenario exists to photograph what the product actually does.
    } else if (normalized == QLatin1String(visual::record_state::kBlocked)) {
        // A blocker is a condition Diagnostics reports about the machine, not a
        // result of anything the user just did: no page notice, and Record is
        // simply unavailable. That is what makes it visually distinct from
        // Failed below, which is a run that started and ended badly.
        record_view_model_.SetState(UiRecordingState::Blocked);
        record_view_model_.capability_status_text =
            L"The selected format is unavailable on this GPU. Choose a supported profile in Settings.";
    } else if (normalized == QLatin1String(visual::record_state::kFailed)) {
        record_view_model_.SetState(UiRecordingState::Failed);
        record_view_model_.capability_status_text = L"Recording stopped because the capture source became unavailable.";
        record_view_model_.result_user_message = L"The capture source is no longer available.";
        // The same notice the real result callback raises. Without it the
        // scenario photographed a failure the page never states in words, which
        // is precisely the evidence a failure state needs to be judged on.
        record_view_model_adapter_.setNoticeText(QStringLiteral("The capture source is no longer available."),
                                                 QStringLiteral("error"));
    } else if (normalized == QLatin1String(visual::record_state::kUnavailable)) {
        record_view_model_.SetState(UiRecordingState::Ready);
        record_view_model_.selected_target_index = -1;
        microphone_available_ = false;
        webcam_available_ = false;
        record_preview_adapter_.clearPreviewTarget();
    } else {
        return false;
    }
    synchronizeRecordState();
    return true;
}

} // namespace exosnap::quick
