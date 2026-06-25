#include "RecordViewModel.h"

#include "../diagnostics/error_message.h"
#include "../settings/RecordingHistoryStore.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <windows.h>

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

std::wstring ContainerLabel(recorder_core::Container container) {
    switch (container) {
    case recorder_core::Container::Matroska:
        return L"MKV";
    case recorder_core::Container::Mp4:
        return L"MP4";
    case recorder_core::Container::WebM:
        return L"WebM";
    }
    return L"MKV";
}

std::wstring VideoCodecLabel(recorder_core::VideoCodec codec) {
    switch (codec) {
    case recorder_core::VideoCodec::H264Nvenc:
        return L"H.264";
    case recorder_core::VideoCodec::HevcNvenc:
        return L"HEVC";
    case recorder_core::VideoCodec::Av1Nvenc:
        return L"AV1";
    }
    return L"AV1";
}

std::wstring AudioCodecLabel(recorder_core::AudioCodec codec) {
    switch (codec) {
    case recorder_core::AudioCodec::AacMf:
        return L"AAC";
    case recorder_core::AudioCodec::Opus:
        return L"Opus";
    case recorder_core::AudioCodec::Pcm:
        return L"PCM";
    case recorder_core::AudioCodec::Flac:
        return L"FLAC";
    }
    return L"Opus";
}

std::wstring FrameRateLabel(uint32_t numerator, uint32_t denominator) {
    if (numerator == 0 || denominator == 0) {
        return L"60 fps";
    }
    if (denominator == 1) {
        return std::to_wstring(numerator) + L" fps";
    }
    return std::to_wstring(numerator) + L"/" + std::to_wstring(denominator) + L" fps";
}

std::string ToLowerAscii(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        result.push_back(static_cast<char>(std::tolower(ch)));
    }
    return result;
}

std::wstring ToWideUtf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int count =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr,
                        nullptr);
    return result;
}

std::string BuildProcessNameFromApp(const std::string& app_name) {
    const std::string trimmed = TrimAscii(app_name);
    if (trimmed.empty()) {
        return "window";
    }

    std::string process_name;
    process_name.reserve(trimmed.size());
    for (const unsigned char ch : trimmed) {
        if (std::isalnum(ch) == 0) {
            continue;
        }
        process_name.push_back(static_cast<char>(std::tolower(ch)));
    }

    if (process_name.empty()) {
        return "window";
    }
    return process_name;
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
    if (capture_mode == CaptureMode::Region) {
        // Region mode: any selected monitor is needed as the base capture target.
        // selected_target_index may point to a monitor even if the mode is Region.
        // Allow start so the coordinator can resolve the target.
        return HasTargets();
    }
    if (selected_target_index < 0)
        return false;
    if (!HasTargets())
        return false;
    return true;
}

bool RecordViewModel::CanStop() const noexcept {
    return state == UiRecordingState::Recording || state == UiRecordingState::Paused;
}

bool RecordViewModel::CanPause() const noexcept {
    return state == UiRecordingState::Recording;
}

bool RecordViewModel::CanResume() const noexcept {
    return state == UiRecordingState::Paused;
}

bool RecordViewModel::HasTargets() const noexcept {
    return !targets.empty();
}

bool RecordViewModel::HasResult() const noexcept {
    return state == UiRecordingState::Completed || state == UiRecordingState::Failed;
}

bool RecordViewModel::HasCompletedRecording() const noexcept {
    return HasResult() && last_succeeded;
}

bool RecordViewModel::HasRecentRecordings() const noexcept {
    return !recent_recordings.isEmpty();
}

