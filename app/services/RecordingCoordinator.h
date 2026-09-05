#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <QImage>
#include <QThreadPool>

#include <capability/audio_ui_state.h>
#include <capability/capability_set.h>
#include <capability/resolver.h>
#include <capability/runtime_snapshot.h>
#include <capability/translation.h>
#include <capability/user_config.h>
#include <exosnap/engine/mp4_remuxer.h>
#include <exosnap/engine/recorder_session.h>

#include "../diagnostics/DiskSpaceProvider.h"
#include "../diagnostics/SessionLedger.h"
#include "../diagnostics/SessionReport.h"
#include "../diagnostics/WindowTargetFacts.h"
#include "../models/FilenameBuilder.h"
#include "../models/OutputPathValidator.h"
#include "../models/OutputSettingsModel.h"
#include "../models/RecordingMarker.h"
#include "../models/VideoSettingsModel.h"
#include "../models/WebcamSettings.h"
#include "../settings/RecoveryManifestStore.h"
#include "../viewmodels/RecordViewModel.h"
#include "RecordingAdmission.h"
#include "WebcamService.h"

namespace exosnap::engine {
class MicMeterService;
class LoopbackMeterService;
} // namespace exosnap::engine

namespace exosnap {

// One recording's frozen session ledger, on its way from the Qt main thread that
// closes it to the session report that carries it.
//
// Shared rather than owned by either end. The freeze arrives through a Qt
// connection whose context object outlives the coordinator, and the report is
// written from a queued call that may run after the coordinator is gone; a sink
// both sides hold by shared_ptr means neither holds a pointer to the other.
class SessionLedgerSink {
  public:
    void Set(std::vector<diagnostics::LedgerEntry> ledger);
    void Clear();
    [[nodiscard]] std::vector<diagnostics::LedgerEntry> Get() const;

  private:
    mutable std::mutex mutex_;
    std::vector<diagnostics::LedgerEntry> ledger_;
};

class RecordingCoordinator {
  public:
    using StateChangedCallback = std::function<void(UiRecordingState)>;
    using StatsUpdatedCallback = std::function<void(const exosnap::engine::SessionStats&)>;
    using DiagnosticsUpdatedCallback = std::function<void(const exosnap::engine::RecordingDiagnosticsSnapshot&)>;
    using ResultReadyCallback = std::function<void(const UiRecordingResult&)>;
    using MicMeterUpdatedCallback = std::function<void(float rms_linear)>;
    using SysMeterUpdatedCallback = std::function<void(float rms_linear)>;
    using AppMeterUpdatedCallback = std::function<void(float rms_linear)>;
    // Fired at ~30 Hz during recording; per-track RMS indexed by AudioThread track_id.
    using RecordingMeterCallback = std::function<void(const std::array<float, 3>&)>;
    // Capture frame result: (success, saved_path_or_empty, error_message_or_empty)
    using FrameCapturedCallback = std::function<void(bool success, const QString& path, const QString& error)>;
    // Transient split feedback for the UI: (accepted, message). On accept the
    // message is e.g. "Started segment 2"; on reject it explains why. Fired on the
    // Qt main thread.
    using SplitFeedbackCallback = std::function<void(bool accepted, const QString& message)>;
    // Remux progress: called on the Qt main thread with fraction in [0,1] during
    // the MP4 remux phase (ADR-0014). fraction is -1 when remux starts (indeterminate).
    using RemuxProgressCallback = std::function<void(float fraction)>;

    RecordingCoordinator();
    ~RecordingCoordinator();

    RecordingCoordinator(const RecordingCoordinator&) = delete;
    RecordingCoordinator& operator=(const RecordingCoordinator&) = delete;

    // Inject the recovery manifest store. Must be called before StartRecording.
    // The store must outlive this object. When nullptr the manifest integration
    // is silently disabled (tests that do not care about recovery can omit this).
    void SetRecoveryManifestStore(RecoveryManifestStore* store);
    [[nodiscard]] RecoveryManifestStore* GetRecoveryManifestStore() const noexcept;

    // Fired on the Qt main thread when a recovery-manifest write did not reach
    // disk. The recording is unaffected — only the crash-recovery safety net is
    // gone for this session, which the user must be told about (canon: a failed
    // local write is a reported failure, not a silent one). `detail` names which
    // step failed. Recording continues; see the manifest-persistence note in
    // PrepareAndRecordThreadProc for why this never blocks or aborts a start.
    using RecoveryProtectionLostCallback = std::function<void(const QString& detail)>;
    void SetRecoveryProtectionLostCallback(RecoveryProtectionLostCallback cb);

    // Supplies the exclusive-fullscreen verdict for a WINDOW capture target — the
    // SAME verdict the diagnostics card reports, produced by the same resolver
    // (ClassifyWindowShape + CombineFullscreenEvidence over the window facts and
    // the measured capture-hub evidence). The coordinator deliberately does not
    // re-derive it: one problem, one resolution.
    //
    // Read on the UI thread inside StartRecording and snapshotted into the prepare
    // context, because the evidence producer owns a capture hub bound to the
    // apartment that created it and the worker must never reach for it.
    //
    // Unset — the default — yields ExclusiveEvidence::None, and the admission gate
    // then never blocks on exclusive fullscreen: nothing measured, nothing proven.
    using WindowExclusiveEvidenceProvider =
        std::function<diagnostics::ExclusiveEvidence(const exosnap::engine::CaptureTarget&)>;
    void SetWindowExclusiveEvidenceProvider(WindowExclusiveEvidenceProvider provider);

    // QCR-804. Records that the mid-recording window-capture stall monitor
    // confirmed and reported one stall episode for the session in flight, so the
    // on-disk session report can still say it happened after the recording ended.
    //
    // Called from the Qt main thread (the monitor is driven by the diagnostics
    // callback); read from the recording thread when the report is written. An
    // atomic counter is the whole handoff — deliberately no callback, no shared
    // state, and nothing the coordinator can dereference back into the UI.
    void NoteWindowCaptureStall() noexcept;
    [[nodiscard]] uint32_t WindowCaptureStallEpisodes() const noexcept;

