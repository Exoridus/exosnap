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
#include "QuickWindowChrome.h"
#include "QuickWindowGeometry.h"
#include "RecordPreviewAdapter.h"
#include "RecordViewModelAdapter.h"
#include "RecordWebcamFrameProvider.h"
#include "RecordingErrorAdapter.h"
#include "RecoveryAdapter.h"
#include "SettingsAdapter.h"
#include "ShellAdapter.h"
#include "ShellIconProvider.h"
#include "ShellPresenceAdapter.h"
#include "TaskbarPresence.h"
#include "TrayAdapter.h"
#include "WhatsNewAdapter.h"

#include "diagnostics/AudioSourceDegradation.h"
#include "diagnostics/DpcLatencyProvider.h"
#include "diagnostics/ElevationProvider.h"
#include "diagnostics/PresentMonProvider.h"
#include "diagnostics/WindowCaptureStall.h"
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
#include "viewmodels/RecordViewModel.h"

#include <capability/resolver.h>
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
    // The present-diagnostics provider, or null when this build/launch has none.
    // Sampling DRAINS the ETW queue and advances reader-side accumulators, so it is
    // GUI-thread-only -- which every caller is, because the control channel
    // marshals its dispatch onto the GUI thread before touching this.
    [[nodiscard]] diagnostics::PresentMonProvider* presentProvider() noexcept;
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
    [[nodiscard]] bool selectCaptureTargetForAutomation(exosnap::engine::CaptureTarget::Kind kind,
                                                        const QString& title_filter);
    // Automation only (--auto-edit chained onto --auto-record). Opens the Editor
    // on the recording this process just finished, through the same
    // openEditorForCurrentRecording() the production completion path calls.
    // Explicit rather than relying on the open-editor-when-finished preference:
    // an automated gate must not silently pass or fail on a persisted user
    // setting. Returns false when there is no completed recording to open.
    [[nodiscard]] bool openEditorForAutomation();
    // The gate openEditorForAutomation() applies, published so the control
    // channel's availableActions and its precondition read the same predicate
    // the intent does instead of a second guess at it.
    [[nodiscard]] bool canOpenEditor() const;
    // Which blocking surface is up, straight from the arbiter that decides it.
    [[nodiscard]] const BlockingSurfaceArbiter& blockingSurfaces() const noexcept {
        return surface_arbiter_;
    }

    // What the LAST start request actually did. The recording start policy sits
    // in startRequested() and nowhere else -- it cancels a countdown, refuses
    // under a blocking surface (QCR-415), refuses when the transport says it
    // cannot start, and refuses when there is no selected target. Before this,
    // that function returned void and merely logged its refusals, so the control
    // channel checked canStart() on its own and answered `ok:true` for a start
    // that the product then dropped on the floor: a false success in the
    // automation contract of a truthfulness release.
    //
    // A latch rather than a return value because the caller is a SIGNAL
    // connection -- the control channel presses the same
    // RecordViewModelAdapter::requestStart() the button presses, and reads the
    // outcome here afterwards. The connection is direct (both objects live on
    // the GUI thread), so this is set by the time requestStart() returns.
    enum class StartAdmission {
        Accepted,
        // A start pressed during the countdown cancels it. Product behaviour,
        // and not a refusal: the request was honoured.
        CountdownCancelled,
        RefusedByBlockingSurface,
        RefusedByState,
        RefusedNoTarget,
    };
    [[nodiscard]] StartAdmission lastStartAdmission() const noexcept {
        return last_start_admission_;
    }
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
    // Harness-only: the Expert/Simple arrangement, through the same path the
    // Appearance card's own toggle uses.
    void applyHarnessExpertMode(bool expert);
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

    // The dev feed override (--update-base-url) and, when this process is itself
    // under a control channel, the run id the updater child is to be given.
    // Both are plumbed into UpdateService rather than kept here, because that is
    // where the check and the launch read them.
    void applyUpdateFeedOverride(const QString& base_url);
    void applyUpdaterAutomationRunId(const QString& run_id);

    // --- Update intents reachable by the control channel ---------------------
    // The SAME entry points the Settings update card drives -- deliberately not
    // a shortcut into UpdateService, because what an acceptance run has to prove
    // is the path a user takes. requestUpdateCheck() is the card's manual check
    // (recording guard, loop-guard reset); requestUpdatePrimaryAction() is its
    // primary button, which launches the updater exactly when the card offers an
    // update and otherwise re-checks -- the caller's precondition is what keeps
    // "apply" from silently meaning "check".
    void requestUpdateCheck();
    void requestUpdatePrimaryAction();
    [[nodiscard]] const UpdateService* updateService() const noexcept;
    // Non-const overload for the one caller that needs to CONNECT to the
    // service's signals -- main.cpp turning updaterLaunched into a control
    // event. Everything else reads.
    [[nodiscard]] UpdateService* updateService() noexcept;

    // --- Observability inputs (Wave C) ---------------------------------------
    // Narrow const reads for the control channel's observability surfaces. Each
    // one hands back a model this class already owns; none of them computes a
    // second version of anything the product decides elsewhere.

    // What the user has configured, as stored. This is the REQUESTED level.
    [[nodiscard]] const RecordingPresetConfig& liveConfig() const noexcept {
        return live_config_;
    }
    [[nodiscard]] const PersistedAppSettings& appSettings() const noexcept {
        return settings_;
    }
    [[nodiscard]] const capability::CapabilitySet& capabilities() const noexcept {
        return capabilities_;
    }

    // Re-reads the per-display DXGI facts and re-publishes what the product derives
    // from them. The capability probe writes `runtime.displays` exactly once at
    // startup, and HDR is a Windows-global toggle the user can flip at any moment
    // afterwards -- so without this, the HDR-handling settings row, the Diagnostics
    // HDR card and environment.snapshot all keep reporting the state the desktop was
    // in when ExoSnap launched.
    //
    // GUI-thread only, and cheap by the same argument that lets the coordinator call
    // QueryDisplayFacts() inline on the admission path: a DXGI factory, an output
    // walk and one QueryDisplayConfig, no encoder session and no device creation.
    void refreshDisplayFacts();

    // The two consumers of the compositor rate that are pushed rather than pulled:
    // the Expert frame-rate ceiling and the Diagnostics display facts. Both used to
    // be read once at startup, which survived only as long as the rate did -- Qt
    // 6.11 tracks a mid-session mode-set that earlier versions missed.
    void publishRefreshRateDerivedState();

    [[nodiscard]] QString settingsFilePath() const {
        return settings_store_.SettingsFilePath();
    }
    [[nodiscard]] const AudioDeviceNotifier& audioDeviceNotifier() const noexcept {
        return audio_notifier_;
    }

    // The configuration the NEXT recording would actually use, and the resolver's
    // account of how it got there. Produced by running the product's own
    // reconciliation -- SanitizePresetConfig followed by
    // capability::SettingsResolver -- rather than by a second set of rules.
    // `evaluated` is false until the capability probe has landed; before that
    // there is no hardware verdict to report and the resolver's empty adjustment
    // list must not be read as "nothing needed changing".
    struct EffectiveRecordingConfig {
        RecordingPresetConfig config;
        capability::ResolveResult resolution;
        bool evaluated = false;
    };
    [[nodiscard]] EffectiveRecordingConfig resolveEffectiveConfig() const;

    // Why the update area refuses to act right now, in product vocabulary:
    // "" (nothing in the way) | "recording" | "finalizing" | "updaterRunning".
    // ONE rule, read by the card's own guard and by the control channel's
    // preconditions -- so a client is never told an action is available and then
    // refused by the intent behind it. Scoop and "restart pending" are card
    // STATES rather than blockers: a check is still meaningful in both, and it
    // is the apply that has no offer to act on.
    [[nodiscard]] QString updateBlockerReason() const;

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
    // Applies the "Show notifications" setting to the notification manager. Same
    // category of gap again: the switch was surfaced, persisted and exported to
    // automation, and turning it off suppressed nothing at all. Per product-spec §9 it
    // gates only the toast glance -- the hub records every event regardless.
    void applyShowNotifications();
    // Pushes `hide_window_from_capture` onto the shell window. Fail-open: a
    // refused platform call leaves the window visible and logs it.
    void applyWindowCaptureExclusion();
    // Opens or closes the kernel DPC/ISR trace against the same gate the present
    // provider evaluates (opt-in AND elevation). Graceful: an unelevated process or a
    // refused session simply keeps measuring nothing, and nothing is then reported.
    void applyDpcLatencyGate();
    void initializeRecordWorkflow();
    // Hardware capability query, off the GUI thread. Until it lands the
    // coordinator stays in LoadingCapabilities and the surfaces render against
    // the empty set; onCapabilitiesReady rebuilds them.
    void startCapabilityProbe();
    void onCapabilitiesReady(const capability::CapabilitySet& capabilities);
    void onCapabilityProbeFailed(const QString& reason);
    // Pushes the two pieces of product state derived from `capabilities_.runtime.displays`
    // -- the HDR-handling row's gate and the selected target's HDR fact -- and nothing
    // else. Separate from updateCaptureEvidenceTarget() so a display re-probe can
    // re-publish them without also re-subscribing the WGC evidence probe.
    void publishDisplayDerivedState();
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
    void applyShellVisualScenarios();
    void initializeEditArea();
    // Close guards: samples what is in flight and applies the effects the user
    // confirmed. The ordering and wording live in models/CloseGuardPolicy.
    void initializeShell();
    // Tray presence. Requires the root window, so it runs from load() rather than
    // from the constructor. Absent entirely when the platform reports no system
    // tray, which is also what makes EvaluateMinimize refuse to hide -- there
    // would be no way back to the window.
    void initializeTray();
    // Binds the shell projection to the window: the chrome's registered
    // TaskbarButtonCreated message, its WM_COMMAND route and its handle identity
    // on one side, the taskbar surfaces on the other. Runs from load() for the
    // same reason initializeTray() does -- it needs the root window.
    void initializeShellPresence();
    // Feeds the current recording state into the shell projection. Called from
    // the same place the Record surface is synchronized, so no shell surface can
    // disagree with the window about whether a recording is running.
    void refreshTrayState();
    // Renders the projection onto every shell surface: the tray icon and menu,
    // the window icon (which is what the taskbar BUTTON shows), and the taskbar
    // button's badge and thumbnail transport. Also runs on each heartbeat tick,
    // which is why every writer below is change-guarded.
    void applyShellPresence();
    // Routes a shell-raised action into the SAME request the in-app transport
    // makes. Not a second interpretation of what Pause means.
    void performShellAction(ShellAction action);
    // Opens the configured recording destination in Explorer. Shared by the tray
    // menu and anything else that offers it, so there is one answer to where
    // recordings go.
    void openConfiguredOutputFolder();
    // Binds the three long operations that publish a fraction to the one taskbar
    // progress bar. Each takes a lease, so a callback that outlives its operation
    // cannot move the next one's bar.
    void wireTaskbarProgress();

    TaskbarProgressLease saving_progress_lease_;
    TaskbarProgressLease recovery_progress_lease_;
    TaskbarProgressLease export_progress_lease_;
    // The window-icon variant currently applied. Held because refreshTrayState()
    // also runs on the diagnostics tick, and re-applying the icon there would be a
    // taskbar redraw per tick for no change.
    // Brings the window back from the tray (tray icon click, "Show window", or
    // the unread-notifications mirror).
    // Banks the geometry, hides the window and raises the one-time notice.
    // Reached from the close-to-tray preference and from the tray menu's own
    // "Hide window" entry, which must not grow a second copy of it.
    void hideWindowToTray();
    // Whether the window was maximized when it went to the tray. Banked at hide
    // time because nothing readable at restore time still says so: the hide
    // minimizes first, and the restore un-minimizes before this is consulted.
    bool hidden_while_maximized_ = false;
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
    // "What's new" (product-spec). Both entry points, and nothing else, reach the
    // one overlay:
    //   - the Settings card link, with the full channel reference list;
    //   - the pending payload the previous update wrote, consumed once at startup.
    void initializeWhatsNew();
    void showWhatsNewForUpdateCard();
    // Consumes the pending payload: reads it, decides with ShouldShowWhatsNew(),
    // and clears it either way — a payload for another version, or one the
    // suppress setting hides, is not kept to be re-decided on every later launch.
    void consumePendingWhatsNewPayload();
    // Raises the post-update overlay, or holds the notes until the blocking
    // surfaces have cleared. A changelog must never stack on top of a question.
    void presentPostUpdateWhatsNew(const QVector<WhatsNewNote>& notes);
    // The staged updater has verified the package and is asking this process to
    // get out of the way so it can replace the installation. Ends the process --
    // deliberately bypassing close-to-tray, which would leave the executable
    // locked — unless a recording is in flight, which is the ONE reason to
    // refuse: the guard that blocked starting the update is not weakened by the
    // handoff, and the updater reports the honest appWontClose instead.
    void closeForUpdaterHandoff();
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
    // live_config_.webcam with an unpinned device_id resolved to the first
    // enumerated camera. Everything that opens a camera goes through this;
    // live_config_ itself keeps the unpinned value.
    [[nodiscard]] WebcamSettings webcamSettingsForCapture() const;
    void applyThemeFromSettings();
    // The appearance the WINDOWS SHELL is drawing, for the two marks that are
    // composited onto it rather than onto an ExoSnap surface.
    [[nodiscard]] QString shellAppearanceId() const;
    void refreshShellChromeAppearance();
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
    // False when there was nothing to record: no selected target, or Region mode
    // without a valid region. Reported rather than silently returned so the
    // start latch above can say which refusal it was.
    [[nodiscard]] bool startRecordingNow();
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
    void updateOutputTargetContext(const exosnap::engine::CaptureTarget& target);
    void persistLiveConfig();
    [[nodiscard]] std::optional<exosnap::engine::CaptureTarget> selectedCaptureTarget() const;

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
    resolveWindowExclusiveEvidence(const exosnap::engine::CaptureTarget& target) const;

    // QCR-804. Feeds one live diagnostics snapshot to the mid-recording capture
    // stall monitor and acts on what it reports: classify a confirmed starvation,
    // raise the standing warning, or clear it when frames come back. Driven only
    // by the coordinator's diagnostics callback (~5 Hz, Qt main thread) — no timer
    // and no probe of its own.
    void observeWindowCaptureStall(const exosnap::engine::RecordingDiagnosticsSnapshot& snapshot);
    // Announces the present-attribution boundary. `pid` is the captured window's
    // process for Window targets and 0 for Display/Region (whose presenter is the
    // dominant one, exactly like the idle desktop).
    //
    // Every forward resets the per-recording present / discard / mode-flip
    // accumulators, so the two callers want different things and say so:
    //
    //   force == false (idle selection change) -- skip when the pid did not move, so
    //     re-selecting the same target does not churn.
    //   force == true (recording start) -- reset UNCONDITIONALLY. A Display or Region
    //     recording targets pid 0 exactly like the idle desktop did a moment earlier,
    //     so a pid-equality guard would carry every present counted while the user was
    //     still choosing a target into the recording's statistics.
    void updatePresentAttribution(unsigned long pid, bool force);
    // The captured window's process id for the current selection, or 0.
    [[nodiscard]] unsigned long presentTargetPidForSelection() const;
    // Dismisses the standing capture-stall toast if one is up. Called when frames
    // resume and again when the session leaves Recording/Paused — the toast says
    // "the recording is still running", which stops being true then.
    void clearWindowCaptureStallWarning();

    // ADR 0046. Feeds one live diagnostics snapshot to the audio-source
    // degradation latch and acts on what it reports: raise or replace the
    // standing "audio source went silent" notice, or clear it once every source
    // is capturing again. Same shape and same driver as the capture-stall path
    // above — the pipeline's existing AudioDiagnostics health facts, no second
    // detection.
    void observeAudioSourceDegradation(const exosnap::engine::RecordingDiagnosticsSnapshot& snapshot);
    // Dismisses the standing audio-degradation toast if one is up. Called when
    // every source recovers and again when the session leaves Recording/Paused —
    // the toast says the recording continues, which stops being true then.
    void clearAudioSourceDegradedWarning();

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
    // ADR 0033 DPC/ISR latency. Declared BEFORE the adapter that borrows it: the
    // adapter samples it on every evaluation, so this member has to outlive it —
    // members are destroyed in reverse declaration order. Owns a real kernel trace
    // only while the same gate the present provider uses (opt-in AND elevation) is
    // open, and nothing at all otherwise.
    diagnostics::DpcLatencyProvider dpc_provider_;
    DiagnosticsAdapter diagnostics_adapter_;
    // ADR 0033 present diagnostics. Declared BEFORE the provider that borrows it:
    // PresentMonProvider holds a reference to the elevation provider for its whole
    // lifetime, so this member must outlive it.
    diagnostics::Win32ElevationProvider elevation_provider_;
    // Null until initializeDiagnosticsArea() runs. Owns a real ETW session while the
    // gate (opt-in AND elevation) is open and nothing at all otherwise -- an
    // unelevated launch never opens a session and never prompts for one.
    std::unique_ptr<diagnostics::PresentMonProvider> present_provider_;
    // The process id present statistics are currently attributed to (0 == dominant
    // presenter / no window target). Kept so the attribution boundary is only
    // announced to the session when it actually moves; every announcement resets the
    // per-recording accumulators.
    unsigned long present_target_pid_ = 0;
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
    WhatsNewAdapter whats_new_adapter_;
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
    // The releases page URL from the last completed check. Empty until then, and
    // the "All releases" link falls back to the product's own address.
    // The releases page the last update check reported. Empty until one has
    // completed, and passed on empty rather than substituted here: WhatsNewAdapter
    // owns the product's own releases address, so that fallback stays in one place.
    QString last_releases_page_url_;
    // Post-update notes held back because a blocking surface owns the screen.
    // Empty whenever nothing is waiting; the payload behind it is already gone,
    // because the decision to show was made when it was read.
    QVector<WhatsNewNote> deferred_whats_new_notes_;
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
    // The relaunch handoff is applied before load(), so the page is parked here
    // and handed to the shell as its STARTING destination (`landingPage`) when
    // the engine is loaded. Empty means "no handoff" — it used to be a -1
    // sentinel in an int, which is exactly the bare-integer navigation QCR-716
    // removed.
    std::optional<ShellAdapter::Page> pending_landing_page_;
