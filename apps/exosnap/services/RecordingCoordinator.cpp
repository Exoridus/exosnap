#include "RecordingCoordinator.h"

#include "../../../libs/recorder_core/src/loopback_meter_service.h"
#include "../../../libs/recorder_core/src/mic_meter_service.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <string_view>
#include <thread>

#include "../diagnostics/AppLog.h"
#include "../models/FilenameBuilder.h"
#include "../models/OutputPathValidator.h"
#include "../models/RecordingPreset.h"

namespace exosnap {

static std::wstring ToWide(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

static std::string TrimAscii(const std::string& value) {
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

static bool StartsWithAsciiInsensitive(const std::string_view value, const std::string_view prefix) {
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

static recorder_core::WebcamOverlayLive ToLiveWebcamOverlay(const WebcamSettings& settings) {
    const WebcamSettings sanitized = SanitizeWebcamSettings(settings);
    recorder_core::WebcamOverlayLive overlay;
    overlay.enabled = sanitized.enabled;
    overlay.overlay_x_norm = sanitized.overlay.x_norm;
    overlay.overlay_y_norm = sanitized.overlay.y_norm;
    overlay.overlay_w_norm = sanitized.overlay.w_norm;
    overlay.overlay_h_norm = sanitized.overlay.h_norm;
    overlay.mirror = sanitized.mirror;
    overlay.chroma_key_enabled = sanitized.chroma_key.enabled;
    {
        const auto ac = sanitized.chroma_key.active_color();
        overlay.chroma_r = ac.r;
        overlay.chroma_g = ac.g;
        overlay.chroma_b = ac.b;
    }
    overlay.chroma_tolerance = sanitized.chroma_key.tolerance;
    overlay.chroma_softness = sanitized.chroma_key.softness;
    overlay.chroma_spill_reduction = sanitized.chroma_key.spill_reduction;
    return overlay;
}

static std::string DisplayLabelFromTargetDescription(const std::string& raw_description) {
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

struct WindowTargetParts {
    std::string app_name;
    std::string title;
};

static WindowTargetParts ParseWindowTargetParts(const std::string& raw_description) {
    WindowTargetParts parts{};
    const std::string value = TrimAscii(raw_description);
    if (value.empty()) {
        return parts;
    }

    const std::string separators[] = {" \xE2\x80\x94 ", " - "};
    bool found_separator = false;
    std::size_t separator_pos = 0;
    std::size_t separator_size = 0;
    for (const auto& separator : separators) {
        const std::size_t candidate = value.rfind(separator);
        if (candidate == std::string::npos) {
            continue;
        }
        if (!found_separator || candidate > separator_pos) {
            found_separator = true;
            separator_pos = candidate;
            separator_size = separator.size();
        }
    }

    if (!found_separator) {
        parts.app_name = value;
        parts.title = value;
        return parts;
    }

    const std::string raw_title = TrimAscii(value.substr(0, separator_pos));
    const std::string raw_app = TrimAscii(value.substr(separator_pos + separator_size));

    parts.app_name = raw_app.empty() ? value : raw_app;
    if (!raw_title.empty() && raw_title != parts.app_name) {
        parts.title = raw_title;
    }

    if (parts.title.empty()) {
        parts.title = parts.app_name;
    }

    return parts;
}

static std::string BuildProcessName(const std::string& app_name) {
    std::string process_name;
    process_name.reserve(app_name.size());

    for (const unsigned char ch : app_name) {
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

static FilenameTargetContext BuildFilenameContextFromTarget(const recorder_core::CaptureTarget& target) {
    FilenameTargetContext context;
    if (target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
        const std::string display = DisplayLabelFromTargetDescription(target.description);
        context.app_name = L"Desktop";
        context.process_name = L"desktop";
        context.window_title = ToWide(display);
        context.target_name = L"Desktop - " + context.window_title;
        return context;
    }

    const WindowTargetParts parts = ParseWindowTargetParts(target.description);
    const std::string app = parts.app_name.empty() ? std::string("Window") : parts.app_name;
    const std::string title = parts.title.empty() ? app : parts.title;

    context.app_name = ToWide(app);
    context.window_title = ToWide(title);
    context.process_name = ToWide(BuildProcessName(app));
    context.target_name = context.app_name + L" - " + context.window_title;
    return context;
}

static bool PlanRequiresTargetPid(const recorder_core::AudioTrackPlan& plan) {
    for (const auto& track : plan.tracks) {
        for (const auto source : track.sources) {
            if (source == recorder_core::AudioSourceKind::App || source == recorder_core::AudioSourceKind::Sys) {
                return true;
            }
        }
    }
    return false;
}

void ApplyOutputSettingsToRecorderConfig(recorder_core::RecorderConfig& config, const OutputSettingsModel& settings) {
    config.output_width = 0;
    config.output_height = 0;
    config.output_fit = settings.resolution.fit;
    if (const auto preset_size = PresetOutputSize(settings.resolution.mode)) {
        config.output_width = preset_size->width;
        config.output_height = preset_size->height;
    } else if (settings.resolution.mode == OutputResolutionMode::Custom) {
        if (const auto custom_size = ResolveRequestedOutputSize(
                settings.resolution, {settings.resolution.custom_width, settings.resolution.custom_height})) {
            config.output_width = custom_size->width;
            config.output_height = custom_size->height;
        }
    }

    switch (settings.audio_codec) {
    case capability::AudioCodec::Opus:
        config.audio_codec = recorder_core::AudioCodec::Opus;
        return;
    case capability::AudioCodec::AacMf:
    case capability::AudioCodec::Pcm:
    default:
        config.audio_codec = recorder_core::AudioCodec::AacMf;
        return;
    }
}

static void ApplyOutputSettingsToUserConfig(capability::UserRecorderConfig& config,
                                            const OutputSettingsModel& settings) {
    config.output_width = 0;
    config.output_height = 0;
    if (const auto preset_size = PresetOutputSize(settings.resolution.mode)) {
        config.output_width = preset_size->width;
        config.output_height = preset_size->height;
    } else if (settings.resolution.mode == OutputResolutionMode::Custom) {
        if (const auto custom_size = ResolveRequestedOutputSize(
                settings.resolution, {settings.resolution.custom_width, settings.resolution.custom_height})) {
            config.output_width = custom_size->width;
            config.output_height = custom_size->height;
        }
    }
}

static std::wstring BuildCapabilityStatusText(const capability::UserRecorderConfig& config) {
    const wchar_t* container_name = L"MKV";
    switch (config.container) {
    case capability::Container::WebM:
        container_name = L"WebM";
        break;
    case capability::Container::Mp4:
        container_name = L"MP4";
        break;
    default:
        break;
    }

    const wchar_t* video_name = L"AV1 NVENC";
    switch (config.video_codec) {
    case capability::VideoCodec::H264Nvenc:
        video_name = L"H.264 NVENC";
        break;
    case capability::VideoCodec::HevcNvenc:
        video_name = L"HEVC NVENC";
        break;
    default:
        break;
    }

    const wchar_t* audio_name = L"Opus";
    switch (config.audio_codec) {
    case capability::AudioCodec::AacMf:
        audio_name = L"AAC";
        break;
    case capability::AudioCodec::Pcm:
        audio_name = L"PCM";
        break;
    default:
        break;
    }

    std::wstring result = L"Ready: ";
    result += container_name;
    result += L" \u00B7 ";
    result += video_name;
    result += L" \u00B7 ";
    result += audio_name;
    result += L" \u00B7 ";
    result += std::to_wstring(config.frame_rate_num);
    result += L" fps";
    return result;
}

RecordingCoordinator::RecordingCoordinator()
    : output_settings_(OutputSettingsModel::Defaults()),
      mic_meter_service_(std::make_unique<recorder_core::MicMeterService>()),
      sys_meter_service_(std::make_unique<recorder_core::LoopbackMeterService>()),
      app_meter_service_(std::make_unique<recorder_core::LoopbackMeterService>()) {
}

RecordingCoordinator::~RecordingCoordinator() {
    StopMicMeter();
    StopSysMeter();
    StopAppMeter();
    if (is_recording_) {
        session_.Stop();
    }
}

void RecordingCoordinator::OnCapabilitiesReady(const exosnap::capability::CapabilitySet& caps,
                                               const exosnap::capability::ResolveResult& validation) {
    caps_ = caps;
    has_caps_ = true;
    validation_result_ = validation;
    resolved_user_config_ = validation.resolved_config;
    if (validation.succeeded) {
        state_ = UiRecordingState::Ready;
        capability_status_text_ = BuildCapabilityStatusText(validation.resolved_config);
    } else {
        state_ = UiRecordingState::Blocked;
        capability_status_text_ =
            validation.invalidity.empty() ? L"Recording unavailable" : ToWide(validation.invalidity.front().message);
    }
}

void RecordingCoordinator::OnCapabilityFailure(std::wstring message) {
    diagnostics::AppLog::error(
        QStringLiteral("record.failure"),
        QStringLiteral("phase=Init category=CapabilityCheck detail=\"%1\"").arg(QString::fromStdWString(message)));
    has_caps_ = false;
    state_ = UiRecordingState::Blocked;
    capability_status_text_ = std::move(message);
}

void RecordingCoordinator::RevalidateCapabilities() {
    if (!has_caps_)
        return;
    const bool busy = state_ == UiRecordingState::Preparing || state_ == UiRecordingState::Recording ||
                      state_ == UiRecordingState::Paused || state_ == UiRecordingState::Stopping;
    if (busy)
        return;

    capability::SettingsResolver resolver(caps_);
    const capability::ResolveResult result = resolver.ValidateConfig(resolved_user_config_);
    validation_result_ = result;

    const UiRecordingState new_state = result.succeeded ? UiRecordingState::Ready : UiRecordingState::Blocked;
    capability_status_text_ =
        result.succeeded
            ? BuildCapabilityStatusText(result.resolved_config)
            : (result.invalidity.empty() ? L"Recording unavailable" : ToWide(result.invalidity.front().message));

    if (new_state != state_)
        PostStateChange(new_state);
}

std::vector<recorder_core::CaptureTarget> RecordingCoordinator::EnumerateTargets() {
    return recorder_core::RecorderSession::EnumerateTargets();
}

void RecordingCoordinator::SetWebcamSettings(const WebcamSettings& settings) {
    const WebcamSettings sanitized = SanitizeWebcamSettings(settings);
    const bool device_changed = sanitized.device_id != webcam_settings_.device_id;
    const bool res_changed = sanitized.width != webcam_settings_.width || sanitized.height != webcam_settings_.height;
    const bool fps_changed = sanitized.fps != webcam_settings_.fps;
    webcam_settings_ = sanitized;

    const bool recording = is_recording_.load();
    if (recording) {
        session_.UpdateWebcamOverlay(ToLiveWebcamOverlay(webcam_settings_));
    }

    // A device/resolution/fps change requires re-opening the capture, so do not
    // restart the webcam device mid-recording. Live overlay fields are pushed
    // above and enable/disable is handled by SyncWebcamService.
    SyncWebcamService((device_changed || res_changed || fps_changed) && !recording);
}

void RecordingCoordinator::SetWebcamPreviewActive(bool active) {
    webcam_preview_active_ = active;
    SyncWebcamService(false);
}

void RecordingCoordinator::SyncWebcamService(bool force_restart) {
    // Recording always owns the device; while idle the capture runs only when the
    // Record preview asked for it (live Ready PiP) and webcam is enabled.
    const bool want_running = webcam_settings_.enabled && (is_recording_.load() || webcam_preview_active_);
    if (!want_running) {
        webcam_service_.Stop();
        return;
    }
    if (force_restart || !webcam_service_.IsRunning()) {
        webcam_service_.Stop();
        webcam_service_.Start(webcam_settings_.device_id, webcam_settings_.width, webcam_settings_.height,
                              webcam_settings_.fps);
    }
}

void RecordingCoordinator::SetWebcamFrameCallback(WebcamService::FrameCallback cb) {
    webcam_service_.SetFrameCallback(std::move(cb));
}

bool RecordingCoordinator::StartRecording(const recorder_core::CaptureTarget& target,
                                          const capability::AudioUiState& audio_ui_state,
                                          std::optional<recorder_core::CaptureRegion> crop_region) {
    StopMicMeter();

    if (is_recording_)
        return false;
    const auto folder_check = ValidateOutputFolder(output_settings_.output_folder);
    if (folder_check != FolderValidationResult::Ok) {
        diagnostics::AppLog::error(QStringLiteral("record.failure"),
                                   QStringLiteral("phase=Prepare category=OutputFolder output_folder=\"%1\" detail=%2")
                                       .arg(QString::fromStdWString(output_settings_.output_folder.wstring()),
                                            QString::fromStdWString(FolderValidationMessage(folder_check))));

        PostStateChange(UiRecordingState::Failed);

        UiRecordingResult result;
        result.succeeded = false;
        result.error_phase = FormatErrorPhase(recorder_core::ErrorPhase::Prepare);
        result.error_detail = FolderValidationMessage(folder_check);
        PostResult(std::move(result));

        return false;
    }
    if (state_ != UiRecordingState::Ready && state_ != UiRecordingState::Completed &&
        state_ != UiRecordingState::Failed) {
        return false;
    }
    if (!has_caps_)
        return false;

    if (!has_output_target_context_) {
        output_target_context_ = BuildFilenameContextFromTarget(target);
    }

    auto output_path = GenerateOutputPath();
    const auto resolved_path = ResolveAvailableOutputPath(output_path);
    if (!resolved_path.has_value()) {
        diagnostics::AppLog::error(QStringLiteral("record.failure"),
                                   QStringLiteral("phase=Prepare category=FilenameCollision output_folder=\"%1\" "
                                                  "detail=\"Collision resolution exhausted\"")
                                       .arg(QString::fromStdWString(output_settings_.output_folder.wstring())));

        PostStateChange(UiRecordingState::Failed);
        UiRecordingResult result;
        result.succeeded = false;
        result.output_path = output_path.wstring();
        result.error_detail =
            L"Could not create a unique output filename. Change the filename pattern or output folder.";
        PostResult(std::move(result));
        return false;
    }
    output_path = *resolved_path;
    current_output_path_ = output_path;

    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);
    if (ec) {
        diagnostics::AppLog::error(
            QStringLiteral("record.failure"),
            QStringLiteral("phase=Prepare category=CreateDirectory output_path=\"%1\" detail=\"%2\"")
                .arg(QString::fromStdWString(output_path.wstring()), QString::fromStdWString(ToWide(ec.message()))));

        PostStateChange(UiRecordingState::Failed);
        UiRecordingResult result;
        result.succeeded = false;
        result.error_detail = L"Failed to create output directory: " + ToWide(ec.message());
        PostResult(std::move(result));
        return false;
    }

    PostStateChange(UiRecordingState::Preparing);

    auto config = exosnap::capability::ToRecorderCoreConfig(resolved_user_config_, caps_);
    config.nvenc_quality_preset = video_settings_.quality;
    config.frame_rate_num = video_settings_.frame_rate_num;
    config.frame_rate_den = video_settings_.frame_rate_den;
    config.cfr = video_settings_.cfr;
    if (config.container == recorder_core::Container::Mp4 && !config.cfr) {
        diagnostics::AppLog::warning(
            QStringLiteral("record.reconcile"),
            QStringLiteral("field=timing requested=VFR effective=CFR reason=\"MP4 mux path is fixed-rate\""));
        config.cfr = true;
    }
    config.capture_cursor = video_settings_.capture_cursor;
    ApplyOutputSettingsToRecorderConfig(config, output_settings_);
    config.target = target;
    config.crop_region = crop_region;
    config.output_path = output_path;
    config.split = split_settings_;

    config.webcam.enabled = webcam_settings_.enabled;
    config.webcam.frame_provider = &webcam_service_;
    config.webcam.overlay_x_norm = webcam_settings_.overlay.x_norm;
    config.webcam.overlay_y_norm = webcam_settings_.overlay.y_norm;
    config.webcam.overlay_w_norm = webcam_settings_.overlay.w_norm;
    config.webcam.overlay_h_norm = webcam_settings_.overlay.h_norm;
    config.webcam.mirror = webcam_settings_.mirror;
    config.webcam.chroma_key_enabled = webcam_settings_.chroma_key.enabled;
    {
        const auto ac = webcam_settings_.chroma_key.active_color();
        config.webcam.chroma_r = ac.r;
        config.webcam.chroma_g = ac.g;
        config.webcam.chroma_b = ac.b;
    }
    config.webcam.chroma_tolerance = webcam_settings_.chroma_key.tolerance;
    config.webcam.chroma_softness = webcam_settings_.chroma_key.softness;
    config.webcam.chroma_spill_reduction = webcam_settings_.chroma_key.spill_reduction;

    capability::AudioUiState audio_state = audio_ui_state;
    if (target.kind == recorder_core::CaptureTarget::Kind::Window && target.native_id != 0) {
        HWND hwnd = reinterpret_cast<HWND>(target.native_id);
        DWORD pid = 0;
        if (::GetWindowThreadProcessId(hwnd, &pid) != 0 && pid != 0) {
            audio_state.selected_window_pid = static_cast<uint32_t>(pid);
        }
    }

    const capability::AudioPlanResult plan = capability::BuildAudioPlan(audio_state);

    config.record_audio = plan.record_audio;
    config.audio_track_plan = plan.plan;
    config.audio_target_process_id = plan.audio_target_process_id;
    config.mic_channel_mode = plan.mic_channel_mode;
    config.mic_device_id = plan.mic_device_id;
    config.mic_gain_linear = plan.mic_gain_linear;

    if (plan.record_audio && PlanRequiresTargetPid(plan.plan) && !plan.audio_target_process_id.has_value()) {
        diagnostics::AppLog::error(QStringLiteral("record.failure"),
                                   QStringLiteral("phase=Prepare category=TargetPid output_path=\"%1\" "
                                                  "detail=\"Window target PID unavailable\"")
                                       .arg(QString::fromStdWString(output_path.wstring())));

        PostStateChange(UiRecordingState::Failed);
        UiRecordingResult result;
        result.succeeded = false;
        result.output_path = output_path.wstring();
        result.error_phase = FormatErrorPhase(recorder_core::ErrorPhase::Prepare);
        result.error_detail = L"Window target PID unavailable; the selected window may have been closed.";
        PostResult(std::move(result));
        return false;
    }

    recorder_core::RecorderResult validate_result;
    if (!session_.Validate(config, &validate_result)) {
        diagnostics::AppLog::error(
            QStringLiteral("record.failure"),
            QStringLiteral("phase=Validate category=SessionValidate output_path=\"%1\" detail=\"%2\"")
                .arg(QString::fromStdWString(output_path.wstring()),
                     QString::fromStdWString(ToWide(validate_result.error_detail))));

        PostStateChange(UiRecordingState::Failed);
        UiRecordingResult result;
        result.succeeded = false;
        result.output_path = output_path.wstring();
        result.error_phase = FormatErrorPhase(validate_result.error_phase);
        result.hresult_text = FormatHResult(validate_result.error_code);
        result.error_detail = ToWide(validate_result.error_detail);
        PostResult(std::move(result));
        return false;
    }

    session_.SetStatsCallback([this](const recorder_core::SessionStats& stats) { PostStats(stats); });
    session_.SetMeterCallback([this](const recorder_core::MeterSnapshot& m) { PostRecordingMeter(m.per_track_rms); });
    {
        std::lock_guard<std::mutex> lock(segments_mutex_);
        segments_.clear();
    }
    split_pending_.store(false);
    session_.SetSegmentCallback([this](const recorder_core::CompletedSegment& seg) { OnSegmentCompleted(seg); });

    if (webcam_settings_.enabled) {
        webcam_service_.Stop();
        webcam_service_.Start(webcam_settings_.device_id, webcam_settings_.width, webcam_settings_.height,
                              webcam_settings_.fps);
    } else {
        webcam_service_.Stop();
    }

    is_recording_ = true;

    {
        std::lock_guard<std::mutex> lock(markers_mutex_);
        markers_.clear();
        last_elapsed_seconds_ = 0.0;
        markers_limit_reported_ = false;
    }

    PostStateChange(UiRecordingState::Recording);

    {
        const bool is_monitor = (target.kind == recorder_core::CaptureTarget::Kind::Monitor);
        const QString backend = is_monitor ? QStringLiteral("dxgi_od") : QStringLiteral("wgc");
        const QString target_desc = QString::fromStdString(target.description);
        diagnostics::AppLog::info(QStringLiteral("record"),
                                  QStringLiteral("start backend=%1 target=\"%2\"").arg(backend, target_desc));
    }

    recording_thread_ = std::jthread([this, cfg = std::move(config), op = std::move(output_path)](std::stop_token) {
        RecordingThreadProc(cfg, op);
    });

    return true;
}

void RecordingCoordinator::StopRecording() {
    if (!is_recording_)
        return;
    is_paused_.store(false);
    PostStateChange(UiRecordingState::Stopping);
    session_.Stop();
}

void RecordingCoordinator::PauseRecording() {
    if (!is_recording_ || is_paused_.load())
        return;
    is_paused_.store(true);
    session_.Pause();
    PostStateChange(UiRecordingState::Paused);
}

void RecordingCoordinator::ResumeRecording() {
    if (!is_paused_.load())
        return;
    is_paused_.store(false);
    session_.Resume();
    PostStateChange(UiRecordingState::Recording);
}

static const char* SplitTriggerName(recorder_core::SplitTriggerSource source) {
    switch (source) {
    case recorder_core::SplitTriggerSource::AutomaticDuration:
        return "auto";
    case recorder_core::SplitTriggerSource::ManualButton:
        return "button";
    case recorder_core::SplitTriggerSource::Hotkey:
        return "hotkey";
    }
    return "unknown";
}

bool RecordingCoordinator::RequestSplit(recorder_core::SplitTriggerSource source) {
    // Central state validation: a split is meaningful only during an active
    // session. Reject honestly elsewhere instead of silently swallowing.
    const auto st = State();
    if (!is_recording_.load() || (st != UiRecordingState::Recording && st != UiRecordingState::Paused)) {
        diagnostics::AppLog::warning(QStringLiteral("split"),
                                     QStringLiteral("rejected: not recording (state=%1 source=%2)")
                                         .arg(static_cast<int>(st))
                                         .arg(QLatin1String(SplitTriggerName(source))));
        PostSplitFeedback(false, QStringLiteral("Split is only available while recording."));
        return false;
    }

    // Coalesce concurrent requests: only one boundary may be pending at a time.
    // The engine also coalesces via a monotonic seq, but rejecting here keeps the
    // UI feedback honest ("already splitting") and avoids spurious toasts.
    bool expected = false;
    if (!split_pending_.compare_exchange_strong(expected, true)) {
        diagnostics::AppLog::info(QStringLiteral("split"),
                                  QStringLiteral("coalesced: split already pending (source=%1)")
                                      .arg(QLatin1String(SplitTriggerName(source))));
        return false;
    }

    diagnostics::AppLog::info(QStringLiteral("split"), QStringLiteral("requested source=%1 paused=%2")
                                                           .arg(QLatin1String(SplitTriggerName(source)))
                                                           .arg(st == UiRecordingState::Paused));
    session_.RequestSplit(source);
    return true;
}

bool RecordingCoordinator::IsSplitPending() const noexcept {
    return split_pending_.load();
}

void RecordingCoordinator::SetSplitSettings(const recorder_core::RecordingSplitSettings& settings) {
    split_settings_ = settings;
}

recorder_core::RecordingSplitSettings RecordingCoordinator::SplitSettings() const noexcept {
    return split_settings_;
}

void RecordingCoordinator::SetSplitFeedbackCallback(SplitFeedbackCallback cb) {
    on_split_feedback_ = std::move(cb);
}

void RecordingCoordinator::PostSplitFeedback(bool accepted, QString message) {
    if (!on_split_feedback_)
        return;
    auto cb = on_split_feedback_;
    if (QCoreApplication::instance() == nullptr) {
        cb(accepted, message);
        return;
    }
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [cb, accepted, message = std::move(message)]() { cb(accepted, message); },
        Qt::QueuedConnection);
}

void RecordingCoordinator::OnSegmentCompleted(const recorder_core::CompletedSegment& segment) {
    // Fired from the mux worker thread as each segment is finalized (including the
    // final one at session end). Accumulate for the multi-segment result and clear
    // the pending flag — the next segment has started.
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(segments_mutex_);
        segments_.push_back(segment);
        total = segments_.size();
    }
    split_pending_.store(false);

    diagnostics::AppLog::info(QStringLiteral("split"),
                              QStringLiteral("segment finalized index=%1 duration_ms=%2 bytes=%3 ok=%4 path=%5")
                                  .arg(segment.index)
                                  .arg(segment.duration_ms)
                                  .arg(segment.file_size_bytes)
                                  .arg(segment.succeeded)
                                  .arg(QString::fromStdWString(segment.path.filename().wstring())));

    // Feedback names the segment that just *started* (the one after the boundary).
    // total counts finalized segments; the new live segment index is total (0-based)
    // i.e. human-friendly part number total+1. Only surface this while still
    // recording (the final session-end finalize also fires this callback).
    if (is_recording_.load() && segment.succeeded) {
        const qulonglong next_part = static_cast<qulonglong>(total) + 1;
        PostSplitFeedback(true, QStringLiteral("Started segment %1").arg(next_part));
    }
}

bool RecordingCoordinator::StartMicMeter(std::optional<std::string> device_id,
                                         recorder_core::MicChannelMode channel_mode) {
    if (!mic_meter_service_) {
        return false;
    }

    if (state_ == UiRecordingState::Preparing || state_ == UiRecordingState::Stopping) {
        return false;
    }

    const bool same_device = mic_meter_device_id_ == device_id;
    const bool same_channel = mic_meter_channel_mode_ == channel_mode;
    if (mic_meter_service_->IsRunning() && mic_meter_config_valid_ && same_device && same_channel) {
        return true;
    }

    StopMicMeter();

    std::string error;
    const bool started = mic_meter_service_->Start(
        device_id, channel_mode, [this](float rms_linear) { PostMicMeter(rms_linear); }, error);
    if (!started) {
        mic_meter_config_valid_ = false;
        mic_meter_device_id_.reset();
        return false;
    }

    mic_meter_device_id_ = std::move(device_id);
    mic_meter_channel_mode_ = channel_mode;
    mic_meter_config_valid_ = true;
    return true;
}

void RecordingCoordinator::StopMicMeter() {
    if (mic_meter_service_) {
        mic_meter_service_->Stop();
    }
    mic_meter_config_valid_ = false;
    mic_meter_device_id_.reset();
}

bool RecordingCoordinator::IsMicMeterRunning() const noexcept {
    return mic_meter_service_ && mic_meter_service_->IsRunning();
}

bool RecordingCoordinator::StartSysMeter() {
    if (!sys_meter_service_) {
        return false;
    }
    if (state_ == UiRecordingState::Preparing || state_ == UiRecordingState::Stopping) {
        return false;
    }
    if (sys_meter_service_->IsRunning()) {
        return true;
    }
    std::string error;
    return sys_meter_service_->Start(0u, [this](float rms_linear) { PostSysMeter(rms_linear); }, error);
}

void RecordingCoordinator::StopSysMeter() {
    if (sys_meter_service_) {
        sys_meter_service_->Stop();
    }
}

bool RecordingCoordinator::IsSysMeterRunning() const noexcept {
    return sys_meter_service_ && sys_meter_service_->IsRunning();
}

bool RecordingCoordinator::StartAppMeter(uint32_t target_pid) {
    if (!app_meter_service_ || target_pid == 0) {
        return false;
    }
    if (state_ == UiRecordingState::Preparing || state_ == UiRecordingState::Stopping) {
        return false;
    }
    if (app_meter_service_->IsRunning()) {
        return true;
    }
    std::string error;
    return app_meter_service_->Start(target_pid, [this](float rms_linear) { PostAppMeter(rms_linear); }, error);
}

void RecordingCoordinator::StopAppMeter() {
    if (app_meter_service_) {
        app_meter_service_->Stop();
    }
}

bool RecordingCoordinator::IsAppMeterRunning() const noexcept {
    return app_meter_service_ && app_meter_service_->IsRunning();
}

void RecordingCoordinator::RecordingThreadProc(const recorder_core::RecorderConfig& config,
                                               const std::filesystem::path& output_path) {
    auto result = session_.Record(config);
    is_recording_ = false;
    is_paused_.store(false);

    UiRecordingResult ui_result;
    ui_result.succeeded = result.succeeded;
    ui_result.output_path = output_path.wstring();
    ui_result.error_phase = FormatErrorPhase(result.error_phase);
    ui_result.hresult_text = FormatHResult(result.error_code);
    ui_result.error_detail = ToWide(result.error_detail);
    ui_result.output_file_bytes = result.stats.output_file_bytes;
    ui_result.elapsed_seconds = result.stats.elapsed_seconds;
    ui_result.source_width = result.stats.source_size.width;
    ui_result.source_height = result.stats.source_size.height;
    ui_result.output_width = result.stats.output_size.width;
    ui_result.output_height = result.stats.output_size.height;
    ui_result.content_rect = result.stats.content_rect;
    ui_result.frame_rate_num = result.stats.frame_rate_num;
    ui_result.frame_rate_den = result.stats.frame_rate_den;
    ui_result.cfr = result.stats.cfr;
    ui_result.container = result.stats.container;
    ui_result.video_codec = result.stats.video_codec;
    ui_result.audio_codec = result.stats.audio_codec;

    {
        std::lock_guard<std::mutex> lock(markers_mutex_);
        ui_result.markers = markers_;
        ui_result.marker_sidecar_path = MarkerSidecarPath().wstring();
    }

    {
        std::lock_guard<std::mutex> lock(segments_mutex_);
        ui_result.segments.reserve(segments_.size());
        for (const auto& seg : segments_) {
            CompletedRecordingSegment out;
            out.file_path = QString::fromStdWString(seg.path.wstring());
            out.index = seg.index;
            out.session_start_ms = seg.session_start_ms;
            out.duration_seconds = static_cast<double>(seg.duration_ms) / 1000.0;
            out.file_size_bytes = static_cast<qint64>(seg.file_size_bytes);
            out.succeeded = seg.succeeded;
            ui_result.segments.push_back(std::move(out));
        }
    }
    split_pending_.store(false);

    if (result.succeeded && !markers_.empty()) {
        WriteMarkerSidecar();
        diagnostics::AppLog::info(QStringLiteral("marker"),
                                  QStringLiteral("sidecar finalized markers=%1 path=%2")
                                      .arg(markers_.size())
                                      .arg(QString::fromStdWString(MarkerSidecarPath().wstring())));
    }

    if (!result.succeeded) {
        diagnostics::AppLog::error(QStringLiteral("record.failure"),
                                   QStringLiteral("phase=%1 output_path=\"%2\" detail=\"%3\"")
                                       .arg(QString::fromStdWString(ui_result.error_phase),
                                            QString::fromStdWString(ui_result.output_path),
                                            QString::fromStdWString(ui_result.error_detail)));
    }

    PostResult(std::move(ui_result));
    PostStateChange(result.succeeded ? UiRecordingState::Completed : UiRecordingState::Failed);
    // is_recording_ is already false here; restore the idle preview capture if the
    // Record page still wants a live PiP, otherwise release the device.
    SyncWebcamService(false);
}

UiRecordingState RecordingCoordinator::State() const noexcept {
    return state_;
}
const std::wstring& RecordingCoordinator::CapabilityStatusText() const {
    return capability_status_text_;
}

std::wstring RecordingCoordinator::ResolvedVideoCodecLabel() const {
    switch (resolved_user_config_.video_codec) {
    case capability::VideoCodec::H264Nvenc:
        return L"H.264 NVENC encoder";
    case capability::VideoCodec::HevcNvenc:
        return L"HEVC NVENC encoder";
    default:
        return L"AV1 NVENC encoder";
    }
}
std::filesystem::path RecordingCoordinator::CurrentOutputPath() const {
    return current_output_path_;
}
void RecordingCoordinator::SetOutputSettings(const OutputSettingsModel& settings) {
    output_settings_ = settings;
    const OutputSettingsModel requested = output_settings_;
    ReconcileContainerCodecs(output_settings_);
    SanitizeOutputResolution(output_settings_.resolution);
    if (requested.video_codec != output_settings_.video_codec ||
        requested.audio_codec != output_settings_.audio_codec) {
        diagnostics::AppLog::warning(QStringLiteral("record.reconcile"),
                                     QStringLiteral("field=codec container=%1 video=%2 audio=%3")
                                         .arg(static_cast<int>(output_settings_.container))
                                         .arg(static_cast<int>(output_settings_.video_codec))
                                         .arg(static_cast<int>(output_settings_.audio_codec)));
    }

    resolved_user_config_.container = output_settings_.container;
    resolved_user_config_.video_codec = output_settings_.video_codec;
    resolved_user_config_.audio_codec = output_settings_.audio_codec;
    ApplyOutputSettingsToUserConfig(resolved_user_config_, output_settings_);
}
void RecordingCoordinator::SetVideoSettings(const VideoSettingsModel& settings) {
    video_settings_ = settings;
    if (video_settings_.frame_rate_num == 0 || video_settings_.frame_rate_den == 0) {
        video_settings_.frame_rate_num = 60;
        video_settings_.frame_rate_den = 1;
    }
    resolved_user_config_.frame_rate_num = video_settings_.frame_rate_num;
    resolved_user_config_.frame_rate_den = video_settings_.frame_rate_den;
}
void RecordingCoordinator::SetOutputTargetContext(const FilenameTargetContext& context) {
    output_target_context_ = context;
    has_output_target_context_ = true;
}

void RecordingCoordinator::SetStateChangedCallback(StateChangedCallback cb) {
    on_state_changed_ = std::move(cb);
}
void RecordingCoordinator::SetStatsUpdatedCallback(StatsUpdatedCallback cb) {
    on_stats_updated_ = std::move(cb);
}
void RecordingCoordinator::SetResultReadyCallback(ResultReadyCallback cb) {
    on_result_ready_ = std::move(cb);
}

void RecordingCoordinator::SetMicMeterUpdatedCallback(MicMeterUpdatedCallback cb) {
    on_mic_meter_updated_ = std::move(cb);
}
void RecordingCoordinator::SetSysMeterUpdatedCallback(SysMeterUpdatedCallback cb) {
    on_sys_meter_updated_ = std::move(cb);
}
void RecordingCoordinator::SetAppMeterUpdatedCallback(AppMeterUpdatedCallback cb) {
    on_app_meter_updated_ = std::move(cb);
}
void RecordingCoordinator::SetRecordingMeterCallback(RecordingMeterCallback cb) {
    on_recording_meter_updated_ = std::move(cb);
}

void RecordingCoordinator::SetFrameCapturedCallback(FrameCapturedCallback cb) {
    on_frame_captured_ = std::move(cb);
}

void RecordingCoordinator::SetReadyFrameSource(std::function<QImage()> getter) {
    ready_frame_source_ = std::move(getter);
}

void RecordingCoordinator::CaptureFrame() {
    using diagnostics::AppLog;

    const auto st = State();
    if (st == UiRecordingState::Recording || st == UiRecordingState::Paused) {
        AppLog::info(QStringLiteral("capture_frame"), QStringLiteral("snapshot requested"));

        // Capture everything needed by value — the callback fires from VideoThread;
        // the coordinator is alive because the recording thread is joined before
        // destruction, but we still avoid capturing 'this' for safety.
        const FrameCapturedCallback cb_copy = on_frame_captured_;
        const std::wstring folder = output_settings_.output_folder.wstring();
        const bool has_ctx = has_output_target_context_;
        const FilenameTargetContext ctx = output_target_context_;

        session_.RequestFrameSnapshot([cb_copy, folder, has_ctx, ctx](bool ok, uint32_t w, uint32_t h,
                                                                      std::vector<uint8_t> bgra,
                                                                      const std::string& err) mutable {
            if (!ok) {
                AppLog::warning(QStringLiteral("capture_frame"),
                                QStringLiteral("readback failed: ") + QString::fromStdString(err));
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [cb_copy, msg = QString::fromStdString(err)]() mutable {
                        if (cb_copy)
                            cb_copy(false, {}, msg);
                    },
                    Qt::QueuedConnection);
                return;
            }

            // Build output path (collision-safe) from folder and context.
            const QString dir_path = QString::fromStdWString(folder);
            const QString datetime = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
            QString name_base = datetime;
            if (has_ctx && !ctx.target_name.empty()) {
                QString tname = QString::fromStdWString(ctx.target_name);
                // Sanitize
                static const QRegularExpression kBad(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
                tname.replace(kBad, QStringLiteral("_"));
                if (tname.length() > 32)
                    tname = tname.left(32);
                name_base += QStringLiteral("_") + tname;
            }
            name_base += QStringLiteral("_frame");

            QDir out_dir(dir_path);
            if (!out_dir.exists())
                out_dir.mkpath(QStringLiteral("."));

            auto uniquePath = [&]() -> QString {
                QString p = out_dir.absoluteFilePath(name_base + QStringLiteral(".png"));
                if (!QFileInfo::exists(p))
                    return p;
                for (int s = 1; s <= 999; ++s) {
                    p = out_dir.absoluteFilePath(QStringLiteral("%1_%2.png").arg(name_base).arg(s, 3, 10, QChar('0')));
                    if (!QFileInfo::exists(p))
                        return p;
                }
                return p;
            };
            const QString out_path = uniquePath();

            // PNG write off the VideoThread (detached worker thread).
            std::thread([cb_copy, w, h, bgra = std::move(bgra), out_path]() mutable {
                // BGRA bytes → QImage (Format_BGRA8888 = B G R A in memory order)
                QImage img(static_cast<int>(w), static_cast<int>(h), QImage::Format_ARGB32);
                std::memcpy(img.bits(), bgra.data(), bgra.size());

                const QString tmp_path = out_path + QStringLiteral(".tmp");
                bool saved = img.save(tmp_path, "PNG");
                if (saved) {
                    if (QFileInfo::exists(out_path))
                        QFile::remove(out_path);
                    saved = QFile::rename(tmp_path, out_path);
                }
                if (!saved)
                    QFile::remove(tmp_path);

                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [cb_copy, saved, out_path]() mutable {
                        if (saved) {
                            AppLog::info(QStringLiteral("capture_frame"), QStringLiteral("frame saved: ") + out_path);
                            if (cb_copy)
                                cb_copy(true, out_path, {});
                        } else {
                            AppLog::warning(QStringLiteral("capture_frame"),
                                            QStringLiteral("PNG write failed: ") + out_path);
                            if (cb_copy)
                                cb_copy(false, {}, QStringLiteral("Failed to write PNG file"));
                        }
                    },
                    Qt::QueuedConnection);
            }).detach();
        });
        return;
    }

    if (st == UiRecordingState::Ready) {
        if (!ready_frame_source_) {
            AppLog::warning(QStringLiteral("capture_frame"), QStringLiteral("no preview frame source in Ready state"));
            if (on_frame_captured_)
                on_frame_captured_(false, {}, QStringLiteral("No preview frame available"));
            return;
        }
        QImage frame = ready_frame_source_();
        if (frame.isNull()) {
            AppLog::warning(QStringLiteral("capture_frame"), QStringLiteral("preview frame not yet available"));
            if (on_frame_captured_)
                on_frame_captured_(false, {}, QStringLiteral("No preview frame available yet"));
            return;
        }

        AppLog::info(QStringLiteral("capture_frame"), QStringLiteral("snapshot from preview (Ready)"));

        const std::wstring folder = output_settings_.output_folder.wstring();
        const bool has_ctx = has_output_target_context_;
        const FilenameTargetContext ctx = output_target_context_;
        const FrameCapturedCallback cb_copy = on_frame_captured_;

        std::thread([cb_copy, frame = std::move(frame), folder, has_ctx, ctx]() mutable {
            const QString dir_path = QString::fromStdWString(folder);
            const QString datetime = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
            QString name_base = datetime;
            if (has_ctx && !ctx.target_name.empty()) {
                QString tname = QString::fromStdWString(ctx.target_name);
                static const QRegularExpression kBad(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
                tname.replace(kBad, QStringLiteral("_"));
                if (tname.length() > 32)
                    tname = tname.left(32);
                name_base += QStringLiteral("_") + tname;
            }
            name_base += QStringLiteral("_frame");

            QDir out_dir(dir_path);
            if (!out_dir.exists())
                out_dir.mkpath(QStringLiteral("."));

            QString out_path = out_dir.absoluteFilePath(name_base + QStringLiteral(".png"));
            if (QFileInfo::exists(out_path)) {
                for (int s = 1; s <= 999; ++s) {
                    out_path =
                        out_dir.absoluteFilePath(QStringLiteral("%1_%2.png").arg(name_base).arg(s, 3, 10, QChar('0')));
                    if (!QFileInfo::exists(out_path))
                        break;
                }
            }

            const QString tmp_path = out_path + QStringLiteral(".tmp");
            bool saved = frame.save(tmp_path, "PNG");
            if (saved) {
                if (QFileInfo::exists(out_path))
                    QFile::remove(out_path);
                saved = QFile::rename(tmp_path, out_path);
            }
            if (!saved)
                QFile::remove(tmp_path);

            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [cb_copy, saved, out_path]() mutable {
                    if (saved) {
                        AppLog::info(QStringLiteral("capture_frame"),
                                     QStringLiteral("frame saved (Ready): ") + out_path);
                        if (cb_copy)
                            cb_copy(true, out_path, {});
                    } else {
                        AppLog::warning(QStringLiteral("capture_frame"),
                                        QStringLiteral("PNG write failed (Ready): ") + out_path);
                        if (cb_copy)
                            cb_copy(false, {}, QStringLiteral("Failed to write PNG file"));
                    }
                },
                Qt::QueuedConnection);
        }).detach();
        return;
    }

    // Unsupported state
    AppLog::warning(QStringLiteral("capture_frame"),
                    QStringLiteral("rejected: unsupported state %1").arg(static_cast<int>(st)));
    if (on_frame_captured_)
        on_frame_captured_(false, {}, QStringLiteral("Capture frame is not available in this state"));
}

void RecordingCoordinator::PostStateChange(UiRecordingState new_state) {
    state_ = new_state;
    if (!on_state_changed_)
        return;
    auto cb = on_state_changed_;
    QMetaObject::invokeMethod(QCoreApplication::instance(), [cb, new_state]() { cb(new_state); }, Qt::QueuedConnection);
}

void RecordingCoordinator::PostResult(UiRecordingResult result) {
    if (!on_result_ready_)
        return;
    auto cb = on_result_ready_;
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [cb, r = std::move(result)]() { cb(r); }, Qt::QueuedConnection);
}

void RecordingCoordinator::PostStats(recorder_core::SessionStats stats) {
    {
        std::lock_guard<std::mutex> lock(markers_mutex_);
        last_elapsed_seconds_ = stats.elapsed_seconds;
    }
    if (!on_stats_updated_)
        return;
    auto cb = on_stats_updated_;
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [cb, s = std::move(stats)]() { cb(s); }, Qt::QueuedConnection);
}

void RecordingCoordinator::PostMicMeter(float rms_linear) {
    if (!on_mic_meter_updated_) {
        return;
    }

    auto cb = on_mic_meter_updated_;
    if (QCoreApplication::instance() == nullptr) {
        cb(rms_linear);
        return;
    }

    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [cb, rms_linear]() { cb(rms_linear); }, Qt::QueuedConnection);
}