    // Where the diagnostics side puts the frozen session ledger of the recording
    // that just ended, and where the session report reads it from.
    //
    // Pushed rather than pulled, for the same reason NoteWindowCaptureStall
    // exists: the ledger is owned by the Qt main thread, and a provider callback
    // would have the recording thread copy a std::vector that thread may be
    // appending to. Cleared at the start of every recording, so a session that
    // ends before its own freeze arrives reports no ledger rather than the
    // previous one's.
    [[nodiscard]] std::shared_ptr<SessionLedgerSink> FrozenLedgerSink() const;

    // ADR-0015: armed-from-recovery state.
    // Enter the armed-from-recovery (paused) state for the given candidate.
    // The artefact is repaired/remuxed in the background as the first slice;
    // resume starts the next slice (same session naming). If another candidate
    // is already armed, it is finalized first (multi-recovery replacement rule).
    // `audio_ui_state` and `preset` carry forward the session configuration.
    // The caller is responsible for kicking off the background remux of the
    // artefact (via RecoveryService::Finish); this only records the armed state.
    //
    // Returns false if the coordinator is currently recording (not in Ready /
    // Completed / Failed / ArmedFromRecovery state).
    struct RecoverySessionInfo {
        RecoveryManifestEntry manifest_entry;  // the candidate being continued
        exosnap::engine::CaptureTarget target; // capture target to resume on
        bool target_valid = false;             // false when target needs re-selection
    };
    bool ArmFromRecovery(const RecoverySessionInfo& info);

    // Finalize the currently armed recovery session: its slices become a finished
    // recording (background remux already in flight or completed). Transitions back
    // to Ready / ArmedFromRecovery (for the new candidate) or Ready.
    // No-op when not in ArmedFromRecovery state.
    void FinalizeArmedRecovery();

    // True when in the ArmedFromRecovery state.
    [[nodiscard]] bool IsArmedFromRecovery() const noexcept;

    // Returns the currently armed recovery session info (valid only when
    // IsArmedFromRecovery() returns true).
    [[nodiscard]] const RecoverySessionInfo& ArmedRecoverySession() const noexcept;

    // Inject a disk-space provider for the runtime low-disk guard.
    // When nullptr (the default) a Win32DiskSpaceProvider is used automatically.
    // Tests inject a stub to simulate arbitrary free-space conditions.
    // Must be called before StartRecording; safe to call after construction.
    void SetDiskSpaceProvider(diagnostics::IDiskSpaceProvider* provider);

    // Supplies the current per-display HDR facts. When unset, the real DXGI query runs.
    // Tests inject a stub to simulate a display whose HDR state changed after startup.
    using DisplayFactsProvider = std::function<std::vector<capability::DisplayHdrFacts>()>;
    void SetDisplayFactsProvider(DisplayFactsProvider provider);

    // Re-reads the per-display HDR facts and replaces the startup snapshot's copy.
    //
    // The startup capability query runs once. Toggling Windows HDR or Advanced Color
    // changes neither screen geometry nor adapter topology, so nothing notices, and both
    // the rec.hdr.h264 blocker and the HDR10 encode reconcile then work from stale facts.
    // StartRecording calls this so the metadata it commits describes the display as it is
    // now, not as it was at launch.
    void RefreshDisplayFacts();

    // The display facts the coordinator currently holds. Exposed for tests.
    [[nodiscard]] const std::vector<capability::DisplayHdrFacts>& DisplayFacts() const;

    // Disk-space stop reason reported via the result when an auto-stop fires.
    // Exposed for tests.
    static const wchar_t* kDiskSpaceStopReason;

    // Validates the coordinator's own already-applied resolved_user_config_ (set by
    // SetOutputSettings/SetVideoSettings) against the freshly probed caps — the same
    // validation RevalidateCapabilities() runs on every later settings change. Callers
    // must not pre-compute a validation against a different config and expect it to
    // stick: OnCapabilitiesReady always re-derives it from resolved_user_config_, so
    // whatever format the caller wants recorded must be applied via SetOutputSettings/
    // SetVideoSettings BEFORE calling this.
    void OnCapabilitiesReady(const exosnap::capability::CapabilitySet& caps);
    void OnCapabilityFailure(std::wstring message);
    void RevalidateCapabilities();
    void ApplyOutputFolderValidation(FolderValidationResult result);

    std::vector<exosnap::engine::CaptureTarget> EnumerateTargets();
    bool StartRecording(const exosnap::engine::CaptureTarget& target, const capability::AudioUiState& audio_ui_state,
                        std::optional<exosnap::engine::CaptureRegion> crop_region = std::nullopt);

    // Webcam overlay
    // Mute or unmute one audio source kind while a recording runs. No-op when
    // no session is in flight: between recordings the source rows are the truth
    // and a new session starts from them.
    void SetAudioSourceMuted(exosnap::engine::AudioSourceKind kind, bool muted);

