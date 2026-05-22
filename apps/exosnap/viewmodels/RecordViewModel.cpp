#include "RecordViewModel.h"

#include "../diagnostics/error_message.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <string_view>

namespace exosnap {
namespace {

std::string TrimAscii(const std::string& value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }

    return value.substr(first, last - first);
}

bool StartsWithAsciiInsensitive(const std::string_view value, const std::string_view prefix) {
    if (prefix.size() > value.size()) {
        return false;
    }

    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(value[i]);
        const unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }

    return true;
}

bool EqualsAsciiInsensitive(const std::string_view a, const std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }

    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char lhs = static_cast<unsigned char>(a[i]);
        const unsigned char rhs = static_cast<unsigned char>(b[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }

    return true;
}

std::string ToLowerAscii(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        result.push_back(static_cast<char>(std::tolower(ch)));
    }
    return result;
}

bool IsHexHandleOnly(const std::string_view value) {
    std::size_t cursor = 0;
    if (value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        cursor = 2;
    }
    if ((value.size() - cursor) < 5) {
        return false;
    }
    for (std::size_t i = cursor; i < value.size(); ++i) {
        if (std::isxdigit(static_cast<unsigned char>(value[i])) == 0) {
            return false;
        }
    }
    return true;
}

bool IsInternalWindowToken(const std::string& raw_value) {
    const std::string value = TrimAscii(raw_value);
    if (value.empty()) {
        return true;
    }
    if (IsHexHandleOnly(value)) {
        return true;
    }
    if (StartsWithAsciiInsensitive(value, "hwnd:")) {
        return true;
    }
    if (StartsWithAsciiInsensitive(value, "window:")) {
        return true;
    }
    if (EqualsAsciiInsensitive(value, "(unnamed)") || EqualsAsciiInsensitive(value, "unnamed")) {
        return true;
    }
    return false;
}

bool FindWindowLabelSeparator(const std::string& value, std::size_t* out_pos, std::size_t* out_size) {
    const std::string separators[] = {" \xE2\x80\x94 ", " - "};

    bool found = false;
    std::size_t best_pos = 0;
    std::size_t best_size = 0;

    for (const auto& separator : separators) {
        const std::size_t candidate = value.rfind(separator);
        if (candidate == std::string::npos) {
            continue;
        }
        if (!found || candidate > best_pos) {
            found = true;
            best_pos = candidate;
            best_size = separator.size();
        }
    }

    if (!found) {
        return false;
    }

    *out_pos = best_pos;
    *out_size = best_size;
    return true;
}

struct WindowLabelParts {
    std::string label = "Window";
    std::string app_name;
    std::string window_title;
    bool has_app_name = false;
};

WindowLabelParts BuildWindowLabelParts(const std::string& raw_description) {
    WindowLabelParts parts;
    const std::string value = TrimAscii(raw_description);
    const bool value_is_internal = IsInternalWindowToken(value);

    if (value_is_internal) {
        return parts;
    }

    // Fallback: preserve the existing user-facing label when no app/title split is available.
    parts.label = value;

    std::size_t separator_pos = 0;
    std::size_t separator_size = 0;
    if (!FindWindowLabelSeparator(value, &separator_pos, &separator_size)) {
        return parts;
    }

    const std::string raw_title = TrimAscii(value.substr(0, separator_pos));
    const std::string raw_app_name = TrimAscii(value.substr(separator_pos + separator_size));

    if (raw_app_name.empty() || IsInternalWindowToken(raw_app_name)) {
        return parts;
    }

    parts.has_app_name = true;
    parts.app_name = raw_app_name;

    if (!raw_title.empty() && !IsInternalWindowToken(raw_title) && !EqualsAsciiInsensitive(raw_title, raw_app_name)) {
        parts.window_title = raw_title;
    }

    if (parts.window_title.empty()) {
        parts.label = parts.app_name;
    } else {
        parts.label = parts.app_name + " \xE2\x80\x94 " + parts.window_title;
    }

    return parts;
}

} // namespace

// ---------------------------------------------------------------------------
// Computed predicates
// ---------------------------------------------------------------------------

bool RecordViewModel::CanStart() const noexcept {
    if (state != UiRecordingState::Ready && state != UiRecordingState::Completed && state != UiRecordingState::Failed) {
        return false;
    }
    if (selected_target_index < 0)
        return false;
    if (!HasTargets())
        return false;
    return true;
}

bool RecordViewModel::CanStop() const noexcept {
    return state == UiRecordingState::Recording;
}

