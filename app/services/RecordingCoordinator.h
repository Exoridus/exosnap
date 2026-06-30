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

#include <capability/audio_ui_state.h>
#include <capability/capability_set.h>
#include <capability/resolver.h>
#include <capability/translation.h>
#include <capability/user_config.h>
#include <recorder_core/mp4_remuxer.h>
#include <recorder_core/recorder_session.h>

#include "../diagnostics/DiskSpaceProvider.h"
#include "../models/FilenameBuilder.h"
#include "../models/OutputSettingsModel.h"
#include "../models/RecordingMarker.h"
#include "../models/VideoSettingsModel.h"
#include "../models/WebcamSettings.h"
#include "../settings/RecoveryManifestStore.h"
#include "../viewmodels/RecordViewModel.h"
#include "WebcamService.h"

namespace recorder_core {
class MicMeterService;
class LoopbackMeterService;
} // namespace recorder_core

namespace exosnap {

class RecordingCoordinator {
  public:
    using StateChangedCallback = std::function<void(UiRecordingState)>;
    using StatsUpdatedCallback = std::function<void(const recorder_core::SessionStats&)>;
    using DiagnosticsUpdatedCallback = std::function<void(const recorder_core::RecordingDiagnosticsSnapshot&)>;
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
        RecoveryManifestEntry manifest_entry; // the candidate being continued
        recorder_core::CaptureTarget target;  // capture target to resume on
        bool target_valid = false;            // false when target needs re-selection
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

    // Disk-space stop reason reported via the result when an auto-stop fires.
    // Exposed for tests.
    static const wchar_t* kDiskSpaceStopReason;

    void OnCapabilitiesReady(const exosnap::capability::CapabilitySet& caps,
                             const exosnap::capability::ResolveResult& validation);
    void OnCapabilityFailure(std::wstring message);
    void RevalidateCapabilities();

    std::vector<recorder_core::CaptureTarget> EnumerateTargets();
    bool StartRecording(const recorder_core::CaptureTarget& target, const capability::AudioUiState& audio_ui_state,
                        std::optional<recorder_core::CaptureRegion> crop_region = std::nullopt);

    // Webcam overlay
    void SetWebcamSettings(const WebcamSettings& settings);
    void SetWebcamFrameCallback(WebcamService::FrameCallback cb);
    // Request that the shared webcam capture run while idle (not recording) so the
    // Record preview can show a live PiP.  Recording always owns the device; this
    // only affects the Ready/idle state.  Idempotent and safe to call repeatedly.
    void SetWebcamPreviewActive(bool active);
    void StopRecording();
    void PauseRecording();
    void ResumeRecording();

    // Typed split command path (SPLIT-RECORDING-R1). Routes the manual button and
    // the global hotkey through the exact same entry point. Accepted only while a
    // session is active (Recording or Paused) and no split transition is already
    // in flight; otherwise rejected honestly (logged, no-op). Returns true if the
    // request was accepted and forwarded to the engine.
    bool RequestSplit(recorder_core::SplitTriggerSource source);

    // True while a split boundary is pending (request forwarded, new segment not
    // yet started). Used to gate the UI so concurrent requests are coalesced.
    [[nodiscard]] bool IsSplitPending() const noexcept;

    // Configure automatic/manual split policy applied at the next StartRecording.
    void SetSplitSettings(const recorder_core::RecordingSplitSettings& settings);
    [[nodiscard]] recorder_core::RecordingSplitSettings SplitSettings() const noexcept;

