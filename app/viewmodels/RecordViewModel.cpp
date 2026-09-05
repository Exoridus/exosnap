#include "RecordViewModel.h"

#include "../diagnostics/error_message.h"
#include "../models/CaptureTargetPresentation.h"
#include "../services/DisplayNumbering.h"
#include "../settings/RecordingHistoryStore.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <optional>
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

std::wstring ContainerLabel(exosnap::engine::Container container) {
    switch (container) {
    case exosnap::engine::Container::Matroska:
        return L"MKV";
    case exosnap::engine::Container::Mp4:
        return L"MP4";
    case exosnap::engine::Container::WebM:
        return L"WebM";
    }
    return L"MKV";
}

std::wstring VideoCodecLabel(exosnap::engine::VideoCodec codec) {
    switch (codec) {
    case exosnap::engine::VideoCodec::H264:
        return L"H.264";
    case exosnap::engine::VideoCodec::Hevc:
        return L"HEVC";
    case exosnap::engine::VideoCodec::Av1:
        return L"AV1";
    }
    return L"AV1";
}

std::wstring AudioCodecLabel(exosnap::engine::AudioCodec codec) {
    switch (codec) {
    case exosnap::engine::AudioCodec::Aac:
        return L"AAC";
    case exosnap::engine::AudioCodec::Opus:
        return L"Opus";
    case exosnap::engine::AudioCodec::Pcm:
        return L"PCM";
    case exosnap::engine::AudioCodec::Flac:
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

bool RecordViewModel::CanCancelPreparing() const noexcept {
    return state == UiRecordingState::Preparing;
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
    case UiRecordingState::Paused:
        state_text = L"Paused";
        break;
    case UiRecordingState::ArmedFromRecovery:
        state_text = L"Paused — recovery ready";
        break;
    case UiRecordingState::Stopping:
        state_text = L"Stopping...";
        break;
    case UiRecordingState::Saving:
        state_text = L"Saving...";
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

void RecordViewModel::UpdateStats(const exosnap::engine::SessionStats& stats) {
    elapsed_text = FormatElapsed(stats.elapsed_seconds);
    elapsed_seconds = stats.elapsed_seconds;
    frames_captured = stats.video_frames_captured;
    video_packets = stats.encoded_video_packets;
    audio_packets = stats.audio_packets;
    video_bytes = stats.video_bytes;
    audio_bytes = stats.audio_bytes;
    output_file_bytes = stats.output_file_bytes;
    // dropped_frames is deliberately NOT fed from stats.dropped_or_skipped_video_frames
    // here: that counter mixes real encoder-backpressure drops together with
    // deliberate CFR pacing/coalescing (frames intentionally discarded to hit
    // the target rate, e.g. a 144 Hz source recorded at 60 fps CFR), which is
    // not a drop. The real-drops-only count arrives with the diagnostics
    // snapshot (see RecordPage's diagnostics callback), same as av_drift_ms
    // below.
    // The muxer only learns the file size when it finalizes a segment, so
    // output_file_bytes stays 0 for the whole recording and the live stat read
    // "0 B" from start to stop. The encoded byte counters ARE live, and their sum
    // is what has been handed to the muxer so far -- close enough to be useful and
    // never larger than the file that results. The real size replaces it in
    // SetResult() once the session ends.
    const uint64_t live_bytes =
        stats.output_file_bytes > 0 ? stats.output_file_bytes : stats.video_bytes + stats.audio_bytes;
    output_size_text = FormatBytes(live_bytes);
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
    const double duration_seconds = ResultDurationSeconds(result);
    result_duration_seconds = duration_seconds;
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
    result_mkv_master_path = result.mkv_master_path;
    result_markers = result.markers;
    result_marker_sidecar_path = result.marker_sidecar_path;

    const auto msg = exosnap::diagnostics::MapErrorToUserMessage(result);
    result_user_title = msg.title;
    result_user_message = msg.message;
    result_action_hint = msg.action_hint;

    if (result.succeeded) {
        const std::wstring elapsed_display = duration_seconds > 0.0 ? FormatElapsed(duration_seconds) : elapsed_text;
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
        cr.duration_seconds = duration_seconds;
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
    av_drift_available = false;
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
    result_container = exosnap::engine::Container::WebM;
    result_video_codec = exosnap::engine::VideoCodec::Av1;
    result_audio_codec = exosnap::engine::AudioCodec::Opus;
    live_stats_available = false;
}

void RecordViewModel::ApplyTargetKind(capability::CaptureTargetKind kind) {
    audio_ui_state.target_kind = kind;
    audio_ui_state.selected_window_pid.reset();
    audio_ui_state.mic_channel_mode = exosnap::engine::MicChannelMode::Auto;

    using K = exosnap::engine::AudioSourceKind;
    if (kind == capability::CaptureTargetKind::Window) {
        // Window: Application audio ON; Other system audio and Microphone OFF by default.
        audio_ui_state.source_rows = {
            {K::App, true, false},
            {K::Mic, false, false},
            {K::Sys, false, false},
        };
    } else {
        // Display/Region: Computer audio ON; Microphone OFF by default.
        //
        // The App row's enabled/merge configuration is a persisted setting like any
        // other, so a target switch must not silently discard it — carry an existing App
        // row over instead of rebuilding without it. Only its ACTIVE state (receded vs.
        // live) follows the target, and the engine strips the row from the recording-time
        // plan anyway (exosnap::engine::NormalizeSourceRowsForTarget, via BuildAudioPlan),
        // since a display/region capture has no process to scope it to.
        const auto existing_app =
            std::find_if(audio_ui_state.source_rows.begin(), audio_ui_state.source_rows.end(),
                         [](const exosnap::engine::AudioSourceRow& r) { return r.kind == K::App; });
        std::optional<exosnap::engine::AudioSourceRow> carried_app;
        if (existing_app != audio_ui_state.source_rows.end())
            carried_app = *existing_app;

        audio_ui_state.source_rows = {
            {K::SystemOutput, true, false},
            {K::Mic, false, false},
        };
        // App keeps its canonical front position in the row order (APP, SYS, MIC).
        if (carried_app.has_value())
            audio_ui_state.source_rows.insert(audio_ui_state.source_rows.begin(), *carried_app);
    }

    RebuildAudioPlan();
}

void RecordViewModel::ApplyTargetKindPreservingAudio(capability::CaptureTargetKind kind) {
    audio_ui_state.target_kind = kind;
    if (kind != capability::CaptureTargetKind::Window) {
        audio_ui_state.selected_window_pid.reset();
    }

    if (kind == capability::CaptureTargetKind::Window) {
        using K = exosnap::engine::AudioSourceKind;
        const bool has_app = std::any_of(audio_ui_state.source_rows.begin(), audio_ui_state.source_rows.end(),
                                         [](const exosnap::engine::AudioSourceRow& r) { return r.kind == K::App; });
        if (!has_app) {
            // App is first in canonical Window row order (App, Mic, Sys).
            audio_ui_state.source_rows.insert(audio_ui_state.source_rows.begin(),
                                              exosnap::engine::AudioSourceRow{K::App, true, false});
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
    return SequentialDisplayLabel(raw_description, {});
}

std::string RecordViewModel::WindowLabelFromTarget(const std::string& raw_description) {
    const exosnap::engine::CaptureTarget target{exosnap::engine::CaptureTarget::Kind::Window, 0, raw_description};
    return ResolveCaptureTargetPresentation(target, CaptureTargetPresentationKind::Window).label;
}

std::string RecordViewModel::TargetLabelFromCaptureTarget(const exosnap::engine::CaptureTarget& target) {
    const CaptureTargetPresentationKind kind = target.kind == exosnap::engine::CaptureTarget::Kind::Window
                                                   ? CaptureTargetPresentationKind::Window
                                                   : CaptureTargetPresentationKind::Display;
    return ResolveCaptureTargetPresentation(target, kind).label;
}

std::string RecordViewModel::LogSafeTargetLabel(const exosnap::engine::CaptureTarget& target) {
    // Privacy (ADR 0045): a window's title (and the app-name/title label built
    // from it) is potentially sensitive -- document titles, chat partner names,
    // private tab titles -- and must be neutralized at the LOG SOURCE, not only
    // when a support bundle is later assembled (the bundle's RedactCaptureTargets
    // pass stays as a defense-in-depth backstop). Monitor descriptions are
    // technical display identifiers ("Desktop - Display 1"), never personal, so
    // they are logged verbatim; only "which window" is replaced with a stable
    // placeholder, matching the [path]/[user]/[machine] convention already used
    // by the crash scrubber.
    if (target.kind == exosnap::engine::CaptureTarget::Kind::Monitor) {
        return TargetLabelFromCaptureTarget(target);
    }
    return "[window]";
}

FilenameTargetContext RecordViewModel::FilenameContextFromCaptureTarget(const exosnap::engine::CaptureTarget& target) {
    const CaptureTargetPresentationKind kind = target.kind == exosnap::engine::CaptureTarget::Kind::Window
                                                   ? CaptureTargetPresentationKind::Window
                                                   : CaptureTargetPresentationKind::Display;
    return ResolveCaptureTargetPresentation(target, kind).filename;
}

std::vector<int> RecordViewModel::SortWindowTargetIndices(const std::vector<exosnap::engine::CaptureTarget>& targets,
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
        if (target.kind != exosnap::engine::CaptureTarget::Kind::Window) {
            continue;
        }

        const CaptureTargetPresentation presentation =
            ResolveCaptureTargetPresentation(target, CaptureTargetPresentationKind::Window);

        entries.push_back({target_index, ToLowerAscii(TrimAscii(presentation.app_name)),
                           ToLowerAscii(TrimAscii(presentation.title))});
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
    result_duration_seconds = 0.0;
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
