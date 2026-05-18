#include "RecordViewModel.h"

#include <cstdio>

namespace exosnap {

// ---------------------------------------------------------------------------
// Computed predicates
// ---------------------------------------------------------------------------

bool RecordViewModel::CanStart() const noexcept {
    if (state != UiRecordingState::Ready &&
        state != UiRecordingState::Completed &&
        state != UiRecordingState::Failed) {
        return false;
    }
    if (selected_target_index < 0) return false;
    if (!HasTargets()) return false;
    return true;
}

bool RecordViewModel::CanStop() const noexcept {
    return state == UiRecordingState::Recording;
}

bool RecordViewModel::HasTargets() const noexcept {
    return !targets.empty();
}

bool RecordViewModel::HasResult() const noexcept {
    return state == UiRecordingState::Completed ||
           state == UiRecordingState::Failed;
}

bool RecordViewModel::ShouldShowStats() const noexcept {
    return state == UiRecordingState::Recording ||
           state == UiRecordingState::Stopping;
}

// ---------------------------------------------------------------------------
// Mutators
// ---------------------------------------------------------------------------

void RecordViewModel::SetState(UiRecordingState new_state) {
    state = new_state;

    switch (new_state) {
    case UiRecordingState::LoadingCapabilities:
        state_text = L"Checking capabilities...";
        break;
    case UiRecordingState::Ready:
        state_text = L"Ready";
        break;
    case UiRecordingState::Blocked:
        state_text = L"Blocked";
        break;
    case UiRecordingState::Preparing:
        state_text = L"Preparing...";
        break;
    case UiRecordingState::Recording:
        state_text = L"Recording";
        break;
    case UiRecordingState::Stopping:
        state_text = L"Stopping...";
        break;
    case UiRecordingState::Completed:
        state_text = L"Completed";
        break;
    case UiRecordingState::Failed:
        state_text = L"Failed";
        break;
    }
}

void RecordViewModel::UpdateStats(const recorder_core::SessionStats& stats) {
    elapsed_text     = FormatElapsed(stats.elapsed_seconds);
    frames_captured  = stats.video_frames_captured;
    video_packets    = stats.encoded_video_packets;
    audio_packets    = stats.audio_packets;
    dropped_frames   = stats.dropped_or_skipped_video_frames;
    output_size_text = FormatBytes(stats.output_file_bytes);
}

void RecordViewModel::SetResult(const UiRecordingResult& result) {
    last_succeeded      = result.succeeded;
    result_status_text  = result.succeeded ? L"Recording succeeded" : L"Recording failed";
    result_output_path  = result.output_path;
    result_error_phase  = result.error_phase;
    result_hresult_text = result.hresult_text;
    result_error_detail = result.error_detail;
}

void RecordViewModel::ResetStats() {
    elapsed_text     = L"0:00";
    frames_captured  = 0;
    video_packets    = 0;
    audio_packets    = 0;
    dropped_frames   = 0;
    output_size_text = L"0 KB";
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

std::wstring RecordViewModel::FormatElapsed(double elapsed_seconds) {
    if (elapsed_seconds < 0.0) elapsed_seconds = 0.0;
    auto total = static_cast<uint64_t>(elapsed_seconds);
    uint64_t minutes = total / 60;
    uint64_t seconds = total % 60;

    wchar_t buf[32];
    _snwprintf_s(buf, _TRUNCATE, L"%llu:%02llu",
                 static_cast<unsigned long long>(minutes),
                 static_cast<unsigned long long>(seconds));
    return buf;
}

std::wstring RecordViewModel::FormatBytes(uint64_t bytes) {
    constexpr uint64_t KB = 1024ULL;
    constexpr uint64_t MB = 1024ULL * 1024ULL;

    wchar_t buf[64];
    if (bytes < KB) {
        _snwprintf_s(buf, _TRUNCATE, L"%llu B",
                     static_cast<unsigned long long>(bytes));
    } else if (bytes < MB) {
        _snwprintf_s(buf, _TRUNCATE, L"%llu KB",
                     static_cast<unsigned long long>(bytes / KB));
    } else {
        double mb = static_cast<double>(bytes) / static_cast<double>(MB);
        _snwprintf_s(buf, _TRUNCATE, L"%.1f MB", mb);
    }
    return buf;
}

} // namespace exosnap