    void SetWebcamSettings(const WebcamSettings& settings);
    void SetWebcamFrameCallback(WebcamService::FrameCallback cb);
    void SetWebcamFrameCallback(QObject* receiver, WebcamService::FrameCallback cb);
    // Receiver-scoped open-reader status transitions (see WebcamService::
    // SetStatusCallback): forwarded verbatim so the Record dock can flag a webcam
    // that cannot be opened. Dropped if the receiver dies.
    void SetWebcamStatusCallback(QObject* receiver, WebcamService::StatusCallback cb);
    // Request that the shared webcam capture run while idle (not recording) so the
    // Record preview can show a live PiP.  Recording always owns the device; this
    // only affects the Ready/idle state.  Idempotent and safe to call repeatedly.
    void SetWebcamPreviewActive(bool active);
    // Same, but for the Settings webcam panel. The one shared capture runs while ANY
    // consumer (recording, Record preview, or the Settings preview) wants it, so the
    // Settings panel shows the exact same frames without opening a second reader — no
    // device-lock fight, and it works while recording. Idempotent.
    void SetWebcamSettingsPreviewActive(bool active);
    void StopRecording();
    // Cooperatively cancel an in-progress Preparing phase (device setup running on
    // the preparation worker thread). Safe to call from the Qt main thread.
    //   - While Preparing (prepare_in_flight_ && !is_recording_): requests a
    //     cooperative cancel; the worker unwinds to Ready at its next checkpoint.
    //   - Once the session has committed to recording (is_recording_): falls through
    //     to StopRecording() so a cancel press in the narrow commit window is never
    //     dropped.
    //   - No-op when nothing is preparing or recording.
    // A running blocking step (MF camera open, ≤750 ms lease wait) is not interrupted;
    // the cancel takes effect after it returns.
    void CancelPreparing();
    void PauseRecording();
    void ResumeRecording();

    // Leaves a finished or failed run behind and returns the transport to its
    // idle arrangement. Nothing is undone: the recording stays exactly where it
    // was written, and the manifest and the result are untouched. Until this
    // existed, the only way out of Completed was to start the next recording,
    // which made "I am done looking at this" and "record again" the same button.
    //
    // A no-op in every other state, so a stray call can never interrupt a run.
    void DismissResult();

    // Typed split command path (SPLIT-RECORDING-R1). Routes the manual button and
    // the global hotkey through the exact same entry point. Accepted only while a
    // session is active (Recording or Paused) and no split transition is already
    // in flight; otherwise rejected honestly (logged, no-op). Returns true if the
    // request was accepted and forwarded to the engine.
    bool RequestSplit(exosnap::engine::SplitTriggerSource source);

    // True while a split boundary is pending (request forwarded, new segment not
    // yet started). Used to gate the UI so concurrent requests are coalesced.
    [[nodiscard]] bool IsSplitPending() const noexcept;

    // Configure automatic/manual split policy applied at the next StartRecording.
    void SetSplitSettings(const exosnap::engine::RecordingSplitSettings& settings);
    [[nodiscard]] exosnap::engine::RecordingSplitSettings SplitSettings() const noexcept;

    void AddMarker(RecordingMarkerType type = RecordingMarkerType::General);
    // A SNAPSHOT, deliberately by value. This used to return `const&` while taking
    // markers_mutex_ for the duration of the return statement -- so the lock was
    // released before the caller had read a single element, and the reference it was
    // handed aliased a vector that AddMarker() can push_back into. One reallocation
    // from the control channel or a hotkey while the caller iterates and the
    // reference dangles; the lock made the signature look synchronised without
    // synchronising anything a caller does. Every other reader in this class already
    // copies under the lock (WriteMarkerSidecar, the result path); this one now does
    // too. Not noexcept: the copy allocates.
    [[nodiscard]] std::vector<RecordingMarker> Markers() const;
    [[nodiscard]] std::filesystem::path MarkerSidecarPath() const;
    bool StartMicMeter(std::optional<std::string> device_id, exosnap::engine::MicChannelMode channel_mode);
    void StopMicMeter();
    [[nodiscard]] bool IsMicMeterRunning() const noexcept;

    bool StartSysMeter();
    void StopSysMeter();
    [[nodiscard]] bool IsSysMeterRunning() const noexcept;

    bool StartAppMeter(uint32_t target_pid);
    void StopAppMeter();
    [[nodiscard]] bool IsAppMeterRunning() const noexcept;

    UiRecordingState State() const noexcept;
    const std::wstring& CapabilityStatusText() const;
    std::wstring ResolvedVideoCodecLabel() const;
    std::filesystem::path CurrentOutputPath() const;
    void SetOutputSettings(const OutputSettingsModel& settings);
    void SetVideoSettings(const VideoSettingsModel& settings);
    void SetOutputTargetContext(const FilenameTargetContext& context);

    // Returns the recording output directory in effect at the moment of the call.
    // When EXOSNAP_OUTPUT_DIR is set to a non-empty value it overrides the configured
    // output_settings_.output_folder without modifying persisted settings (tooling /
    // CI isolation; mirrors EXOSNAP_CONFIG_DIR in ConfigPaths.h).
    [[nodiscard]] std::filesystem::path EffectiveOutputFolder() const;

    void SetStateChangedCallback(StateChangedCallback cb);
    void SetStatsUpdatedCallback(StatsUpdatedCallback cb);
    void SetDiagnosticsCallback(DiagnosticsUpdatedCallback cb);
    // Read-back of the most recent accepted diagnostics snapshot for the current or
    // just-finished session. Returns false when none has been accepted yet.
    //
    // Exists because SetDiagnosticsCallback is single-occupancy: a second registration
    // silently displaces the first, which is how dropped_frames and av_drift once went
    // to zero for a whole session. A reader that only needs the terminal numbers — the
    // benchmark harness, chiefly — must not have to take the one callback slot away
    // from the frontend that owns it.
    [[nodiscard]] bool LastDiagnosticsSnapshot(exosnap::engine::RecordingDiagnosticsSnapshot* out);

    // Read-back of the RecorderConfig the most recent StartRecording actually
    // handed the engine, captured after session_.Validate() accepted it. Returns
    // false when no session has been prepared in this process yet.
    //
    // Exists for the frontend A/B benchmark: comparing the CLI strings two runs
    // were launched with proves nothing, because a frontend can commit settings of
    // its own on the way to StartRecording (the Qt Quick path did exactly that,
    // seeding OutputSettingsModel::Defaults() while the Widgets path used the
    // CLI-committed values, so the two sides silently recorded different formats).
    // This is the committed truth, not the requested one.
    [[nodiscard]] bool LastCommittedRecorderConfig(exosnap::engine::RecorderConfig* out) const;
    void SetResultReadyCallback(ResultReadyCallback cb);
    void SetMicMeterUpdatedCallback(MicMeterUpdatedCallback cb);
    void SetSysMeterUpdatedCallback(SysMeterUpdatedCallback cb);
    void SetAppMeterUpdatedCallback(AppMeterUpdatedCallback cb);
    void SetRecordingMeterCallback(RecordingMeterCallback cb);
    void SetFrameCapturedCallback(FrameCapturedCallback cb);
    void SetSplitFeedbackCallback(SplitFeedbackCallback cb);
    void SetRemuxProgressCallback(RemuxProgressCallback cb);