    void AddMarker(RecordingMarkerType type = RecordingMarkerType::General);
    [[nodiscard]] const std::vector<RecordingMarker>& Markers() const noexcept;
    [[nodiscard]] std::filesystem::path MarkerSidecarPath() const;
    bool StartMicMeter(std::optional<std::string> device_id, recorder_core::MicChannelMode channel_mode);
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
    void SetResultReadyCallback(ResultReadyCallback cb);
    void SetMicMeterUpdatedCallback(MicMeterUpdatedCallback cb);
    void SetSysMeterUpdatedCallback(SysMeterUpdatedCallback cb);
    void SetAppMeterUpdatedCallback(AppMeterUpdatedCallback cb);
    void SetRecordingMeterCallback(RecordingMeterCallback cb);
    void SetFrameCapturedCallback(FrameCapturedCallback cb);
    void SetSplitFeedbackCallback(SplitFeedbackCallback cb);
    void SetRemuxProgressCallback(RemuxProgressCallback cb);

    // Request cooperative cancellation of any in-progress remux job.
    // Safe to call from the Qt main thread at any time; no-op if no remux is running.
    void CancelRemux();

    // True while the background remux job is running (Saving state).
    [[nodiscard]] bool IsRemuxing() const noexcept;

    // Inject a getter for the latest preview QImage (used in Ready state).
    // The getter is called on the calling thread; must be safe to call from the UI thread.
    void SetReadyFrameSource(std::function<QImage()> getter);

    // Request a frame capture. Saves a PNG to the active output folder.
    // Valid in Ready, Recording, and Paused states.
    // Fires the FrameCapturedCallback on the Qt main thread when complete.
    void CaptureFrame();

  private:
    void RecordingThreadProc(const recorder_core::RecorderConfig& config, const std::filesystem::path& output_path);
    // (Re)start or stop the shared webcam capture based on enabled/recording/preview state.
    void SyncWebcamService(bool force_restart);
    void PostStateChange(UiRecordingState new_state);
    void PostResult(UiRecordingResult result);
    void PostStats(recorder_core::SessionStats stats);
    void PostDiagnostics(recorder_core::RecordingDiagnosticsSnapshot snapshot);
    // Emit a single Initializing diagnostics snapshot so the Diagnostics page shows an
    // "initializing" state during preparation, before the engine produces live data.
    void EmitInitializingDiagnostics();
    void PostMicMeter(float rms_linear);
    void PostSysMeter(float rms_linear);
    void PostAppMeter(float rms_linear);
    void PostRecordingMeter(std::array<float, 3> per_track_rms);

    std::filesystem::path GenerateOutputPath() const;
    void WriteMarkerSidecar();
    // Write a per-segment marker sidecar adjacent to `segment_media_path`,
    // containing only markers whose session time falls in this segment, rebased to
    // segment-local time. No sidecar is written when the segment has zero markers.
    void WriteSegmentMarkerSidecar(const recorder_core::CompletedSegment& segment);
    static std::wstring FormatHResult(int32_t hr);
    static std::wstring FormatErrorPhase(recorder_core::ErrorPhase phase);

    // Recovery manifest store (nullable — injected by MainWindow via SetRecoveryManifestStore).
    RecoveryManifestStore* recovery_manifest_store_ = nullptr;
    // UUID of the manifest entry for the currently active or most recent recording.
    // Empty when no session is in flight.
    QString current_manifest_id_;

    // ADR-0015: armed-from-recovery state.
    bool is_armed_from_recovery_ = false;
    RecoverySessionInfo armed_recovery_session_{};
    // Placeholder for future: slice count for the recovery session.
    int armed_recovery_slice_count_ = 0;

    // Low-disk guard (LOW-DISK-GUARD-R1)
    // Nullable injected provider; fallback to the Win32 implementation when nullptr.
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

    void StartDiskMonitor(const std::filesystem::path& output_folder, bool is_mp4,
                          const std::filesystem::path& transient_mkv);
    void StopDiskMonitor();
    // Called by the monitor thread when the threshold is crossed.
    void OnDiskSpaceLow(uint64_t free_bytes, uint64_t threshold_bytes);

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
    bool has_output_target_context_ = false;
    FilenameTargetContext output_target_context_;

    recorder_core::RecorderSession session_;
    std::unique_ptr<recorder_core::MicMeterService> mic_meter_service_;
    std::unique_ptr<recorder_core::LoopbackMeterService> sys_meter_service_;
    std::unique_ptr<recorder_core::LoopbackMeterService> app_meter_service_;
    std::jthread recording_thread_;
    std::atomic<bool> is_recording_{false};
    std::atomic<bool> is_paused_{false};

