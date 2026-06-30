#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <capability/audio_track_preview.h>
#include <capability/audio_ui_state.h>
#include <recorder_core/recorder_session.h>

#include "models/CompletedRecording.h"
#include "models/FilenameBuilder.h"
#include "models/RecordingMarker.h"

namespace exosnap {

class RecordingHistoryStore;

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
    // ADR-0015: armed-from-recovery. The user chose "Continue" for a crash
    // artefact. The artefact is being repair-remuxed in the background as the
    // first slice of the session. The session is paused; Resume starts the next
    // slice. Visually equivalent to Paused from the TransportDock's perspective.
    ArmedFromRecovery,
    Stopping,
    // ADR-0014: MP4 remux-on-stop. After the recording engine stops for an MP4
    // session, the background remux job runs before the result is ready.
    // The UI shows "Saving…" with a progress indicator during this phase.
    Saving,
    Completed,
    Failed,
};

[[nodiscard]] inline bool IsWebcamOverlayEditable(UiRecordingState state) noexcept {
    return state == UiRecordingState::Ready || state == UiRecordingState::Countdown ||
           state == UiRecordingState::Recording || state == UiRecordingState::Paused ||
           state == UiRecordingState::ArmedFromRecovery;
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
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    recorder_core::ContentRect content_rect;
    uint32_t frame_rate_num = 60;
    uint32_t frame_rate_den = 1;
    bool cfr = true;
    recorder_core::Container container = recorder_core::Container::WebM;
    recorder_core::VideoCodec video_codec = recorder_core::VideoCodec::Av1Nvenc;
    recorder_core::AudioCodec audio_codec = recorder_core::AudioCodec::Opus;
    std::vector<RecordingMarker> markers;
    std::wstring marker_sidecar_path;

    // Path to the canonical MKV edit master (0.9.0 S1 — Edit/Output/Save):
    //   - For MKV recordings: same as output_path (the file IS the master).
    //   - For MP4 recordings (single-file): the companion .edit.mkv retained after remux.
    //   - Empty for split sessions, failed recordings, or when retention failed.
    std::wstring mkv_master_path;

    // Multi-segment split results (SPLIT-RECORDING-R1). Empty for a legacy
    // single-file recording (the scalar output_path/file fields describe it).
    std::vector<CompletedRecordingSegment> segments;
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
    uint32_t result_source_width = 0;
    uint32_t result_source_height = 0;
    uint32_t result_output_width = 0;
    uint32_t result_output_height = 0;
    recorder_core::ContentRect result_content_rect;
    uint32_t result_frame_rate_num = 60;
    uint32_t result_frame_rate_den = 1;
    bool result_cfr = true;
    recorder_core::Container result_container = recorder_core::Container::WebM;
    recorder_core::VideoCodec result_video_codec = recorder_core::VideoCodec::Av1Nvenc;
    recorder_core::AudioCodec result_audio_codec = recorder_core::AudioCodec::Opus;

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
    bool HasCompletedRecording() const noexcept;

    // Mutators
    void SetState(UiRecordingState new_state);
    void UpdateStats(const recorder_core::SessionStats& stats);
    // Update only the audio meter RMS fields — used by the high-cadence recording meter path.
    void UpdateMeterRms(const std::array<float, 3>& per_track_rms);
    void SetResult(const UiRecordingResult& result);
    void ResetStats();
    void ClearCompletedResult();

    // Completed recording operations
    CompletedRecording current_completed_recording;
    QVector<CompletedRecording> recent_recordings;
    static constexpr int kMaxRecentRecordings = 10;

    void SetHistoryStore(RecordingHistoryStore* store);
    // Visual-test harness isolation (VR-003): when disabled, history mutations
    // stay in-memory and never reach the user's persisted store.
    void SetHistoryPersistenceEnabled(bool enabled);
    void RestoreHistory(const QVector<CompletedRecording>& recordings);

    void AddToRecentRecordings(const CompletedRecording& recording);
    void RemoveFromRecentRecordings(int index);
    void UpdateRecentRecording(int index, const CompletedRecording& recording);
    void ClearRecentRecordings();
    [[nodiscard]] bool HasRecentRecordings() const noexcept;
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

  private:
    RecordingHistoryStore* history_store_ = nullptr;
    bool history_persistence_enabled_ = true;
    void PersistHistory() const;
};

} // namespace exosnap