    // Register a callback fired (from the engine's video thread) once the shared
    // WYSIWYG preview texture is ready. nt_handle is a Windows HANDLE passed as
    // void*; ownership transfers to the callee (open then CloseHandle). `tap`
    // names the display transform the consumer must apply before drawing
    // (recorder_core/preview_tap.h — a native HDR10 session shares FP16 scRGB,
    // which the preview tone-maps). Fires for every session except the
    // already-PQ 10-bit native sub-path, and only when the callback is set before
    // StartRecording. The callback must return fast and must not make D3D calls
    // on the calling thread.
    using PreviewSharedHandleReadyCallback =
        std::function<void(void* nt_handle, uint32_t width, uint32_t height, exosnap::engine::PreviewTapDesc tap)>;
    void SetPreviewSharedHandleReadyCallback(PreviewSharedHandleReadyCallback cb);

    // Register the per-frame publish edge for the same WYSIWYG tap
    // (exosnap::engine::PreviewFramePublishedCallback). Fires from the engine's
    // video thread after each frame that actually reached the shared texture,
    // so a preview consumer can schedule one redraw per new frame instead of
    // polling. No payload; same fast-return / no-D3D contract as above.
    using PreviewFramePublishedCallback = std::function<void()>;
    void SetPreviewFramePublishedCallback(PreviewFramePublishedCallback cb);

    // Invoked on the recording preparation worker thread (NOT the UI thread),
    // immediately before the engine opens its capture — after every validation,
    // guard, and cancellation checkpoint, so it never fires for a start that is
    // rejected or cancelled. The idle preview's DXGI capture hub releases its
    // duplication here (an output can only be duplicated once per process, and
    // the engine is about to open its own). The hook must be thread-safe and must
    // block until the release has actually happened. The lease is returned
    // page-side when the recording reaches Ready/Completed/Failed, alongside the
    // pushed-source revert.
    using PreviewCaptureReleaseHook = std::function<void()>;
    void SetPreviewCaptureReleaseHook(PreviewCaptureReleaseHook hook);

    // Request cooperative cancellation of any in-progress remux job.
    // Safe to call from the Qt main thread at any time; no-op if no remux is running.
    void CancelRemux();

    // True while the background remux job is running (Saving state).
    [[nodiscard]] bool IsRemuxing() const noexcept;

    // Callback shape for a Ready-state frame request: fires with the readback
    // result (BGRA8, tightly packed) or ok=false with a reason.
    using ReadyFrameCallback =
        std::function<void(bool ok, uint32_t width, uint32_t height, std::vector<uint8_t> bgra, QString error)>;
    // Inject the async requester for the live Ready-state preview frame (the
    // actual DXGI-rendered WYSIWYG content, not a Qt-side copy — Ready-state
    // preview is a native child HWND Qt never sees pixels for). Must be safe to
    // call from the UI thread; the callback it eventually invokes may fire on a
    // different thread (see PreviewSurface::requestDxgiSnapshot).
    using ReadyFrameRequester = std::function<void(ReadyFrameCallback)>;
    void SetReadyFrameRequester(ReadyFrameRequester requester);

    // Request a frame capture. Saves a PNG to the active output folder.
    // Valid in Ready, Recording, and Paused states.
    // Fires the FrameCapturedCallback on the Qt main thread when complete.
    void CaptureFrame();

    // ── Split-remux lifecycle test seams (MP4-SPLIT-REMUX-R1 / QCR-107) ────────
    // These drive the exact production scheduling / reaping / drain path with a
    // stubbed remux body, so the lifecycle can be pinned without a real MKV, a
    // muxer or a GPU. No production code calls them.
    //
    // `work` runs on the job's own thread and returns whether the remux succeeded;
    // the completion bookkeeping around it is the production one.
    void ScheduleSegmentRemuxForTest(std::filesystem::path transient_mkv, std::filesystem::path output_mp4,
                                     QString manifest_id, std::function<bool()> work);
    [[nodiscard]] size_t SegmentRemuxJobCountForTest() const;
    [[nodiscard]] uint64_t PendingRemuxReserveBytesForTest() const;
    void ReapFinishedSegmentRemuxJobsForTest();
    bool DrainSegmentRemuxJobsForTest(bool cancel);

    // ── Session-report handoff test seams ─────────────────────────────────────
    // The report carries what the diagnostics side froze, and the two arrive on
    // different threads. These drive the exact production posting path with no
    // engine behind it, so the ordering can be pinned without a real recording.
    // No production code calls them.
    //
    // Mints the session id the report is named after and clears the ledger sink,
    // the two things StartRecording does for the report.
    void BeginReportSessionForTest(QString recording_session_id);
    void PostDiagnosticsForTest(exosnap::engine::RecordingDiagnosticsSnapshot snapshot);
    void PostResultForTest(UiRecordingResult result);

  private:
    // Shared tail of CaptureFrame()'s Recording/Paused and Ready paths: converts
    // a raw BGRA readback (or a failure) into a saved PNG + FrameCapturedCallback
    // notification. Static and parameter-only (no captured `this`) so it stays
    // safe to invoke from a background-thread-fired snapshot callback — matches
    // this class's existing convention of never capturing `this` into a callback
    // that fires off a worker thread it does not own the lifetime of.
    // `pool` is the only exception to that rule and is passed explicitly rather
    // than reached through `this`: the PNG encode must not run on the caller's
    // thread (VideoThread / the DXGI preview render thread would drop frames),
    // and it must have an owner that waits for it — see snapshot_pool_.
    // log_context_suffix distinguishes the two call sites in the app log only
    // (e.g. " (Ready)").
    static void WriteSnapshotAndNotify(QThreadPool& pool, FrameCapturedCallback cb, const std::wstring& folder,
                                       bool has_target_context, const FilenameTargetContext& target_context,
                                       const QString& log_context_suffix, bool ok, uint32_t width, uint32_t height,
                                       std::vector<uint8_t> bgra, const QString& error);