void RecordingCoordinator::PostSysMeter(float rms_linear) {
    if (!on_sys_meter_updated_) {
        return;
    }
    auto cb = on_sys_meter_updated_;
    if (QCoreApplication::instance() == nullptr) {
        cb(rms_linear);
        return;
    }
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [cb, rms_linear]() { cb(rms_linear); }, Qt::QueuedConnection);
}

void RecordingCoordinator::PostAppMeter(float rms_linear) {
    if (!on_app_meter_updated_) {
        return;
    }
    auto cb = on_app_meter_updated_;
    if (QCoreApplication::instance() == nullptr) {
        cb(rms_linear);
        return;
    }
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [cb, rms_linear]() { cb(rms_linear); }, Qt::QueuedConnection);
}

void RecordingCoordinator::PostRecordingMeter(std::array<float, 3> per_track_rms) {
    if (!on_recording_meter_updated_) {
        return;
    }
    auto cb = on_recording_meter_updated_;
    if (QCoreApplication::instance() == nullptr) {
        cb(per_track_rms);
        return;
    }
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [cb, per_track_rms]() { cb(per_track_rms); }, Qt::QueuedConnection);
}

std::filesystem::path RecordingCoordinator::GenerateOutputPath() const {
    FilenameTargetContext context = output_target_context_;
    context.video_codec = output_settings_.video_codec;
    context.audio_codec = output_settings_.audio_codec;
    return BuildOutputPath(output_settings_.output_folder, output_settings_.naming_pattern, output_settings_.container,
                           std::time(nullptr), context);
}

