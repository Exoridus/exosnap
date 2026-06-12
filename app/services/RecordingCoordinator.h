#pragma once
#include <array>
#include <atomic>
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

#include "../models/FilenameBuilder.h"
#include "../models/OutputSettingsModel.h"
#include "../models/RecordingMarker.h"
#include "../models/VideoSettingsModel.h"
#include "../models/WebcamSettings.h"
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
    RemuxProgressCallback on_remux_progress_;
    void PostRemuxProgress(float fraction);
    void RunRemuxJob(const std::filesystem::path& transient_mkv, const std::filesystem::path& final_mp4,
                     UiRecordingResult base_result);

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