    // Immutable snapshot of every input and config model StartRecording's device
    // work reads, copied by value on the UI thread before the preparation worker
    // starts. "Thin gate, fat worker": the worker reads exclusively from this
    // snapshot — never a live coordinator member — so the Settings setters remain
    // free to mutate output_settings_/video_settings_/etc. during Preparing without
    // tearing a std::wstring/std::filesystem::path across the thread boundary.
    struct PrepareContext {
        exosnap::engine::CaptureTarget target;
        capability::AudioUiState audio_ui_state;
        std::optional<exosnap::engine::CaptureRegion> crop_region;
        OutputSettingsModel output_settings;
        exosnap::engine::RecordingSplitSettings split_settings;
        VideoSettingsModel video_settings;
        WebcamSettings webcam_settings;
        capability::UserRecorderConfig resolved_user_config;
        FilenameTargetContext output_target_context;
        bool has_output_target_context = false;
        capability::CapabilitySet caps;
        // Measured exclusive-fullscreen verdict for a window target, resolved on
        // the UI thread (the evidence producer is not worker-thread-safe) and
        // carried here for the worker's admission gate. None for monitor targets.
        diagnostics::ExclusiveEvidence window_exclusive_evidence = diagnostics::ExclusiveEvidence::None;
    };

    // Runs the device-setup ("Preparing") phase and, on success, the recording
    // itself — all on the recording thread, so the UI thread never blocks on the
    // free-space query, filesystem work, DXGI display-facts refresh, webcam open,
    // or the capture-lease handshake. Reads only from `ctx`.
    void PrepareAndRecordThreadProc(const PrepareContext& ctx);
    void RecordingThreadProc(const exosnap::engine::RecorderConfig& config, const std::filesystem::path& output_path);
    // (Re)start or stop the shared webcam capture based on enabled/recording/preview state.
    void SyncWebcamService(bool force_restart);
    void PostStateChange(UiRecordingState new_state);
    // Stamp the configured container/codecs onto a result before posting it.
    // The error dialog shows this format context; a result that omits it falls
    // back to the struct defaults (WebM · AV1 · Opus) and contradicts the
    // Record footer and the output filename.
    void FillResultFormat(UiRecordingResult& result, const PrepareContext& ctx) const;
    void PostResult(UiRecordingResult result);
    // Everything the on-disk session-<id>.json report needs, copied out of the
    // coordinator so the write can happen on another thread later without
    // reaching back into it.
    struct SessionReportJob {
        QString reports_dir;
        diagnostics::SessionReportInputs inputs;
        std::shared_ptr<SessionLedgerSink> ledger_sink;
    };
    // Gather the report for a finished (or failed) recording from the result plus
    // the stashed final snapshot. Empty when there is no recording session id to
    // name the report after, or no log directory to put it beside.
    [[nodiscard]] std::optional<SessionReportJob> BuildSessionReportJob(const UiRecordingResult& result);
    // Write a gathered report. Best-effort: a write failure is logged and never
    // blocks posting the result. Static, and takes everything by value, because it
    // runs from a queued call that may outlive the coordinator.
    static void RunSessionReportJob(SessionReportJob job);
    void PostStats(exosnap::engine::SessionStats stats);
    void PostDiagnostics(exosnap::engine::RecordingDiagnosticsSnapshot snapshot);
    // Emit a single Initializing diagnostics snapshot so the Diagnostics page shows an
    // "initializing" state during preparation, before the engine produces live data.
    void EmitInitializingDiagnostics();
    void PostMicMeter(float rms_linear);
    void PostSysMeter(float rms_linear);
    void PostAppMeter(float rms_linear);
    void PostRecordingMeter(std::array<float, 3> per_track_rms);

    // Builds the output path from the prepare snapshot (reads only from ctx). Runs
    // on the preparation worker thread.
    std::filesystem::path GenerateOutputPath(const PrepareContext& ctx) const;
    // Apply the EXOSNAP_OUTPUT_DIR override to a configured output folder. The
    // no-arg EffectiveOutputFolder() delegates here with output_settings_; the
    // worker calls it with the snapshot's folder.
    static std::filesystem::path EffectiveOutputFolderFor(const OutputSettingsModel& settings);
    void WriteMarkerSidecar();
    // Write a per-segment marker sidecar adjacent to `segment_media_path`,
    // containing only markers whose session time falls in this segment, rebased to
    // segment-local time. No sidecar is written when the segment has zero markers.
    void WriteSegmentMarkerSidecar(const exosnap::engine::CompletedSegment& segment);
    static std::wstring FormatHResult(int32_t hr);
    static std::wstring FormatErrorPhase(exosnap::engine::ErrorPhase phase);

    // Recovery manifest store (nullable — injected by MainWindow via SetRecoveryManifestStore).
    RecoveryManifestStore* recovery_manifest_store_ = nullptr;
    // UUID of the manifest entry for the currently active or most recent recording.
    // Empty when no session is in flight.
    //
    // INVARIANT (QCR-103): non-empty ONLY while a manifest entry with that id is
    // known to have reached disk. A failed Add leaves this empty, because every
    // later step reads it as "this session is recovery-protected" — cleanup,
    // finalize and removal all key off it, and a phantom id would make the code
    // believe it protected a session it never wrote.
    //
    // GUARDED BY segment_remux_mutex_ (QCR-106). That mutex owns the whole
    // manifest-id handoff between the mux worker thread (OnSegmentCompleted mints
    // the next segment's entry) and the recording thread (which consumes it after
    // Record() returns) — pending_segment_manifest_id_ was already documented that
    // way; this field is the other half of the same transaction and was only
    // partially locked. Access it through CurrentManifestId()/SetCurrentManifestId()
    // /TakeCurrentManifestId(), or inside an existing segment_remux_mutex_ block.
    QString current_manifest_id_;

