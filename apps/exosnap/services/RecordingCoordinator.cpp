#include "RecordingCoordinator.h"

#include "../../../libs/recorder_core/src/loopback_meter_service.h"
#include "../../../libs/recorder_core/src/mic_meter_service.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <string_view>

#include "../models/FilenameBuilder.h"
#include "../models/OutputPathValidator.h"

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
    if (validation.succeeded) {
        resolved_user_config_ = validation.resolved_config;
        state_ = UiRecordingState::Ready;
        capability_status_text_ = L"Ready: WebM · AV1 NVENC · OPUS · 60 fps";
    } else {
        state_ = UiRecordingState::Blocked;
        capability_status_text_ =
            validation.invalidity.empty() ? L"Recording unavailable" : ToWide(validation.invalidity.front().message);
    }
}

void RecordingCoordinator::OnCapabilityFailure(std::wstring message) {
    has_caps_ = false;
    state_ = UiRecordingState::Blocked;
    capability_status_text_ = std::move(message);
}

std::vector<recorder_core::CaptureTarget> RecordingCoordinator::EnumerateTargets() {
    return recorder_core::RecorderSession::EnumerateTargets();
}

bool RecordingCoordinator::StartRecording(const recorder_core::CaptureTarget& target,
                                          const capability::AudioUiState& audio_ui_state) {
    StopMicMeter();

    if (is_recording_)
        return false;
    const auto folder_check = ValidateOutputFolder(output_settings_.output_folder);
    if (folder_check != FolderValidationResult::Ok) {
        PostStateChange(UiRecordingState::Failed);

        UiRecordingResult result;
        result.succeeded = false;
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
    current_output_path_ = output_path;

    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);
    if (ec) {
        PostStateChange(UiRecordingState::Failed);
        UiRecordingResult result;
        result.succeeded = false;
        result.error_detail = L"Failed to create output directory: " + ToWide(ec.message());
        PostResult(std::move(result));
        return false;
    }

    PostStateChange(UiRecordingState::Preparing);

    auto config = exosnap::capability::ToRecorderCoreConfig(resolved_user_config_, caps_);
    ApplyOutputSettingsToRecorderConfig(config, output_settings_);
    config.target = target;
    config.output_path = output_path;

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

    is_recording_ = true;
    PostStateChange(UiRecordingState::Recording);

    recording_thread_ = std::jthread([this, cfg = std::move(config), op = std::move(output_path)](std::stop_token) {
        RecordingThreadProc(cfg, op);
    });

    return true;
}

void RecordingCoordinator::StopRecording() {
    if (!is_recording_)
        return;
    PostStateChange(UiRecordingState::Stopping);
    session_.Stop();
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

    UiRecordingResult ui_result;
    ui_result.succeeded = result.succeeded;
    ui_result.output_path = output_path.wstring();
    ui_result.error_phase = FormatErrorPhase(result.error_phase);
    ui_result.hresult_text = FormatHResult(result.error_code);
    ui_result.error_detail = ToWide(result.error_detail);

    PostResult(std::move(ui_result));
    PostStateChange(result.succeeded ? UiRecordingState::Completed : UiRecordingState::Failed);
}

UiRecordingState RecordingCoordinator::State() const noexcept {
    return state_;
}
const std::wstring& RecordingCoordinator::CapabilityStatusText() const {
    return capability_status_text_;
}
std::filesystem::path RecordingCoordinator::CurrentOutputPath() const {
    return current_output_path_;
}
void RecordingCoordinator::SetOutputSettings(const OutputSettingsModel& settings) {
    output_settings_ = settings;
    if (settings.container == capability::Container::Mp4) {
        resolved_user_config_.container = capability::Container::Mp4;
        resolved_user_config_.video_codec = capability::VideoCodec::H264Nvenc;
        resolved_user_config_.audio_codec = capability::AudioCodec::AacMf;
    } else {
        resolved_user_config_.container = capability::Container::WebM;
        resolved_user_config_.video_codec = capability::VideoCodec::Av1Nvenc;
        resolved_user_config_.audio_codec = capability::AudioCodec::Opus;
    }
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

std::filesystem::path RecordingCoordinator::GenerateOutputPath() const {
    return BuildOutputPath(output_settings_.output_folder, output_settings_.naming_pattern, output_settings_.container,
                           std::time(nullptr), output_target_context_);
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

} // namespace exosnap
