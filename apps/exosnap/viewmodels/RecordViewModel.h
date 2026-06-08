#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <capability/audio_track_preview.h>
#include <capability/audio_ui_state.h>
#include <recorder_core/recorder_session.h>

#include "models/FilenameBuilder.h"

namespace exosnap {

// ---------------------------------------------------------------------------
// UiRecordingState
// ---------------------------------------------------------------------------

enum class UiRecordingState {
    LoadingCapabilities,
    Ready,
    Blocked,
    Countdown,
    Preparing,
    RegionSelecting, // overlay shown; user drawing selection rectangle
    Recording,
    Paused,
    Stopping,
    Completed,
    Failed,
};

[[nodiscard]] inline bool IsWebcamOverlayEditable(UiRecordingState state) noexcept {
    return state == UiRecordingState::Ready || state == UiRecordingState::Countdown ||
           state == UiRecordingState::Recording || state == UiRecordingState::Paused;
}

// ---------------------------------------------------------------------------
// CaptureMode
// ---------------------------------------------------------------------------

enum class CaptureMode {
    Monitor,
    Window,
    Region, // crop from monitor capture; requires a CaptureRegion
};

// ---------------------------------------------------------------------------
// UiRecordingResult
// ---------------------------------------------------------------------------

struct UiRecordingResult {
    bool succeeded = false;
    std::wstring output_path;
    std::wstring error_phase;
    std::wstring hresult_text;
    std::wstring error_detail;
    uint64_t output_file_bytes = 0;
    double elapsed_seconds = 0.0;
};

// ---------------------------------------------------------------------------
// RecordViewModel
// ---------------------------------------------------------------------------

class RecordViewModel {
  public:
    UiRecordingState state = UiRecordingState::LoadingCapabilities;
    std::wstring capability_status_text = L"Checking system capabilities...";

    std::vector<recorder_core::CaptureTarget> targets;
    std::vector<std::wstring> target_display_names;
    int selected_target_index = -1;
    capability::AudioUiState audio_ui_state;
    capability::AudioPlanResult audio_plan;
    std::vector<capability::AudioTrackPreview> audio_track_preview;

    std::wstring output_path_display = L"--";
    std::wstring state_text;

    // Result fields
    bool last_succeeded = false;
    std::wstring result_status_text;
    std::wstring result_output_path;
    std::wstring result_error_phase;
    std::wstring result_hresult_text;
    std::wstring result_error_detail;
    std::wstring result_user_title;
    std::wstring result_user_message;
    std::wstring result_action_hint;
    std::wstring result_stats_text;
    uint64_t result_output_file_bytes = 0;
    double result_elapsed_seconds = 0.0;
    std::wstring result_destination_text;

    // Live stats fields
    std::wstring elapsed_text = L"0:00";
    uint64_t frames_captured = 0;
    uint64_t video_packets = 0;
    uint64_t audio_packets = 0;
    uint64_t video_bytes = 0;
    uint64_t audio_bytes = 0;
    uint64_t output_file_bytes = 0;
    double elapsed_seconds = 0.0;
    uint64_t dropped_frames = 0;
    double av_drift_ms = 0.0;
    std::wstring output_size_text = L"0 KB";
    float audio_rms_app = 0.0f;
    float audio_rms_sys = 0.0f;
    float audio_rms_mic = 0.0f;

    bool audio_active_app = false;
    bool audio_active_sys = false;
    bool audio_active_mic = false;
    bool live_stats_available = false;

    // Capture mode
    CaptureMode capture_mode = CaptureMode::Monitor;

    // Region capture state (only relevant when capture_mode == CaptureMode::Region)
    bool has_region = false;
    recorder_core::CaptureRegion region{}; // virtual screen coordinates
    bool select_on_record = true;          // show overlay on each record start

    // Computed predicates
    bool CanStart() const noexcept;
    bool CanStop() const noexcept;
    bool CanPause() const noexcept;
    bool CanResume() const noexcept;
    bool HasTargets() const noexcept;
    bool HasResult() const noexcept;
    bool ShouldShowStats() const noexcept;

    // Mutators
    void SetState(UiRecordingState new_state);
    void UpdateStats(const recorder_core::SessionStats& stats);
    // Update only the audio meter RMS fields — used by the high-cadence recording meter path.
    void UpdateMeterRms(const std::array<float, 3>& per_track_rms);
    void SetResult(const UiRecordingResult& result);
    void ResetStats();
    void ApplyTargetKind(capability::CaptureTargetKind kind);
    void ApplyTargetKindPreservingAudio(capability::CaptureTargetKind kind);
    void RebuildAudioPlan();

    // Formatting helpers
    static std::wstring FormatElapsed(double elapsed_seconds);
    static std::wstring FormatBytes(uint64_t bytes);
    static std::string DisplayLabelFromTarget(const std::string& raw_description);
    static std::string WindowLabelFromTarget(const std::string& raw_description);
    static std::string TargetLabelFromCaptureTarget(const recorder_core::CaptureTarget& target);
    static FilenameTargetContext FilenameContextFromCaptureTarget(const recorder_core::CaptureTarget& target);
    static std::vector<int> SortWindowTargetIndices(const std::vector<recorder_core::CaptureTarget>& targets,
                                                    const std::vector<int>& window_indices);
};

} // namespace exosnap