    // Locked accessors for current_manifest_id_. None of them calls the recovery
    // store: an id is taken under the lock, the store call happens without it.
    [[nodiscard]] QString CurrentManifestId() const;
    void SetCurrentManifestId(QString id);
    // Reads and clears in one critical section, so "this session is
    // recovery-protected" cannot be observed by two threads at once.
    [[nodiscard]] QString TakeCurrentManifestId();

    RecoveryProtectionLostCallback on_recovery_protection_lost_;
    WindowExclusiveEvidenceProvider window_exclusive_evidence_provider_;
    // QCR-804: reported window-capture stalls for the session in flight. Written
    // from the UI thread, read from the recording thread — see NoteWindowCaptureStall.
    std::atomic<uint32_t> window_capture_stall_episodes_{0};
    // Written from the Qt main thread when the ledger freezes, read from the Qt
    // main thread when the report is written -- see FrozenLedgerSink.
    const std::shared_ptr<SessionLedgerSink> frozen_ledger_sink_ = std::make_shared<SessionLedgerSink>();
    // Posts the recovery-protection-lost notice onto the Qt main thread and logs
    // it. Safe from any thread.
    void PostRecoveryProtectionLost(QString detail);
    // Resolves the exclusive-fullscreen verdict for a window target. UI thread
    // only — it reads the injected evidence provider and Win32 window state.
    [[nodiscard]] diagnostics::ExclusiveEvidence
    ResolveWindowExclusiveEvidence(const exosnap::engine::CaptureTarget& target) const;

    // Stable per-recording session id, minted at StartRecording independent of the
    // (nullable) recovery store and NOT cleared before PostResult. This — not
    // current_manifest_id_, which is store-gated, cleared before PostResult, and
    // re-minted per split segment — names the on-disk session report and stays
    // constant across a split recording. Overwritten only at the next StartRecording.
    QString recording_session_id_;

    // ADR-0015: armed-from-recovery state.
    bool is_armed_from_recovery_ = false;
    RecoverySessionInfo armed_recovery_session_{};
    // Placeholder for future: slice count for the recovery session.
    int armed_recovery_slice_count_ = 0;

    // Low-disk guard (LOW-DISK-GUARD-R1)
    // Nullable injected provider; fallback to the Win32 implementation when nullptr.
    // Refreshes, then hands out the facts. The HDR reconcile reads through this rather
    // than through the startup snapshot, so the refresh cannot be left out by accident.
    const std::vector<capability::DisplayHdrFacts>& RefreshedDisplayFacts();

    // Nullable; when unset RefreshDisplayFacts() queries DXGI directly.
    DisplayFactsProvider display_facts_provider_;
    diagnostics::IDiskSpaceProvider* disk_space_provider_ = nullptr;
    // Owned Win32 fallback; allocated lazily on first StartRecording if no provider was injected.
    std::unique_ptr<diagnostics::Win32DiskSpaceProvider> default_disk_space_provider_;
    // Background thread polling free space during recording.
    std::jthread disk_monitor_thread_;
    // Set to true when the disk-monitor auto-stop fires to suppress duplicate stops.
    std::atomic<bool> disk_stop_triggered_{false};
    // True when the active session targets MP4 (requires remux reserve in threshold).
    bool session_is_mp4_ = false;
    // Path of the transient MKV for the active MP4 session (used to size remux reserve).
    std::filesystem::path session_transient_mkv_;

    // The monitor is bound to the recording it was started for: a verdict it
    // reaches after that recording ended (its query can outlast the session) is
    // dropped rather than applied to the recording running by then.
    void StartDiskMonitor(const std::filesystem::path& output_folder, bool is_mp4,
                          const std::filesystem::path& transient_mkv, exosnap::engine::RecordRequestId request);
    void StopDiskMonitor();
    // Called by the monitor thread when the threshold is crossed.
    void OnDiskSpaceLow(exosnap::engine::RecordRequestId request, uint64_t free_bytes, uint64_t threshold_bytes);

    // Captured by OnDiskSpaceLow before calling StopRecording; read in
    // RecordingThreadProc to enrich the UiRecordingResult::error_detail.
    // Protected by the single-fire guarantee of disk_stop_triggered_.
    uint64_t disk_stop_reason_bytes_free_ = 0;
    uint64_t disk_stop_reason_threshold_ = 0;

    exosnap::capability::CapabilitySet caps_{};
    bool has_caps_ = false;
    exosnap::capability::ResolveResult validation_result_;
    exosnap::capability::UserRecorderConfig resolved_user_config_;
    OutputSettingsModel output_settings_;
    VideoSettingsModel video_settings_;
    WebcamSettings webcam_settings_;
    WebcamService webcam_service_;
    // Record preview requested the idle webcam capture (Ready-state live PiP).
    bool webcam_preview_active_ = false;
    bool webcam_settings_preview_active_ = false;
    bool has_output_target_context_ = false;
    FilenameTargetContext output_target_context_;

