#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
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

#include "../models/OutputSettingsModel.h"
#include "../viewmodels/RecordViewModel.h"

namespace exosnap {

class RecordingCoordinator {
  public:
    using StateChangedCallback = std::function<void(UiRecordingState)>;
    using StatsUpdatedCallback = std::function<void(const recorder_core::SessionStats&)>;
    using ResultReadyCallback = std::function<void(const UiRecordingResult&)>;

    RecordingCoordinator();
    ~RecordingCoordinator();

    RecordingCoordinator(const RecordingCoordinator&) = delete;
    RecordingCoordinator& operator=(const RecordingCoordinator&) = delete;

    void OnCapabilitiesReady(const exosnap::capability::CapabilitySet& caps,
                             const exosnap::capability::ResolveResult& validation);
    void OnCapabilityFailure(std::wstring message);

    std::vector<recorder_core::CaptureTarget> EnumerateTargets();
    bool StartRecording(const recorder_core::CaptureTarget& target, const capability::AudioUiState& audio_ui_state);
    void StopRecording();

    UiRecordingState State() const noexcept;
    const std::wstring& CapabilityStatusText() const;
    std::filesystem::path CurrentOutputPath() const;
    void SetOutputSettings(const OutputSettingsModel& settings);

    void SetStateChangedCallback(StateChangedCallback cb);
    void SetStatsUpdatedCallback(StatsUpdatedCallback cb);
    void SetResultReadyCallback(ResultReadyCallback cb);

  private:
    void RecordingThreadProc(const recorder_core::RecorderConfig& config, const std::filesystem::path& output_path);
    void PostStateChange(UiRecordingState new_state);
    void PostResult(UiRecordingResult result);
    void PostStats(recorder_core::SessionStats stats);

    std::filesystem::path GenerateOutputPath() const;
    static std::wstring FormatHResult(int32_t hr);
    static std::wstring FormatErrorPhase(recorder_core::ErrorPhase phase);

    exosnap::capability::CapabilitySet caps_{};
    bool has_caps_ = false;
    exosnap::capability::ResolveResult validation_result_;
    exosnap::capability::UserRecorderConfig resolved_user_config_;
    OutputSettingsModel output_settings_;

    recorder_core::RecorderSession session_;
    std::jthread recording_thread_;
    std::atomic<bool> is_recording_{false};

    UiRecordingState state_ = UiRecordingState::LoadingCapabilities;
    std::wstring capability_status_text_;
    std::filesystem::path current_output_path_;

    StateChangedCallback on_state_changed_;
    StatsUpdatedCallback on_stats_updated_;
    ResultReadyCallback on_result_ready_;
};

} // namespace exosnap