bool RecordViewModel::ShouldShowStats() const noexcept {
    return state == UiRecordingState::Recording || state == UiRecordingState::Paused ||
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
    case UiRecordingState::Countdown:
        state_text = L"Countdown";
        break;
    case UiRecordingState::Preparing:
        state_text = L"Preparing...";
        break;
    case UiRecordingState::RegionSelecting:
        state_text = L"Select Region...";
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

void RecordViewModel::UpdateMeterRms(const std::array<float, 3>& per_track_rms) {
    audio_rms_app = 0.0f;
    audio_rms_sys = 0.0f;
    audio_rms_mic = 0.0f;

    for (const auto& preview : audio_track_preview) {
        if (preview.track_number == 0)
            continue;

        const std::size_t track_index = static_cast<std::size_t>(preview.track_number - 1);
        if (track_index >= per_track_rms.size())
            continue;

        const float rms = per_track_rms[track_index];

        if (preview.source_key == "app") {
            audio_rms_app = rms;
        } else if (preview.source_key == "sys" || preview.source_key == "system_output") {
            audio_rms_sys = rms;
        } else if (preview.source_key == "mic") {
            audio_rms_mic = rms;
        } else if (preview.source_key == "merged") {
            if (audio_ui_state.IsAppEnabled())
                audio_rms_app = rms;
            if (audio_ui_state.IsSysEnabled())
                audio_rms_sys = rms;
            if (audio_ui_state.IsMicEnabled())
                audio_rms_mic = rms;
        }
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
    av_drift_ms = stats.duration_skew_ms;
    output_size_text = FormatBytes(stats.output_file_bytes);
    live_stats_available = (stats.elapsed_seconds > 0.0) || (stats.output_file_bytes > 0) || (stats.video_bytes > 0) ||
                           (stats.audio_bytes > 0) || (stats.video_frames_captured > 0);

    UpdateMeterRms(stats.per_track_rms);
}

void RecordViewModel::SetResult(const UiRecordingResult& result) {
    last_succeeded = result.succeeded;
    result_status_text = result.succeeded ? L"Recording succeeded" : L"Recording failed";
    result_output_path = result.output_path;
    result_error_phase = result.error_phase;
    result_hresult_text = result.hresult_text;
    result_error_detail = result.error_detail;
    result_output_file_bytes = result.output_file_bytes;
    result_elapsed_seconds = result.elapsed_seconds;
    result_source_width = result.source_width;
    result_source_height = result.source_height;
    result_output_width = result.output_width;
    result_output_height = result.output_height;
    result_content_rect = result.content_rect;
    result_frame_rate_num = result.frame_rate_num;
    result_frame_rate_den = result.frame_rate_den;
    result_cfr = result.cfr;
    result_container = result.container;
    result_video_codec = result.video_codec;
    result_audio_codec = result.audio_codec;

    const auto msg = exosnap::diagnostics::MapErrorToUserMessage(result);
    result_user_title = msg.title;
    result_user_message = msg.message;
    result_action_hint = msg.action_hint;

    if (result.succeeded) {
        const std::wstring elapsed_display =
            result.elapsed_seconds > 0.0 ? FormatElapsed(result.elapsed_seconds) : elapsed_text;
        const std::wstring size_display =
            result.output_file_bytes > 0 ? FormatBytes(result.output_file_bytes) : output_size_text;
        const std::wstring output_display =
            (result.output_width > 0 && result.output_height > 0)
                ? std::to_wstring(result.output_width) + L"x" + std::to_wstring(result.output_height)
                : L"Output size unknown";
        const std::wstring timing_display =
            FrameRateLabel(result.frame_rate_num, result.frame_rate_den) + L" " + (result.cfr ? L"CFR" : L"VFR");
        const std::wstring format_display = VideoCodecLabel(result.video_codec) + L" · " +
                                            AudioCodecLabel(result.audio_codec) + L" · " +
                                            ContainerLabel(result.container);
        result_stats_text =
            elapsed_display + L"  ·  " + size_display + L"  ·  " + output_display + L"  ·  " + timing_display;
        std::filesystem::path p(result.output_path);
        std::wstring filename = p.filename().wstring();
        result_destination_text = filename;
        if (!size_display.empty() || !elapsed_display.empty()) {
            result_destination_text += L"  ·  ";
            result_destination_text += size_display;
            result_destination_text += L"  ·  ";
            result_destination_text += elapsed_display;
            result_destination_text += L"  ·  ";
            result_destination_text += output_display;
            result_destination_text += L"  ·  ";
            result_destination_text += format_display;
        }

        // Multi-segment recordings: prepend a "N segments" summary so the completed
        // panel reads e.g. "3 segments · ..." (single-file recordings unchanged).
        if (result.segments.size() > 1) {
            result_destination_text =
                std::to_wstring(result.segments.size()) + L" segments  ·  " + result_destination_text;
        }

        // Populate CompletedRecording model from effective runtime result
        CompletedRecording cr;
        cr.succeeded = true;
        cr.file_path = QString::fromStdWString(result.output_path);
        cr.display_name = QString::fromStdWString(p.filename().wstring());
        cr.file_size_bytes = static_cast<qint64>(result.output_file_bytes);
        cr.duration_seconds = result.elapsed_seconds;
        cr.source_width = result.source_width;
        cr.source_height = result.source_height;
        cr.output_width = result.output_width;
        cr.output_height = result.output_height;
        cr.frame_rate_num = result.frame_rate_num;
        cr.frame_rate_den = result.frame_rate_den;
        cr.cfr = result.cfr;
        cr.container = result.container;
        cr.video_codec = result.video_codec;
        cr.audio_codec = result.audio_codec;
        cr.completed_at = QDateTime::currentDateTime();
        cr.markers = result.markers;
        cr.marker_sidecar_path = QString::fromStdWString(result.marker_sidecar_path);
        // Multi-segment split results: carry the per-segment list so the completed
        // panel and history can show totals / per-segment rows. The scalar fields
        // above continue to describe the first (or only) segment for single-file
        // compatibility.
        cr.segments = result.segments;
        current_completed_recording = cr;
        AddToRecentRecordings(cr);
    } else {
        result_stats_text = {};
        result_destination_text = {};
        current_completed_recording = CompletedRecording{};
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
    av_drift_ms = 0.0;
    output_size_text = L"0 KB";
    audio_rms_app = 0.0f;
    audio_rms_sys = 0.0f;
    audio_rms_mic = 0.0f;
    result_user_title = {};
    result_user_message = {};
    result_action_hint = {};
    result_stats_text = {};
    result_source_width = 0;
    result_source_height = 0;
    result_output_width = 0;
    result_output_height = 0;
    result_content_rect = {};
    result_frame_rate_num = 60;
    result_frame_rate_den = 1;
    result_cfr = true;
    result_container = recorder_core::Container::WebM;
    result_video_codec = recorder_core::VideoCodec::Av1Nvenc;
    result_audio_codec = recorder_core::AudioCodec::Opus;
    live_stats_available = false;
}

void RecordViewModel::ApplyTargetKind(capability::CaptureTargetKind kind) {
    audio_ui_state.target_kind = kind;
    audio_ui_state.selected_window_pid.reset();
    audio_ui_state.mic_channel_mode = recorder_core::MicChannelMode::Auto;

    using K = recorder_core::AudioSourceKind;
    if (kind == capability::CaptureTargetKind::Window) {
        // Window: Application audio ON; Other system audio and Microphone OFF by default.
        audio_ui_state.source_rows = {
            {K::App, true, false},
            {K::Mic, false, false},
            {K::Sys, false, false},
        };
    } else {
        // Display/Region: Computer audio ON; Microphone OFF by default.
        audio_ui_state.source_rows = {
            {K::SystemOutput, true, false},
            {K::Mic, false, false},
        };
    }

    RebuildAudioPlan();
}

void RecordViewModel::ApplyTargetKindPreservingAudio(capability::CaptureTargetKind kind) {
    audio_ui_state.target_kind = kind;
    if (kind != capability::CaptureTargetKind::Window) {
        audio_ui_state.selected_window_pid.reset();
    }

    if (kind == capability::CaptureTargetKind::Window) {
        using K = recorder_core::AudioSourceKind;
        const bool has_app = std::any_of(audio_ui_state.source_rows.begin(), audio_ui_state.source_rows.end(),
                                         [](const recorder_core::AudioSourceRow& r) { return r.kind == K::App; });
        if (!has_app) {
            // App is first in canonical Window row order (App, Mic, Sys).
            audio_ui_state.source_rows.insert(audio_ui_state.source_rows.begin(),
                                              recorder_core::AudioSourceRow{K::App, true, false});
        }
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
        } else if (preview.source_key == "merged") {
            if (audio_ui_state.IsAppEnabled())
                audio_active_app = true;
            if (audio_ui_state.IsSysEnabled())
                audio_active_sys = true;
            if (audio_ui_state.IsMicEnabled())
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

std::string RecordViewModel::TargetLabelFromCaptureTarget(const recorder_core::CaptureTarget& target) {
    const FilenameTargetContext context = FilenameContextFromCaptureTarget(target);
    const std::string label = ToUtf8(context.target_name);

    if (!label.empty()) {
        return label;
    }

    if (target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
        return "Desktop - " + DisplayLabelFromTarget(target.description);
    }

    return WindowLabelFromTarget(target.description);
}

FilenameTargetContext RecordViewModel::FilenameContextFromCaptureTarget(const recorder_core::CaptureTarget& target) {
    FilenameTargetContext context;

    if (target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
        const std::string display_label = DisplayLabelFromTarget(target.description);
        context.app_name = L"Desktop";
        context.window_title = ToWideUtf8(display_label);
        context.process_name = L"desktop";
        context.target_name = L"Desktop - " + context.window_title;
        return context;
    }

    const WindowLabelParts parts = BuildWindowLabelParts(target.description);
    const std::string app_name = parts.has_app_name ? TrimAscii(parts.app_name) : TrimAscii(parts.label);
    std::string title = TrimAscii(parts.window_title);

    const std::string fallback_app = app_name.empty() ? std::string("Window") : app_name;
    if (title.empty()) {
        title = fallback_app;
    }

    const std::string process_name = BuildProcessNameFromApp(fallback_app);

    context.app_name = ToWideUtf8(fallback_app);
    context.window_title = ToWideUtf8(title);
    context.process_name = ToWideUtf8(process_name);
    context.target_name = context.app_name + L" - " + context.window_title;
    return context;
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

// ---------------------------------------------------------------------------
// Completed recording operations
// ---------------------------------------------------------------------------

void RecordViewModel::SetHistoryStore(RecordingHistoryStore* store) {
    history_store_ = store;
}

void RecordViewModel::SetHistoryPersistenceEnabled(bool enabled) {
    history_persistence_enabled_ = enabled;
}

void RecordViewModel::RestoreHistory(const QVector<CompletedRecording>& recordings) {
    recent_recordings = recordings;
    while (recent_recordings.size() > kMaxRecentRecordings) {
        recent_recordings.removeLast();
    }
    PersistHistory();
}

void RecordViewModel::PersistHistory() const {
    if (history_persistence_enabled_ && history_store_)
        history_store_->Save(recent_recordings);
}

void RecordViewModel::ClearCompletedResult() {
    current_completed_recording = CompletedRecording{};
    result_output_path.clear();
    result_destination_text.clear();
    result_stats_text.clear();
    result_output_file_bytes = 0;
    result_elapsed_seconds = 0.0;
}

void RecordViewModel::AddToRecentRecordings(const CompletedRecording& recording) {
    if (!recording.succeeded || recording.file_path.isEmpty())
        return;

    for (int i = 0; i < recent_recordings.size(); ++i) {
        if (recent_recordings[i].file_path == recording.file_path) {
            recent_recordings.removeAt(i);
            break;
        }
    }

    recent_recordings.prepend(recording);

    while (recent_recordings.size() > kMaxRecentRecordings) {
        recent_recordings.removeLast();
    }

    PersistHistory();
}

void RecordViewModel::RemoveFromRecentRecordings(int index) {
    if (index < 0 || index >= recent_recordings.size())
        return;
    recent_recordings.removeAt(index);
    PersistHistory();
}

void RecordViewModel::UpdateRecentRecording(int index, const CompletedRecording& recording) {
    if (index < 0 || index >= recent_recordings.size())
        return;
    recent_recordings[index] = recording;
    PersistHistory();
}

void RecordViewModel::ClearRecentRecordings() {
    recent_recordings.clear();
}

} // namespace exosnap