std::wstring RecordingCoordinator::FormatHResult(int32_t hr) {
    if (hr == 0)
        return {};
    wchar_t buf[32] = {};
    _snwprintf_s(buf, _TRUNCATE, L"0x%08X", static_cast<uint32_t>(hr));
    return buf;
}

std::wstring RecordingCoordinator::FormatErrorPhase(recorder_core::ErrorPhase phase) {
    switch (phase) {
    case recorder_core::ErrorPhase::None:
        return {};
    case recorder_core::ErrorPhase::Prepare:
        return L"Prepare";
    case recorder_core::ErrorPhase::VideoCapture:
        return L"Video Capture";
    case recorder_core::ErrorPhase::VideoEncode:
        return L"Video Encoder";
    case recorder_core::ErrorPhase::AudioCapture:
        return L"Audio Capture";
    case recorder_core::ErrorPhase::AudioEncode:
        return L"Audio Encoder";
    case recorder_core::ErrorPhase::Mux:
        return L"Mux";
    case recorder_core::ErrorPhase::Finalize:
        return L"Finalize";
    case recorder_core::ErrorPhase::Shutdown:
        return L"Shutdown";
    default:
        return L"Unknown";
    }
}

void RecordingCoordinator::AddMarker(RecordingMarkerType type) {
    const auto st = State();
    if (st != UiRecordingState::Recording && st != UiRecordingState::Paused) {
        diagnostics::AppLog::warning(QStringLiteral("marker"),
                                     QStringLiteral("rejected: unsupported state %1").arg(static_cast<int>(st)));
        return;
    }

    std::lock_guard<std::mutex> lock(markers_mutex_);

    if (markers_.size() >= kMaxRecordingMarkers) {
        if (!markers_limit_reported_) {
            diagnostics::AppLog::warning(QStringLiteral("marker"),
                                         QStringLiteral("rejected: marker limit %1 reached").arg(kMaxRecordingMarkers));
            markers_limit_reported_ = true;
        }
        return;
    }

    const uint64_t time_ms = static_cast<uint64_t>(last_elapsed_seconds_ * 1000.0);

    RecordingMarker marker;
    marker.time_ms = time_ms;
    marker.type = type;
    marker.label = RecordingMarkerTypeDefaultLabel(type);

    markers_.push_back(marker);

    diagnostics::AppLog::info(QStringLiteral("marker"), QStringLiteral("added id=%1 time_ms=%2 type=%3")
                                                            .arg(markers_.size())
                                                            .arg(time_ms)
                                                            .arg(QLatin1String(RecordingMarkerTypeToString(type))));

    WriteMarkerSidecar();
}