    exosnap::engine::RecorderSession session_;
    std::unique_ptr<exosnap::engine::MicMeterService> mic_meter_service_;
    std::unique_ptr<exosnap::engine::LoopbackMeterService> sys_meter_service_;
    std::unique_ptr<exosnap::engine::LoopbackMeterService> app_meter_service_;
    std::jthread recording_thread_;
    std::atomic<bool> is_recording_{false};
    std::atomic<bool> is_paused_{false};
    // Guards the Preparing phase (device setup on the recording thread, before the
    // engine opens). Set by the UI-thread gate in StartRecording via a
    // compare-exchange so two starts can never both win; cleared by the worker when
    // it fails, cancels, or commits to recording (is_recording_ then takes over).
    // Also owns the shared webcam device across the prepare boundary (see
    // SyncWebcamService) so a queued Preparing state-callback cannot Stop() the
    // camera the worker is opening.
    std::atomic<bool> prepare_in_flight_{false};
    // Identifies the recording this coordinator is currently starting or running,
    // from the UI-thread gate in StartRecording until Record() returns. Every stop
    // this coordinator issues names it, so a stop belonging to a recording that has
    // already ended can never cut short the next one -- which it did while the
    // engine only knew "some stop happened outside a recording window": the next
    // session began already stopped and reported no frames.
    std::atomic<exosnap::engine::RecordRequestId> record_request_{exosnap::engine::kUnscopedRecordRequest};
    // Cooperative cancel request for the Preparing phase; honored at the worker's
    // checkpoints (after disk/FS, after display facts, after webcam start, and
    // immediately before the recording commits).
    std::atomic<bool> prepare_cancel_requested_{false};

    std::atomic<UiRecordingState> state_{UiRecordingState::LoadingCapabilities};
    std::wstring capability_status_text_;
    FolderValidationResult output_folder_validation_ = FolderValidationResult::Ok;
    // Written by the preparation worker; read on the UI thread and on the mux
    // worker thread. The mutex prevents a torn read of the std::filesystem::path
    // across those boundaries, so EVERY reader goes through CurrentOutputPath()
    // (QCR-106 — the marker sidecar path and OnSegmentCompleted used to read the
    // member directly, which is safe only as long as the engine joins its mux
    // thread inside Record(); the abandoned-worker path it documents means that
    // is not always true).
    mutable std::mutex output_path_mutex_;
    std::filesystem::path current_output_path_;

    // Recording markers
    mutable std::mutex markers_mutex_;
    std::vector<RecordingMarker> markers_;
    double last_elapsed_seconds_ = 0.0;
    uint64_t last_media_time_ns_ = 0; // media-PTS for marker timestamps
    bool markers_limit_reported_ = false;

    // Split recording (SPLIT-RECORDING-R1)
    exosnap::engine::RecordingSplitSettings split_settings_{};
    // True between a forwarded split request and the next segment being reported
    // by the engine. Guards against concurrent/coalesced requests.
    std::atomic<bool> split_pending_{false};
    // Segments accumulated from the engine SegmentCallback (mux worker thread).
    mutable std::mutex segments_mutex_;
    std::vector<exosnap::engine::CompletedSegment> segments_;
    SplitFeedbackCallback on_split_feedback_;
    void PostSplitFeedback(bool accepted, QString message);
    void OnSegmentCompleted(const exosnap::engine::CompletedSegment& segment);

    // ADR-0014: remux-on-stop state.
    std::jthread remux_thread_;
    std::atomic<bool> is_remuxing_{false};
    std::atomic<bool> remux_cancel_requested_{false};
    // Transient MKV path and final MP4 path for the current (or last) remux job.
    std::filesystem::path transient_mkv_path_;
    std::filesystem::path final_mp4_path_;
    // 0.9.0 S1: path of the retained edit master MKV for the last completed session.
    //   - MP4 sessions: the .edit.mkv companion retained after successful remux.
    //   - MKV sessions: empty (the output file IS the master; reported directly in UiRecordingResult).
    //   - Empty on failure, split sessions, or when retention failed.
    std::filesystem::path mkv_master_path_;
    RemuxProgressCallback on_remux_progress_;
    void PostRemuxProgress(float fraction);
    void RunRemuxJob(const std::filesystem::path& transient_mkv, const std::filesystem::path& final_mp4,
                     UiRecordingResult base_result);

    // MP4-SPLIT-REMUX-R1: per-segment background remux jobs.
    //
    // When container == MP4 and split is active, each completed MKV segment is
    // remuxed concurrently in a background thread while recording continues into
    // the next segment.  The final segment is handled the same way; the recording
    // thread waits for all jobs to complete before posting "Saved"/"Failed".
    //
    // Manifest lifecycle per segment (mirrors the single-file flow):
    //   1. Segment N's manifest entry is created before segment N starts writing:
    //      - Segment 0: at StartRecording (uses current_manifest_id_).
    //      - Segment N (N>0): created in OnSegmentCompleted for segment N-1,
    //        stored in pending_segment_manifest_id_, then consumed by the
    //        recording thread when it picks up the segment from the jobs queue.
    //   2. finalized=true is written before the remux starts.
    //   3. The entry is removed only on full remux success.
    //   4. On failure the entry remains so recovery UI can offer re-export.

    // One in-flight or completed segment remux job.
    struct SegmentRemuxJob {
        std::filesystem::path transient_mkv; // input .mkv.tmp
        std::filesystem::path output_mp4;    // desired final .mp4
        QString manifest_id;                 // recovery manifest entry for this segment
        // Background remux thread. jthread (not thread) so that a future
        // early-return destroying this job (or the vector holding it) while the
        // thread is still running joins safely in the destructor instead of
        // calling std::terminate.
        std::jthread thread;
        // The job's explicit lifecycle flag: false = running, true = the thread
        // body has finished and the job is ready to be joined and dropped.
        //
        // thread.joinable() is NOT this flag (QCR-107): a jthread stays joinable
        // from construction until it is joined, so it reads "running" for the whole
        // session for a job that finished seconds after it started. The disk
        // monitor's remux reserve believed exactly that, and a FAILED remux — whose
        // transient MKV is deliberately kept forever as the only trustworthy
        // artefact — was reserved against for the rest of the session.
        std::atomic<bool> completed{false};
        // Written by the thread before it sets `completed`; read after the join.
        bool succeeded = false;
        int av_error_code = 0;
        std::string error_message;
    };

