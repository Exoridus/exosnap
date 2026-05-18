#include "RecordingCoordinator.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>

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

RecordingCoordinator::RecordingCoordinator() = default;

RecordingCoordinator::~RecordingCoordinator() {
    if (is_recording_) {
        session_.Stop();
    }
}

void RecordingCoordinator::OnCapabilitiesReady(const exosnap::capability::CapabilitySet& caps,
                                               const exosnap::capability::ResolveResult& validation) {
    caps_ = &caps;
    validation_result_ = validation;
    if (validation.succeeded) {
        resolved_user_config_ = validation.resolved_config;
        state_ = UiRecordingState::Ready;
        capability_status_text_ = L"Ready: MKV · AV1 NVENC · AAC · 60 fps";
    } else {
        state_ = UiRecordingState::Blocked;
        capability_status_text_ =
            validation.invalidity.empty() ? L"Recording unavailable" : ToWide(validation.invalidity.front().message);
    }
}

void RecordingCoordinator::OnCapabilityFailure(std::wstring message) {
    state_ = UiRecordingState::Blocked;
    capability_status_text_ = std::move(message);
}

std::vector<recorder_core::CaptureTarget> RecordingCoordinator::EnumerateTargets() {
    return recorder_core::RecorderSession::EnumerateTargets();
}

bool RecordingCoordinator::StartRecording(const recorder_core::CaptureTarget& target) {
    if (is_recording_)
        return false;
    if (state_ != UiRecordingState::Ready && state_ != UiRecordingState::Completed &&
        state_ != UiRecordingState::Failed) {
        return false;
    }
    if (!caps_)
        return false;

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

    auto config = exosnap::capability::ToRecorderCoreConfig(resolved_user_config_, *caps_);
    config.target = target;
    config.output_path = output_path;

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

void RecordingCoordinator::SetStateChangedCallback(StateChangedCallback cb) {
    on_state_changed_ = std::move(cb);
}
void RecordingCoordinator::SetStatsUpdatedCallback(StatsUpdatedCallback cb) {
    on_stats_updated_ = std::move(cb);
}
void RecordingCoordinator::SetResultReadyCallback(ResultReadyCallback cb) {
    on_result_ready_ = std::move(cb);
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

std::filesystem::path RecordingCoordinator::GenerateOutputPath() {
    wchar_t profile[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    std::filesystem::path base_dir = (len > 0 && len < MAX_PATH)
                                         ? std::filesystem::path(profile) / L"Videos" / L"Exosnap"
                                         : std::filesystem::path(L"C:\\Users\\Public\\Videos\\Exosnap");

    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t ts[32] = {};
    wcsftime(ts, _countof(ts), L"%Y%m%d_%H%M%S", &tm);
    return base_dir / (std::wstring(L"exosnap_") + ts + L".mkv");
}

std::wstring RecordingCoordinator::FormatHResult(HRESULT hr) {
    if (hr == S_OK)
        return {};
    wchar_t buf[32] = {};
    _snwprintf_s(buf, _TRUNCATE, L"0x%08X", static_cast<unsigned int>(hr));
    return buf;
}

std::wstring RecordingCoordinator::FormatErrorPhase(recorder_core::ErrorPhase phase) {
    switch (phase) {
    case recorder_core::ErrorPhase::None:
        return {};
    case recorder_core::ErrorPhase::Prepare:
        return L"Prepare";
    case recorder_core::ErrorPhase::VideoCapture:
        return L"VideoCapture";
    case recorder_core::ErrorPhase::VideoEncode:
        return L"VideoEncode";
    case recorder_core::ErrorPhase::AudioCapture:
        return L"AudioCapture";
    case recorder_core::ErrorPhase::AudioEncode:
        return L"AudioEncode";
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