const std::vector<RecordingMarker>& RecordingCoordinator::Markers() const noexcept {
    std::lock_guard<std::mutex> lock(markers_mutex_);
    return markers_;
}

std::filesystem::path RecordingCoordinator::MarkerSidecarPath() const {
    if (current_output_path_.empty())
        return {};
    auto sidecar = current_output_path_;
    sidecar.replace_extension(L".markers.json");
    return sidecar;
}

void RecordingCoordinator::WriteMarkerSidecar() {
    const auto sidecar_path = MarkerSidecarPath();
    if (sidecar_path.empty())
        return;

    std::vector<RecordingMarker> snapshot;
    {
        std::lock_guard<std::mutex> lock(markers_mutex_);
        snapshot = markers_;
    }

    QJsonArray markers_array;
    for (const auto& m : snapshot) {
        QJsonObject obj;
        obj[QStringLiteral("timeMs")] = static_cast<qint64>(m.time_ms);
        obj[QStringLiteral("type")] = QString::fromLatin1(RecordingMarkerTypeToString(m.type));
        obj[QStringLiteral("label")] = QString::fromStdString(m.label);
        markers_array.append(obj);
    }

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("media")] = QString::fromStdWString(current_output_path_.filename().wstring());
    root[QStringLiteral("timebase")] = QStringLiteral("milliseconds");
    root[QStringLiteral("markers")] = markers_array;

    QJsonDocument doc(root);
    const QString sidecar_qstr = QString::fromStdWString(sidecar_path.wstring());

    QSaveFile file(sidecar_qstr);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        diagnostics::AppLog::warning(QStringLiteral("marker"),
                                     QStringLiteral("sidecar write open failed: %1").arg(sidecar_qstr));
        return;
    }

    file.write(doc.toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        diagnostics::AppLog::warning(QStringLiteral("marker"),
                                     QStringLiteral("sidecar write commit failed: %1").arg(sidecar_qstr));
    }
}

} // namespace exosnap