#if defined(Q_OS_WIN)
    std::unique_ptr<Win32HotkeyRegistrar> hotkey_registrar_;
    std::unique_ptr<QAbstractNativeEventFilter> hotkey_event_filter_;
    // The updater's marked close request. Without it the staged updater has no
    // way to ask this process to get out of the way, and a portable update stops
    // at CloseApp with appWontClose -- which is precisely what happened after the
    // Qt Quick cutover dropped the Widgets shell's nativeEvent handler.
    std::unique_ptr<QAbstractNativeEventFilter> updater_handoff_filter_;
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
    exosnap::engine::RecordingDiagnosticsSnapshot last_completed_snapshot_;
    double peak_av_drift_ms_ = 0.0;
    bool av_drift_ever_available_ = false;
    float preflight_system_rms_ = 0.0f;
    float preflight_app_rms_ = 0.0f;
    float preflight_microphone_rms_ = 0.0f;
    QStringList live_toggleable_sources_;
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
    // QCR-804. Lives on the Qt main thread and is driven only from the diagnostics
    // callback, which the coordinator already marshals here.
    diagnostics::WindowCaptureStallMonitor capture_stall_monitor_;
    // Sequence of the standing capture-stall toast while it is up, 0 when none is.
    // The hub keeps its own permanent record either way.
    uint64_t capture_stall_toast_sequence_ = 0;
    // ADR 0046. Same threading and same driver as capture_stall_monitor_.
    diagnostics::AudioSourceDegradationMonitor audio_degradation_monitor_;
    // Sequence of the standing audio-degradation toast while it is up, 0 when
    // none is. The hub keeps its own permanent record either way.
    uint64_t audio_degraded_toast_sequence_ = 0;
    StartAdmission last_start_admission_ = StartAdmission::RefusedByState;
    // The selection Diagnostics currently holds. Same reason as the two below:
    // pushing it re-runs the recommendation checklist, so only a real change may.
    std::optional<exosnap::engine::CaptureTarget> pushed_selected_target_;
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
    // Set when the preset store had to repair fields while loading. Raised as a
    // notification once the manager exists, never swallowed.
    bool preset_store_repaired_ = false;
    // First camera reported by the last webcam enumeration, used to resolve an
    // unpinned webcam device. Empty when no camera is attached.
    std::string default_webcam_device_id_;
    // The dirty field named in the last [preset] log line. refreshPresetState()
    // runs on every config mirror sync, so without this the same line would be
    // written dozens of times per edit.
    std::string last_logged_dirty_field_;
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
    // Declared before engine_ for the same reason window_geometry_ is: it
    // outlives the window it acts on.
    //
    // Constructed unconditionally, because Main.qml binds to it as a required
    // property. Whether an icon actually appears is TrayAdapter::active(), which
    // is false when the platform has no notification area or a harness mode
    // suppressed it.
    TrayAdapter tray_adapter_;
    // The renderer behind image://exosnap-shell/... Owned by the engine once
    // registered, so this only records that the registration happened.
    bool shell_icon_provider_registered_ = false;
    // The renderer behind the WINDOW icon. Separate from the engine-owned image
    // provider because this side needs QIcons rather than URLs -- WM_SETICON does
    // not go through Qt Quick.
    ui::brand::ShellIconCache window_icon_cache_;
    // The last state written to the shell surfaces, so the log line that stands
    // in for a developer looking at the screen fires on transitions and not on
    // every metrics tick.
    ShellIconState shell_icon_state_ = ShellIconState::Idle;
    // The heartbeat frame the window icon currently shows. Tracked separately
    // from the state because the entry beat changes the icon without changing the
    // state, and -1 so the first publish always writes through.
    int shell_mark_frame_ = -1;
    // The shell's view of the session, and the two clocks it needs: the recording
    // heartbeat and the bounded Saved dwell. Every shell surface reads this one
    // object rather than the recording state directly.
    ShellPresenceAdapter shell_presence_;
    // The Windows taskbar button. Present whether or not a tray is: a session
    // with the notification area disabled still has a taskbar.
    TaskbarPresence taskbar_presence_;
    // The engine owns the window; this only observes it. A QPointer because the
    // engine can destroy the window while this object is still alive, and every
    // tray action would otherwise act on a dangling pointer.
    QPointer<QQuickWindow> root_window_;
    QQmlApplicationEngine engine_;

    // Declared last so it is destroyed FIRST: its destructor waits for the
    // in-flight hardware capability query. The probe captures `this`, and its
    // DXGI/encoder enumeration must not still be running once the members above
    // — or Qt's own statics — are gone.
    QThreadPool capability_probe_pool_;
};

} // namespace exosnap::quick