bool RecordViewModel::HasTargets() const noexcept {
    return !targets.empty();
}

bool RecordViewModel::HasResult() const noexcept {
    return state == UiRecordingState::Completed || state == UiRecordingState::Failed;
}

bool RecordViewModel::ShouldShowStats() const noexcept {
    return state == UiRecordingState::Recording || state == UiRecordingState::Stopping;
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
    elapsed_text = FormatElapsed(stats.elapsed_seconds);
    elapsed_seconds = stats.elapsed_seconds;
    frames_captured = stats.video_frames_captured;
    video_packets = stats.encoded_video_packets;
    audio_packets = stats.audio_packets;
    video_bytes = stats.video_bytes;
    audio_bytes = stats.audio_bytes;
    output_file_bytes = stats.output_file_bytes;
    dropped_frames = stats.dropped_or_skipped_video_frames;
    output_size_text = FormatBytes(stats.output_file_bytes);
    live_stats_available = (stats.elapsed_seconds > 0.0) || (stats.output_file_bytes > 0) || (stats.video_bytes > 0) ||
                           (stats.audio_bytes > 0) || (stats.video_frames_captured > 0);

    audio_rms_app = 0.0f;
    audio_rms_sys = 0.0f;
    audio_rms_mic = 0.0f;

    for (const auto& preview : audio_track_preview) {
        if (preview.track_number == 0) {
            continue;
        }

        const std::size_t track_index = static_cast<std::size_t>(preview.track_number - 1);
        if (track_index >= stats.per_track_rms.size()) {
            continue;
        }

        const float rms = stats.per_track_rms[track_index];

        if (preview.source_key == "app") {
            audio_rms_app = rms;
        } else if (preview.source_key == "sys" || preview.source_key == "system_output") {
            audio_rms_sys = rms;
        } else if (preview.source_key == "mic") {
            audio_rms_mic = rms;
        }
    }
}

void RecordViewModel::SetResult(const UiRecordingResult& result) {
    last_succeeded = result.succeeded;
    result_status_text = result.succeeded ? L"Recording succeeded" : L"Recording failed";
    result_output_path = result.output_path;
    result_error_phase = result.error_phase;
    result_hresult_text = result.hresult_text;
    result_error_detail = result.error_detail;

    const auto msg = exosnap::diagnostics::MapErrorToUserMessage(result);
    result_user_title = msg.title;
    result_user_message = msg.message;
    result_action_hint = msg.action_hint;

    if (result.succeeded) {
        result_stats_text = elapsed_text + L"  ·  " + output_size_text;
    } else {
        result_stats_text = {};
    }
}

void RecordViewModel::ResetStats() {
    elapsed_text = L"0:00";
    elapsed_seconds = 0.0;
    frames_captured = 0;
    video_packets = 0;
    audio_packets = 0;
    video_bytes = 0;
    audio_bytes = 0;
    output_file_bytes = 0;
    dropped_frames = 0;
    output_size_text = L"0 KB";
    audio_rms_app = 0.0f;
    audio_rms_sys = 0.0f;
    audio_rms_mic = 0.0f;
    result_user_title = {};
    result_user_message = {};
    result_action_hint = {};
    result_stats_text = {};
    live_stats_available = false;
}

void RecordViewModel::ApplyTargetKind(capability::CaptureTargetKind kind) {
    audio_ui_state.target_kind = kind;
    audio_ui_state.selected_window_pid.reset();
    // Privacy-first MVP default: microphone recording starts disabled.

    if (kind == capability::CaptureTargetKind::Window) {
        audio_ui_state.record_application_audio = true;
        audio_ui_state.record_system_audio = true;
        audio_ui_state.separate_output_tracks = true;
        audio_ui_state.record_microphone = false;
        audio_ui_state.mic_channel_mode = recorder_core::MicChannelMode::Auto;
    } else {
        audio_ui_state.record_application_audio = false;
        audio_ui_state.record_system_audio = true;
        audio_ui_state.separate_output_tracks = false;
        audio_ui_state.record_microphone = false;
        audio_ui_state.mic_channel_mode = recorder_core::MicChannelMode::Auto;
    }

    RebuildAudioPlan();
}

void RecordViewModel::ApplyTargetKindPreservingAudio(capability::CaptureTargetKind kind) {
    audio_ui_state.target_kind = kind;
    if (kind != capability::CaptureTargetKind::Window) {
        audio_ui_state.selected_window_pid.reset();
    }

    RebuildAudioPlan();
}

