#pragma once

#include "AboutViewModelAdapter.h"
#include "BlockingSurfaceArbiter.h"
#include "CrashReportAdapter.h"
#include "DeviceAdapter.h"
#include "DiagnosticsAdapter.h"
#include "EditExportAdapter.h"
#include "EditPlayerAdapter.h"
#include "EditSessionAdapter.h"
#include "EditTimelineAdapter.h"
#include "EditTimelineModels.h"
#include "LogsAdapter.h"
#include "NotificationsAdapter.h"
#include "OverlayAdapter.h"
#include "QuickThemeTokens.h"
#include "QuickWindowGeometry.h"
#include "RecordPreviewAdapter.h"
#include "RecordViewModelAdapter.h"
#include "RecordWebcamFrameProvider.h"
#include "RecordingErrorAdapter.h"
#include "RecoveryAdapter.h"
#include "SettingsAdapter.h"
#include "ShellAdapter.h"

#include "models/RecordingPreset.h"
#include "models/RecordingPresetRegistry.h"
#include "services/AudioDeviceNotifier.h"
#include "services/CaptureTargetNotifier.h"
#include "services/GlobalHotkeyService.h"
#include "services/RecordingCountdownController.h"
#include "services/RecoveryService.h"
#include "services/UpdateService.h"
#include "services/WebcamDeviceNotifier.h"
#include "services/Win32HotkeyRegistrar.h"
#include "services/WindowEvidenceProbe.h"
#include "settings/AppSettingsStore.h"
#include "settings/RecordingPresetStore.h"
#include "settings/RecoveryManifestStore.h"
#include "ui/tray/TrayPresence.h"
#include "viewmodels/RecordViewModel.h"

#include <crash_capture/crash_capture.h>

#include <QAbstractNativeEventFilter>
#include <QElapsedTimer>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QThreadPool>
#include <QTimer>

class QQuickWindow;

#include <memory>
#include <optional>
#include <string>

namespace exosnap {
class RecordingCoordinator;
}

namespace exosnap::quick {

class QuickApplication {
  public:
    QuickApplication();
    ~QuickApplication();

    QuickApplication(const QuickApplication&) = delete;
    QuickApplication& operator=(const QuickApplication&) = delete;

    [[nodiscard]] bool load(bool no_activate = false);
    [[nodiscard]] AboutViewModelAdapter* aboutViewModel() noexcept;
    [[nodiscard]] RecordPreviewAdapter* recordPreviewAdapter() noexcept;
    [[nodiscard]] RecordViewModelAdapter* recordViewModelAdapter() noexcept;
    [[nodiscard]] SettingsAdapter* settingsAdapter() noexcept;
    [[nodiscard]] DeviceAdapter* deviceAdapter() noexcept;
    [[nodiscard]] DiagnosticsAdapter* diagnosticsAdapter() noexcept;
    [[nodiscard]] LogsAdapter* logsAdapter() noexcept;
    [[nodiscard]] EditSessionAdapter* editSessionAdapter() noexcept;
    [[nodiscard]] EditTimelineAdapter* editTimelineAdapter() noexcept;
    [[nodiscard]] EditPlayerAdapter* editPlayerAdapter() noexcept;
    [[nodiscard]] EditExportAdapter* editExportAdapter() noexcept;
    [[nodiscard]] ShellAdapter* shellAdapter() noexcept;
    [[nodiscard]] NotificationsAdapter* notificationsAdapter() noexcept;
    [[nodiscard]] RecoveryAdapter* recoveryAdapter() noexcept;
    [[nodiscard]] RecordingErrorAdapter* recordingErrorAdapter() noexcept;
    [[nodiscard]] CrashReportAdapter* crashReportAdapter() noexcept;
    [[nodiscard]] QQmlApplicationEngine& engine() noexcept;
    [[nodiscard]] RecordingCoordinator* recordingCoordinator() noexcept;
    // Read-only view of the shared record state. The adapter above exposes what
    // QML binds to; the Live Verify result snapshot needs the typed result
    // fields (paths, container/codecs, marker count) that never became QML
    // properties because no surface renders them individually.
    [[nodiscard]] const RecordViewModel& recordViewModel() const noexcept;
    [[nodiscard]] bool prepareRecordingBenchmark(uint32_t frame_rate, QString& error);