    // Protected by segment_remux_mutex_; appended from OnSegmentCompleted (mux
    // worker thread), reaped opportunistically at each split boundary, and drained
    // from RecordingThreadProc (recording thread) at session end.
    //
    // The mutex also owns current_manifest_id_ and pending_segment_manifest_id_:
    // scheduling a job and handing the next segment's recovery id over are one
    // transaction between the same two threads, and splitting them across two
    // locks would let a job be queued against an id nobody owns yet.
    mutable std::mutex segment_remux_mutex_;
    std::vector<std::unique_ptr<SegmentRemuxJob>> segment_remux_jobs_;
    // Latches when a reaped job had failed. Reaping removes the job before the
    // end-of-session drain can inspect it, so without this a failed intermediate
    // segment would be reported as a fully successful split session. Reset at
    // StartRecording alongside segment_remux_jobs_.
    std::atomic<bool> reaped_segment_remux_failed_{false};

    // Manifest ID for the next segment that has started recording but whose
    // manifest entry was created when the previous segment completed.
    // Written from OnSegmentCompleted (mux worker thread) under segment_remux_mutex_.
    // Consumed by RecordingThreadProc when the session ends.
    QString pending_segment_manifest_id_;

    // Schedule a background remux job for one MKV segment → MP4.
    // Creates a SegmentRemuxJob and starts its thread.  Called on the recording
    // thread (for the final segment) or from OnSegmentCompleted via ScheduleSegmentRemux.
    // `work` performs the actual remux and returns whether it succeeded; the
    // completion bookkeeping around it belongs to this function, so a test body
    // and the real remuxer follow the identical lifecycle.
    void StartSegmentRemuxThread(SegmentRemuxJob& job, std::function<bool()> work);
    // The production remux body for one segment (remux → atomic move → transient
    // cleanup → manifest removal), as handed to StartSegmentRemuxThread.
    bool RunSegmentRemuxWork(const std::filesystem::path& transient_mkv, const std::filesystem::path& output_mp4,
                             const QString& manifest_id);

    // Join and drop every job whose thread has already finished, latching any
    // failure into reaped_segment_remux_failed_. Opportunistic: it never waits on
    // a running job, so it is not a barrier at a split boundary. Called from
    // OnSegmentCompleted, which is the natural rhythm of a split session.
    void ReapFinishedSegmentRemuxJobs();

    // Join all segment remux jobs and return false if any failed (including the
    // ones already reaped). cancel=true requests cancellation of any running remux.
    // Called on the recording thread after Record() returns — the final safety-net
    // join that guarantees no remux thread outlives the session.
    bool DrainSegmentRemuxJobs(bool cancel);

    // Total bytes across all transient MKV files whose remux job is still RUNNING.
    // Used by the disk monitor for a conservative reserve. A finished job — success
    // or failure — is not transient work and must not inflate the reserve.
    // Thread-safe (acquires segment_remux_mutex_).
    uint64_t PendingRemuxReserveBytes() const;

    StateChangedCallback on_state_changed_;
    StatsUpdatedCallback on_stats_updated_;
    DiagnosticsUpdatedCallback on_diagnostics_updated_;
    // Rejects stale-session diagnostics snapshots before they reach the UI.
    exosnap::engine::DiagnosticsSessionGuard diagnostics_guard_;
    std::mutex diagnostics_guard_mutex_;
    // Most recent accepted diagnostics snapshot, stashed so PostResult can write the
    // session report from the end-of-session counters. The stop path emits the final
    // Completed snapshot before PostResult; an error path leaves the last snapshot
    // seen, whose unavailable metrics stay unavailable (never fake zeros). Guarded by
    // diagnostics_guard_mutex_.
    exosnap::engine::RecordingDiagnosticsSnapshot last_snapshot_;
    bool has_last_snapshot_ = false;
    // The RecorderConfig the engine was actually handed, published by the prepare
    // worker once session_.Validate() has accepted it. Written on the recording
    // thread, read on the UI thread, hence its own mutex.
    mutable std::mutex committed_config_mutex_;
    exosnap::engine::RecorderConfig last_committed_config_;
    bool has_last_committed_config_ = false;
    ResultReadyCallback on_result_ready_;
    MicMeterUpdatedCallback on_mic_meter_updated_;
    SysMeterUpdatedCallback on_sys_meter_updated_;
    AppMeterUpdatedCallback on_app_meter_updated_;
    RecordingMeterCallback on_recording_meter_updated_;
    FrameCapturedCallback on_frame_captured_;
    PreviewSharedHandleReadyCallback on_preview_shared_handle_ready_;
    PreviewFramePublishedCallback on_preview_frame_published_;
    PreviewCaptureReleaseHook preview_capture_release_hook_;
    ReadyFrameRequester ready_frame_requester_;

    std::optional<std::string> mic_meter_device_id_;
    exosnap::engine::MicChannelMode mic_meter_channel_mode_ = exosnap::engine::MicChannelMode::Auto;
    bool mic_meter_config_valid_ = false;

    // Runs the screenshot PNG encode + write off the thread that delivered the
    // readback (VideoThread, or the DXGI preview render thread).
    //
    // Declared LAST so it is destroyed FIRST: members are destroyed in reverse
    // declaration order, and the pool's destructor waits for the worker. This
    // used to be a detached std::thread — nobody owned it, nobody waited, and
    // its body touches QDateTime/QDir/QImage/QFile, a function-static
    // QRegularExpression and QCoreApplication::instance(). At process teardown
    // those Qt statics go away underneath it, which is a 0xC0000005 after the
    // last test has already passed. The QCoreApplication::closingDown() check
    // in the worker is a sampled race, not a barrier: it can be false at the
    // check and true one instruction later.
    //
    // Single thread: only one snapshot is ever in flight per source (the engine
    // ignores a second request while one is pending), and serialising the
    // writes also removes the race between two workers running the same
    // collision scan over the same output directory.
    //
    // ~RecordingCoordinator additionally calls waitForDone() explicitly, after
    // the engine threads are joined — see the destructor for why the order
    // matters.
    QThreadPool snapshot_pool_;
};

} // namespace exosnap