void RecordViewModel::RebuildAudioPlan() {
    audio_plan = capability::BuildAudioPlan(audio_ui_state);
    audio_track_preview = capability::BuildAudioTrackPreview(audio_plan);

    audio_active_app = false;
    audio_active_sys = false;
    audio_active_mic = false;

    for (const auto& preview : audio_track_preview) {
        if (preview.source_key == "app") {
            audio_active_app = true;
        } else if (preview.source_key == "sys" || preview.source_key == "system_output") {
            audio_active_sys = true;
        } else if (preview.source_key == "mic") {
            audio_active_mic = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

std::wstring RecordViewModel::FormatElapsed(double elapsed_seconds) {
    if (elapsed_seconds < 0.0)
        elapsed_seconds = 0.0;
    auto total = static_cast<uint64_t>(elapsed_seconds);
    uint64_t minutes = total / 60;
    uint64_t seconds = total % 60;

    wchar_t buf[32];
    _snwprintf_s(buf, _TRUNCATE, L"%llu:%02llu", static_cast<unsigned long long>(minutes),
                 static_cast<unsigned long long>(seconds));
    return buf;
}

std::wstring RecordViewModel::FormatBytes(uint64_t bytes) {
    constexpr uint64_t KB = 1024ULL;
    constexpr uint64_t MB = 1024ULL * 1024ULL;

    wchar_t buf[64];
    if (bytes < KB) {
        _snwprintf_s(buf, _TRUNCATE, L"%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < MB) {
        _snwprintf_s(buf, _TRUNCATE, L"%llu KB", static_cast<unsigned long long>(bytes / KB));
    } else {
        double mb = static_cast<double>(bytes) / static_cast<double>(MB);
        _snwprintf_s(buf, _TRUNCATE, L"%.1f MB", mb);
    }
    return buf;
}

std::string RecordViewModel::DisplayLabelFromTarget(const std::string& raw_description) {
    std::string value = TrimAscii(raw_description);
    if (value.empty()) {
        return "Display";
    }

    if (StartsWithAsciiInsensitive(value, R"(\\.\)")) {
        value.erase(0, 4);
    } else if (StartsWithAsciiInsensitive(value, "//./")) {
        value.erase(0, 4);
    }

    if (value.size() > 7 && StartsWithAsciiInsensitive(value, "DISPLAY")) {
        const std::string suffix = value.substr(7);
        const bool digits_only = !suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](const char ch) {
            return std::isdigit(static_cast<unsigned char>(ch)) != 0;
        });
        if (digits_only) {
            return "Display " + suffix;
        }
    }

    return value;
}

std::string RecordViewModel::WindowLabelFromTarget(const std::string& raw_description) {
    return BuildWindowLabelParts(raw_description).label;
}

std::vector<int> RecordViewModel::SortWindowTargetIndices(const std::vector<recorder_core::CaptureTarget>& targets,
                                                          const std::vector<int>& window_indices) {
    struct SortEntry {
        int target_index = -1;
        std::string app_key;
        std::string title_key;
    };

    std::vector<SortEntry> entries;
    entries.reserve(window_indices.size());

    for (const int target_index : window_indices) {
        if (target_index < 0 || target_index >= static_cast<int>(targets.size())) {
            continue;
        }

        const auto& target = targets[static_cast<std::size_t>(target_index)];
        if (target.kind != recorder_core::CaptureTarget::Kind::Window) {
            continue;
        }

        const WindowLabelParts parts = BuildWindowLabelParts(target.description);
        const std::string app_value = parts.has_app_name ? parts.app_name : parts.label;
        const std::string title_value = parts.has_app_name ? parts.window_title : std::string{};

        entries.push_back({target_index, ToLowerAscii(TrimAscii(app_value)), ToLowerAscii(TrimAscii(title_value))});
    }

    std::stable_sort(entries.begin(), entries.end(), [](const SortEntry& lhs, const SortEntry& rhs) {
        if (lhs.app_key != rhs.app_key) {
            return lhs.app_key < rhs.app_key;
        }
        if (lhs.title_key != rhs.title_key) {
            return lhs.title_key < rhs.title_key;
        }
        return lhs.target_index < rhs.target_index;
    });

    std::vector<int> sorted_indices;
    sorted_indices.reserve(entries.size());
    for (const auto& entry : entries) {
        sorted_indices.push_back(entry.target_index);
    }

    return sorted_indices;
}

} // namespace exosnap