    // Automation only (--auto-record). Picks the first enumerated target of `kind`
    // whose description contains `title_filter` (ignored for monitors) and selects
    // it through the same path a source-picker click takes, so the live preview
    // shows exactly what is about to be recorded. Mirrors
    // RecordPage::selectCaptureTargetForAutomation on the Widgets side, which is
    // what lets the frontend A/B benchmark put both frontends in the same state.
    [[nodiscard]] bool selectCaptureTargetForAutomation(recorder_core::CaptureTarget::Kind kind,
                                                        const QString& title_filter);
    // Automation only (--auto-edit chained onto --auto-record). Opens the Editor
    // on the recording this process just finished, through the same
    // openEditorForCurrentRecording() the production completion path calls.
    // Explicit rather than relying on the open-editor-when-finished preference:
    // an automated gate must not silently pass or fail on a persisted user
    // setting. Returns false when there is no completed recording to open.
    [[nodiscard]] bool openEditorForAutomation();
    // Harness-only (--visual-test-size). Resizes the window to the size a
    // capture was asked for and takes the size away from the persistence layer
    // for the rest of the process.
    //
    // Both halves are needed. The window is placed on its persisted rect by a
    // deferred correction that fires after the first rendered frame, which is
    // AFTER a plain resize() — so a harness resize was being silently undone and
    // every capture came out at whatever size the developer last left the real
    // window at. And a size dictated on the command line is not a size the next
    // launch should reopen at, so nothing is written back.
    void applyHarnessWindowSize(const QSize& size);
    [[nodiscard]] bool applyRecordVisualScenario(const QString& scenario);
    // Harness-only (--overlay-visual-state). Seeds one of the runtime overlay
    // surfaces with deterministic content so a --visual-test capture can
    // photograph it without a real crash, a real failed recording, or a real
    // interrupted session on this machine. Never creates or drives a window, and
    // never touches the manifest, the crash sidecar or persisted settings.
    [[nodiscard]] bool applyOverlayVisualScenario(const QString& scenario);

    // ADR 0033: applies the handoff a prior elevated self-relaunch put in our own
    // argv. Called by the entry point straight after construction, before the
    // window loads, exactly as the Widgets frontend does — the page choice has to
    // be in place before the shell picks its landing page.
    void applyStartupRelaunchHandoff(const QString& page_name, bool reenable_present_diag);
    // ADR 0055: armed from --verify-update-reinstall for this run only. Nothing is
    // persisted, so a plain restart drops back to normal update behaviour.
    void applyVerifyUpdateReinstallMode(bool enabled);
    // Suppresses the tray icon for harness runs that would otherwise put a second
    // ExoSnap icon into the developer's notification area — a --visual-test sweep
    // is 179 processes, and a benchmark run measures a frontend, not a tray.
    // Deliberately NOT tied to every diagnostic mode: --smoke-test keeps the tray
    // on, which is what makes the QApplication + QSystemTrayIcon startup path
    // automatically covered instead of only provable by hand.
    void applyTraySuppression(bool suppressed);

