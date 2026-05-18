#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <recorder_core/recorder_session.h>

#include <capability/capability_set.h>
#include <capability/resolver.h>
#include <capability/translation.h>
#include <capability/user_config.h>

#include "../viewmodels/RecordViewModel.h"

#include <winrt/Microsoft.UI.Dispatching.h>

namespace exosnap {

class RecordingCoordinator {
public:
    using StateChangedCallback = std::function<void(UiRecordingState)>;
    using StatsUpdatedCallback = std::function<void(const recorder_core::SessionStats&)>;
    using ResultReadyCallback  = std::function<void(const UiRecordingResult&)>;

    explicit RecordingCoordinator(
        winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher);

    ~RecordingCoordinator();

    RecordingCoordinator(const RecordingCoordinator&)            = delete;
    RecordingCoordinator& operator=(const RecordingCoordinator&) = delete;

    // Called on UI thread when capability discovery completes successfully.
    void OnCapabilitiesReady(
        const exosnap::capability::CapabilitySet& caps,
        const exosnap::capability::ResolveResult& validation);

    // Called on UI thread when capability discovery fails.
    void OnCapabilityFailure(std::wstring message);

    // Enumerate capture targets (monitors + windows).
    std::vector<recorder_core::CaptureTarget> EnumerateTargets();

    // Start a recording session on a background thread.
    // Returns true if the session was started, false on precondition failure.
    bool StartRecording(const recorder_core::CaptureTarget& target);

    // Cooperative stop. No-op if not recording.
    void StopRecording();

    // Current state and capability status text (read from UI thread).
    UiRecordingState State()               const noexcept;
    std::wstring     CapabilityStatusText() const;

    // Current output path (valid after StartRecording returns true).
    std::filesystem::path CurrentOutputPath() const;

    void SetStateChangedCallback(StateChangedCallback cb);
    void SetStatsUpdatedCallback(StatsUpdatedCallback cb);
    void SetResultReadyCallback(ResultReadyCallback cb);

private:
    void RecordingThreadProc(
        recorder_core::RecorderConfig config,
        std::filesystem::path output_path);

    void PostStateChange(UiRecordingState new_state);
    void PostResult(UiRecordingResult result);
    void PostStats(recorder_core::SessionStats stats);

    static std::filesystem::path GenerateOutputPath();
    static std::wstring          FormatHResult(HRESULT hr);
    static std::wstring          FormatErrorPhase(recorder_core::ErrorPhase phase);

    winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher_;

    const exosnap::capability::CapabilitySet* caps_             = nullptr;
    exosnap::capability::ResolveResult        validation_result_;
    exosnap::capability::UserRecorderConfig   resolved_user_config_;

    recorder_core::RecorderSession session_;
    std::jthread                   recording_thread_;
    std::atomic<bool>              is_recording_{ false };

    UiRecordingState     state_                = UiRecordingState::LoadingCapabilities;
    std::wstring         capability_status_text_;
    std::filesystem::path current_output_path_;

    StateChangedCallback on_state_changed_;
    StatsUpdatedCallback on_stats_updated_;
    ResultReadyCallback  on_result_ready_;
};

} // namespace exosnap