    UiRecordingState state_ = UiRecordingState::LoadingCapabilities;
    std::wstring capability_status_text_;
    std::filesystem::path current_output_path_;

    // Recording markers
    mutable std::mutex markers_mutex_;
    std::vector<RecordingMarker> markers_;
    double last_elapsed_seconds_ = 0.0;
    uint64_t last_media_time_ns_ = 0; // media-PTS for marker timestamps
    bool markers_limit_reported_ = false;

    // Split recording (SPLIT-RECORDING-R1)
    recorder_core::RecordingSplitSettings split_settings_{};
    // True between a forwarded split request and the next segment being reported
    // by the engine. Guards against concurrent/coalesced requests.
    std::atomic<bool> split_pending_{false};
    // Segments accumulated from the engine SegmentCallback (mux worker thread).
    mutable std::mutex segments_mutex_;
    std::vector<recorder_core::CompletedSegment> segments_;
    SplitFeedbackCallback on_split_feedback_;
    void PostSplitFeedback(bool accepted, QString message);
    void OnSegmentCompleted(const recorder_core::CompletedSegment& segment);

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
        std::thread thread;                  // background remux thread
        // Written by the thread before it exits; read on the recording thread by DrainSegmentRemuxJobs.
        bool succeeded = false;
        int av_error_code = 0;
        std::string error_message;
    };

    // Protected by segment_remux_mutex_; appended from OnSegmentCompleted (mux
    // worker thread) and drained from RecordingThreadProc (recording thread).
    mutable std::mutex segment_remux_mutex_;
    std::vector<std::unique_ptr<SegmentRemuxJob>> segment_remux_jobs_;

    // Manifest ID for the next segment that has started recording but whose
    // manifest entry was created when the previous segment completed.
    // Written from OnSegmentCompleted (mux worker thread) under segment_remux_mutex_.
    // Consumed by RecordingThreadProc when the session ends.
    QString pending_segment_manifest_id_;

    // Schedule a background remux job for one MKV segment → MP4.
    // Creates a SegmentRemuxJob and starts its thread.  Called on the recording
    // thread (for the final segment) or from OnSegmentCompleted via ScheduleSegmentRemux.
    void StartSegmentRemuxThread(SegmentRemuxJob& job);

    // Join all segment remux jobs and return false if any failed.
    // cancel=true requests cancellation of any running remux.
    // Called on the recording thread after Record() returns.
    bool DrainSegmentRemuxJobs(bool cancel);

    // Total bytes across all transient MKV files that have an outstanding remux
    // job (not yet completed).  Used by the disk monitor for a conservative reserve.
    // Thread-safe (acquires segment_remux_mutex_).
    uint64_t PendingRemuxReserveBytes() const;

    StateChangedCallback on_state_changed_;
    StatsUpdatedCallback on_stats_updated_;
    DiagnosticsUpdatedCallback on_diagnostics_updated_;
    // Rejects stale-session diagnostics snapshots before they reach the UI.
    recorder_core::DiagnosticsSessionGuard diagnostics_guard_;
    std::mutex diagnostics_guard_mutex_;
    ResultReadyCallback on_result_ready_;
    MicMeterUpdatedCallback on_mic_meter_updated_;
    SysMeterUpdatedCallback on_sys_meter_updated_;
    AppMeterUpdatedCallback on_app_meter_updated_;
    RecordingMeterCallback on_recording_meter_updated_;
    FrameCapturedCallback on_frame_captured_;
    std::function<QImage()> ready_frame_source_;

    std::optional<std::string> mic_meter_device_id_;
    recorder_core::MicChannelMode mic_meter_channel_mode_ = recorder_core::MicChannelMode::Auto;
    bool mic_meter_config_valid_ = false;
};

} // namespace exosnap