  private:
    // Crash session sidecar (ADR 0017). Reads the previous session's context
    // before overwriting it, so a crash in the last run stays detectable.
    void initializeCrashSession();
    [[nodiscard]] crash_capture::SessionContext currentCrashSessionContext() const;
    void refreshCrashSessionContext();
    // Reconciles the SDK-wide persisted consent with the app's own explicit
    // crash-report policy. Without it the Settings row is a preference that
    // records the user's choice and then never acts on it.
    void applyCrashReportPolicy();
    // Narrows AppLog's recording filter to the persisted developer log level.
    // Same category of gap as the consent above: the setting was persisted and
    // displayed, but nothing ever applied it in this frontend.
    void applyDeveloperLogLevel();
    void initializeRecordWorkflow();
    // Hardware capability query, off the GUI thread. Until it lands the
    // coordinator stays in LoadingCapabilities and the surfaces render against
    // the empty set; onCapabilitiesReady rebuilds them.
    void startCapabilityProbe();
    void onCapabilitiesReady(const capability::CapabilitySet& capabilities);
    void onCapabilityProbeFailed(const QString& reason);
    // Re-seeds a --record-visual-state / --overlay-visual-state after the
    // capability probe has pushed the coordinator's own state over it. See the
    // definition for why this cannot be a delay instead.
    void reapplyVisualScenarios();
    // True once a visual scenario has been seeded. While it is, the coordinator's
    // own state pushes stop reaching the view model — otherwise a probe or a
    // notifier landing after the seed silently reverts the scenario.
    [[nodiscard]] bool visualScenarioLatched() const noexcept;
    void initializeSettingsArea();
    void initializeDiagnosticsArea();
    // Rebuilds the diagnostics inputs from the live settings + capabilities.
    // Called whenever anything the recommendation engine reads has changed.
    void refreshDiagnosticsData();
    void applyDiagnosticsFix(const QString& fix_id);
    void selectHostingMonitorForSelectedWindow();
    // Harness-only, env-configured: seeds deterministic Diagnostics/Logs
    // content so a --visual-test capture never photographs whatever this
    // machine happened to be doing. Never creates or drives a window.
    void applyDiagnosticsVisualScenarios();
    void initializeEditArea();
    // Close guards: samples what is in flight and applies the effects the user
    // confirmed. The ordering and wording live in models/CloseGuardPolicy.
    void initializeShell();
    // Tray presence and the close-to-tray contract. Requires the root window, so
    // it runs from load() rather than from the constructor. Absent entirely when
    // the platform reports no system tray, which is also what makes
    // ShouldHideToTray refuse to hide — there would be no way back to the window.
    void initializeTray();
    // Pushes the current recording state onto the tray icon/tooltip. Called from
    // the same place the Record surface is synchronized, so the tray can never
    // disagree with the window about whether a recording is running.
    void refreshTrayState();
    // Brings the window back from the tray (tray icon click, "Show window", or
    // the unread-notifications mirror).
    void restoreWindowFromTray();
    // Notification event sources and the action router. Dispatch lives here
    // rather than in the adapter: navigating, opening Explorer and relaunching
    // elevated are application concerns, not QML-boundary ones.
    // Startup recovery (ADR-0014/ADR-0015): scans the manifest the coordinator
    // writes, raises the standing notification and puts the surface up. Without
    // this the manifest was written and never read — an interrupted recording
    // stayed on disk with nothing offering to save it.
    void initializeDisplayGeometryWatch();
    void initializeBlockingSurfaces();
    void initializeRecovery();
    // Routes the two actions the failure surface offers. The consent grant and
    // the scrubbed non-fatal report are SDK concerns, so they live here rather
    // than behind the QML boundary.
    void initializeRecordingError();
    // QCR-415. Hands the failure to the arbiter instead of raising the surface
    // straight away, and holds the report until the arbiter says it is this
    // surface's turn. The report has to live somewhere while it waits, and only
    // the composition root has it.
    void presentRecordingFailure(const models::RecordingFailureReport& report, bool can_send_report);
    // Next-launch crash consent (ADR 0017). The crash surface is an in-window
    // overlay of this application, not a separate reporter executable, so it is
    // part of the main-app cutover. Raised only when the persisted policy says
    // to ask; deferred behind recovery so the two never stack.
    void initializeCrashReport();
    void showCrashReportSurface();
    bool applyCrashConsentAction(CrashConsentAction action);
    void initializeNotifications();
    // ADR 0012: the update check, the Settings card state machine and the updater
    // handoff. Until this existed the card rendered state nobody ever set and its
    // button reached nothing.
    void initializeUpdates();
    // Push the persisted channel into UpdateService and drop the previous
    // channel's answer. Called on every change, not only at startup (QCR-202).
    void applyUpdateChannel();
    void triggerUpdateCheck(bool manual);
    void onUpdateCheckComplete(const exosnap::update::UpdateCheckResult& result);
    void runUpdatePrimaryAction();
    void dispatchNotificationAction(notifications::NotificationAction action, const QString& payload);
    void publishRecordingResultNotification(const UiRecordingResult& result);
    [[nodiscard]] CloseGuardState sampleCloseGuardState() const;
    // Flushes anything a debounced timer still owes to disk. Runs on the way
    // out so a quit inside the debounce window never loses the last edit.
    void flushPendingPersists();
    // Production Record -> Editor handoff (ADR 0022). Builds the EditContext
    // from the completed session and hands it to the session adapter, which is
    // what makes the overlay appear.
    void openEditorForCurrentRecording();
    [[nodiscard]] bool canOpenEditorForCurrentRecording() const;
    // Harness-only, env-configured (EXOSNAP_VISUAL_EDIT_SCENARIO): seeds a
    // deterministic Edit surface so a --visual-test capture never depends on
    // there being a real recording on this machine. Never creates or drives a
    // window, and never starts a real export.
    void applyEditVisualScenario();
    void wireSettingsCommands();
    // Mirrors a Settings edit into the live config, the recording side, and disk.
    void applySettingsConfigEdit();
    // live_config_ is the single owner of RecordingPresetConfig; SettingsAdapter
    // and RecordViewModel both hold mirrors of overlapping slices of it. A
    // mutation on either surface therefore has to refresh the other mirror, or
    // the next edit on the far side writes its stale copy back over the near
    // side's change -- and persists it. Both directions are covered here.
    void syncConfigMirrors();
    // Same problem one level up: SettingsAdapter mirrors the whole
    // PersistedAppSettings, so a hotkey rebind that only touches settings_ is
    // silently reverted by the next Settings toggle.
    void saveAndPublishAppSettings(SettingsWriteIntent intent = SettingsWriteIntent::Incidental);
    // The one write edge for settings_. Returns false when the write was
    // refused (blocked by a failed load) or failed (reported as
    // SettingsSaveFailed).
    bool persistAppSettings(SettingsWriteIntent intent);
    void applyPresetConfig(RecordingPresetConfig config);
    void refreshPresetState();
    void applyThemeFromSettings();
    void initializeHotkeys();
    void refreshHotkeyRows();
    void triggerHotkeyAction(HotkeyAction action);
    void exportSelectedPreset(const QString& path);
    void importPresetsFromFile(const QString& path);
    void wireRecordCommands();
    void synchronizeRecordState();
    void selectTarget(int target_index, CaptureMode mode);
    void selectRegion(const QRectF& normalized_rect);
    void startRequested();
    void startRecordingNow();
    void cancelCountdown();
    void updateCountdown();
    void toggleSource(const QString& key);
    void updateWebcamOverlay(const QRectF& normalized_rect);
    void updateMeters();
    void scheduleMeterUpdate();
    void updateMeterServices();
    // The deferred half of updateMeterServices(): opens the endpoints the current
    // state wants. Re-checks the stop condition, because it runs one debounce
    // interval after the decision that scheduled it.
    void startMeterServices();
    void refreshCaptureTargets(const CaptureTargetSnapshot& snapshot, DiscoveryReason reason);
    void updateOutputTargetContext(const recorder_core::CaptureTarget& target);
    void persistLiveConfig();
    [[nodiscard]] std::optional<recorder_core::CaptureTarget> selectedCaptureTarget() const;

