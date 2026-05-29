#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <capability/audio_ui_state.h>
#include <capability/capability_set.h>
#include <capability/resolver.h>
#include <capability/translation.h>
#include <capability/user_config.h>
#include <recorder_core/recorder_session.h>

#include "../models/FilenameBuilder.h"
#include "../models/OutputSettingsModel.h"
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
    using ResultReadyCallback = std::function<void(const UiRecordingResult&)>;
    using MicMeterUpdatedCallback = std::function<void(float rms_linear)>;
    using SysMeterUpdatedCallback = std::function<void(float rms_linear)>;
    using AppMeterUpdatedCallback = std::function<void(float rms_linear)>;

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
    void StopRecording();
    void PauseRecording();
    void ResumeRecording();
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

    void SetStateChangedCallback(StateChangedCallback cb);
    void SetStatsUpdatedCallback(StatsUpdatedCallback cb);
    void SetResultReadyCallback(ResultReadyCallback cb);
    void SetMicMeterUpdatedCallback(MicMeterUpdatedCallback cb);
    void SetSysMeterUpdatedCallback(SysMeterUpdatedCallback cb);
    void SetAppMeterUpdatedCallback(AppMeterUpdatedCallback cb);

  private:
    void RecordingThreadProc(const recorder_core::RecorderConfig& config, const std::filesystem::path& output_path);
    void PostStateChange(UiRecordingState new_state);
    void PostResult(UiRecordingResult result);
    void PostStats(recorder_core::SessionStats stats);
    void PostMicMeter(float rms_linear);
    void PostSysMeter(float rms_linear);
    void PostAppMeter(float rms_linear);

    std::filesystem::path GenerateOutputPath() const;
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

    StateChangedCallback on_state_changed_;
    StatsUpdatedCallback on_stats_updated_;
    ResultReadyCallback on_result_ready_;
    MicMeterUpdatedCallback on_mic_meter_updated_;
    SysMeterUpdatedCallback on_sys_meter_updated_;
    AppMeterUpdatedCallback on_app_meter_updated_;

    std::optional<std::string> mic_meter_device_id_;
    recorder_core::MicChannelMode mic_meter_channel_mode_ = recorder_core::MicChannelMode::Auto;
    bool mic_meter_config_valid_ = false;
};

} // namespace exosnap