    // QCR-110. Points the exclusive-fullscreen probe at whatever is selected now,
    // pauses it while the recording engine owns the capture, and pushes the
    // target-derived facts (selection, HDR) into Diagnostics. Called from every
    // edge that can change the selection or the recording state.
    //
    // The probe is created on first use and only for a WINDOW target: a monitor
    // capture can record exclusive fullscreen, so there is nothing to prove, and
    // the display-capture user should not pay for a WGC subscription and a D3D11
    // device that will never be read.
    void updateCaptureEvidenceTarget();
    // Pulls the probe's snapshot on a light cadence and pushes it into Diagnostics
    // only when it differs from what was pushed last — the evidence changes on a
    // human scale, and each push re-runs the whole recommendation checklist.
    void refreshCaptureWindowEvidence();
    // The exclusive-fullscreen verdict for `target`, from the probe's stable
    // snapshot. Reads no GUI state and takes only the probe's own mutex, so it is
    // safe as the coordinator's UI-thread-called admission provider. Returns None
    // for any target the snapshot does not describe, so a retarget can never be
    // judged by the previous window's evidence.
    [[nodiscard]] diagnostics::ExclusiveEvidence
    resolveWindowExclusiveEvidence(const recorder_core::CaptureTarget& target) const;

    AppSettingsStore settings_store_;
    PersistedAppSettings settings_;
    RecordingPresetStore preset_store_;
    RecordingPresetConfig live_config_;
    capability::CapabilitySet capabilities_;
    RecordingPresetRegistry preset_registry_;
    RecoveryManifestStore recovery_manifest_store_;
    // Declared after the store it borrows: RecoveryService holds a reference.
    RecoveryService recovery_service_;
    AboutViewModelAdapter about_view_model_;
    SettingsAdapter settings_adapter_;
    DeviceAdapter device_adapter_;
    DiagnosticsAdapter diagnostics_adapter_;
    LogsAdapter logs_adapter_;
    EditSessionAdapter edit_session_adapter_;
    EditTimelineAdapter edit_timeline_adapter_;
    EditPlayerAdapter edit_player_adapter_;
    EditExportAdapter edit_export_adapter_;
    ShellAdapter shell_adapter_;
    NotificationsAdapter notifications_adapter_;
    RecoveryAdapter recovery_adapter_;
    RecordingErrorAdapter recording_error_adapter_;
    CrashReportAdapter crash_report_adapter_;
    // After both surfaces it arbitrates: it connects to them in setSurfaces()
    // and must be destroyed before they are.
    BlockingSurfaceArbiter surface_arbiter_;
    RecordViewModel record_view_model_;
    RecordViewModelAdapter record_view_model_adapter_;
    // After record_view_model_: it holds a pointer into it and is constructed
    // with that pointer.
    OverlayAdapter overlay_adapter_;
    RecordPreviewAdapter record_preview_adapter_;
    // Declared BEFORE the coordinator so it is destroyed AFTER it: the coordinator
    // holds an evidence provider that reads this probe. Null until a window target
    // is selected for the first time.
    std::unique_ptr<WindowEvidenceProbe> window_evidence_probe_;
    std::unique_ptr<RecordingCoordinator> recording_coordinator_;
    CaptureTargetNotifier capture_target_notifier_;
    AudioDeviceNotifier audio_notifier_;
    WebcamDeviceNotifier webcam_notifier_;
    GlobalHotkeyService hotkey_service_;
    // Created in initializeUpdates(), after the coordinator exists: the service's
    // recording guard reads it. Owns its own worker pool and joins it on destroy.
    std::unique_ptr<UpdateService> update_service_;
    UpdateHandoffPhase update_handoff_phase_ = UpdateHandoffPhase::Idle;
    QString last_available_version_;
    // ADR 0055, argv-armed for this run only; never persisted.
    bool verify_update_reinstall_ = false;
    bool tray_suppressed_ = false;
    // Harness only. Empty unless the corresponding CLI option was given.
    QString pending_record_visual_state_;
    QString pending_overlay_visual_state_;
    // Harness-only. A --visual-test run that seeded a synthetic Diagnostics state
    // must keep it: the capability probe lands asynchronously ~3 s later and its
    // refresh would otherwise overwrite the scenario with this machine's real,
    // healthy environment — which is exactly what every `diagnostics__issues`
    // capture in the earlier baselines silently photographed.
    bool diagnostics_visual_scenario_active_ = false;
    bool reapplying_visual_scenarios_ = false;
    // The relaunch handoff is applied before load(), when QML is not connected to
    // navigateToPageRequested yet, so the page is parked here and emitted once the
    // shell exists. Empty means "no handoff" — it used to be a -1 sentinel in an
    // int, which is exactly the bare-integer navigation QCR-716 removed.
    std::optional<ShellAdapter::Page> pending_landing_page_;
#if defined(Q_OS_WIN)
    std::unique_ptr<Win32HotkeyRegistrar> hotkey_registrar_;
    std::unique_ptr<QAbstractNativeEventFilter> hotkey_event_filter_;
#endif
    RecordingCountdownController countdown_;
    QElapsedTimer countdown_clock_;
    QTimer countdown_timer_;
    // Coalesces meter/webcam-preview STARTS only; stops stay synchronous. See
    // updateMeterServices().
    QTimer meter_service_start_timer_;
    QTimer meter_update_timer_;
    // 1 Hz, and only while the probe has a window target. The probe's own fact
    // poll runs at the same cadence, so a faster pull would only re-read a value
    // that cannot have changed.
    QTimer capture_evidence_timer_;
    QTimer webcam_frame_delivery_timer_;
    QTimer webcam_overlay_persist_timer_;
    int countdown_remaining_ = 0;
    // Unrounded companion to countdown_remaining_, updated on the same 100 ms
    // tick, so the ring can follow the clock instead of the digit.
    double countdown_progress_ = 0.0;
    // Post-flight numbers the Edit surface's report badge reads. Latched from
    // the diagnostics stream during the session (the final snapshot arrives
    // before the result callback) and reset on every fresh start.
    recorder_core::RecordingDiagnosticsSnapshot last_completed_snapshot_;
    double peak_av_drift_ms_ = 0.0;
    bool av_drift_ever_available_ = false;
    float preflight_system_rms_ = 0.0f;
    float preflight_app_rms_ = 0.0f;
    float preflight_microphone_rms_ = 0.0f;
    bool microphone_available_ = true;
    bool webcam_available_ = true;
    QString webcam_error_;
    RecordWebcamFrameProvider* webcam_frame_provider_ = nullptr;
    // Engine-owned once registered (addImageProvider takes ownership), same
    // lifetime rule the webcam provider above follows.
    EditTimelineTileProvider* edit_tile_provider_ = nullptr;
    bool edit_tile_provider_registered_ = false;
    quint64 webcam_frame_revision_ = 0;
    bool webcam_provider_registered_ = false;
    bool capture_target_refresh_pending_ = false;
    // What the probe was last told. synchronizeRecordState() is the single edge
    // that re-evaluates this and runs at stats cadence during a recording, so the
    // unchanged case must not wake the probe's worker.
    uintptr_t evidence_target_hwnd_ = 0;
    bool evidence_paused_ = false;
    // The selection Diagnostics currently holds. Same reason as the two below:
    // pushing it re-runs the recommendation checklist, so only a real change may.
    std::optional<recorder_core::CaptureTarget> pushed_selected_target_;
    // What refreshCaptureWindowEvidence() last handed to Diagnostics. Kept so an
    // unchanged snapshot costs one comparison instead of a checklist rebuild.
    std::optional<WindowEvidenceProbe::Snapshot> pushed_window_evidence_;
    // The HDR verdict for the selected target that Diagnostics currently holds.
    // Unset until the first push, so the first SDR target still writes once.
    std::optional<bool> pushed_capture_target_hdr_active_;
    // Empty when the crash directory could not be resolved, i.e. session
    // tracking is off for this run.
    std::string crash_dir_;
    // Set when the previous session never marked a clean exit. The next-launch
    // crash overlay is not migrated yet, so today this only reaches the log.
    std::optional<crash_capture::SessionContext> pending_crash_;
    // QCR-415. The failure report waiting for its turn behind another blocking
    // surface. Overwritten by a later failure, matching the surface's own rule
    // that the newest attempt is the one the user just made.
    std::optional<models::RecordingFailureReport> pending_recording_failure_;
    bool pending_recording_failure_can_send_ = false;
    // Harness-only Expert override; suppresses the persist of expert_mode_enabled.
    bool visual_expert_override_ = false;
    // Set when the preset store had to repair fields while loading. Raised as a
    // notification once the manager exists, never swallowed.
    bool preset_store_repaired_ = false;
    // QCR-201. Set once the user has deliberately written over a settings file
    // that failed to load: from then on the file is theirs again and every
    // write, incidental ones included, is legitimate. See ResolveSettingsWrite.
    bool settings_load_failure_superseded_ = false;
    // Latched like preset_store_repaired_: the load happens in the constructor,
    // long before the notification manager is wired up in initializeNotifications().
    bool settings_load_failed_pending_ = false;
    // One warning per session for refused incidental writes — the geometry
    // debounce alone would otherwise fill the log.
    bool settings_block_logged_ = false;
    // Created in load(), once the root window exists. Declared before engine_ so
    // it outlives the window it tracks; it holds a QPointer, so the window dying
    // first is safe.
    std::unique_ptr<QuickWindowGeometry> window_geometry_;
    // Null when the platform reports no system tray. Declared before engine_ for
    // the same reason window_geometry_ is: it outlives the window it acts on.
    std::unique_ptr<ui::tray::TrayPresence> tray_presence_;
    // The engine owns the window; this only observes it. A QPointer because the
    // engine can destroy the window while this object is still alive, and every
    // tray action would otherwise act on a dangling pointer.
    QPointer<QQuickWindow> root_window_;
    // Latched by the tray "Quit" action and consumed by the very next close
    // attempt, exactly as the Widgets shell's force_quit_ is: it is what makes
    // that one close bypass the hide-to-tray branch and reach the guards.
    bool force_quit_ = false;
    QQmlApplicationEngine engine_;

    // Declared last so it is destroyed FIRST: its destructor waits for the
    // in-flight hardware capability query. The probe captures `this`, and its
    // DXGI/encoder enumeration must not still be running once the members above
    // — or Qt's own statics — are gone.
    QThreadPool capability_probe_pool_;
};

} // namespace exosnap::quick
