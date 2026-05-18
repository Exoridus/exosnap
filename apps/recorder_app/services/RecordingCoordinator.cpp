#include "RecordingCoordinator.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace exosnap {

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

RecordingCoordinator::RecordingCoordinator(
    winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher)
    : dispatcher_(std::move(dispatcher))
{}

RecordingCoordinator::~RecordingCoordinator() {
    // Signal stop if still recording. jthread RAII joins the thread.
    if (is_recording_) {
        session_.Stop();
    }
    // recording_thread_ destructor calls request_stop() + join().
}

// ---------------------------------------------------------------------------
// Capability notification (called on UI thread)
// ---------------------------------------------------------------------------

void RecordingCoordinator::OnCapabilitiesReady(
    const exosnap::capability::CapabilitySet& caps,
    const exosnap::capability::ResolveResult& validation) {

    caps_             = &caps;
    validation_result_ = validation;

    if (validation.succeeded) {
        resolved_user_config_    = validation.resolved_config;
        state_                   = UiRecordingState::Ready;
        capability_status_text_  = L"Ready: MKV · AV1 NVENC · AAC · 60 fps";
    } else {
        state_ = UiRecordingState::Blocked;
        if (!validation.invalidity.empty()) {
            capability_status_text_ = ToWide(validation.invalidity.front().message);
        } else {
            capability_status_text_ = L"Recording unavailable";
        }
    }
}

void RecordingCoordinator::OnCapabilityFailure(std::wstring message) {
    state_                  = UiRecordingState::Blocked;
    capability_status_text_ = std::move(message);
}

// ---------------------------------------------------------------------------
// Target enumeration
// ---------------------------------------------------------------------------

std::vector<recorder_core::CaptureTarget> RecordingCoordinator::EnumerateTargets() {
    return recorder_core::RecorderSession::EnumerateTargets();
}

// ---------------------------------------------------------------------------
// Recording lifecycle
// ---------------------------------------------------------------------------

bool RecordingCoordinator::StartRecording(const recorder_core::CaptureTarget& target) {
    // Precondition checks
    if (is_recording_) return false;
    if (state_ != UiRecordingState::Ready &&
        state_ != UiRecordingState::Completed &&
        state_ != UiRecordingState::Failed) {
        return false;
    }
    if (!caps_) return false;

    // Generate output path
    auto output_path = GenerateOutputPath();
    current_output_path_ = output_path;

    // Create parent directory
    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);
    if (ec) {
        state_ = UiRecordingState::Failed;
        PostStateChange(UiRecordingState::Failed);
        UiRecordingResult result;
        result.succeeded    = false;
        result.error_detail = L"Failed to create output directory: " + ToWide(ec.message());
        PostResult(std::move(result));
        return false;
    }

    // Transition → Preparing
    PostStateChange(UiRecordingState::Preparing);

    // Build recorder config from resolved user config
    auto config = exosnap::capability::ToRecorderCoreConfig(resolved_user_config_, *caps_);
    config.target      = target;
    config.output_path = output_path;

    // Validate
    recorder_core::RecorderResult validate_result;
    if (!session_.Validate(config, &validate_result)) {
        state_ = UiRecordingState::Failed;
        PostStateChange(UiRecordingState::Failed);
        UiRecordingResult result;
        result.succeeded    = false;
        result.output_path  = output_path.wstring();
        result.error_phase  = FormatErrorPhase(validate_result.error_phase);
        result.hresult_text = FormatHResult(validate_result.error_code);
        result.error_detail = ToWide(validate_result.error_detail);
        PostResult(std::move(result));
        return false;
    }

    // Register stats callback — fires from RecorderSession worker thread.
    session_.SetStatsCallback([this](const recorder_core::SessionStats& stats) {
        PostStats(stats);
    });

    // Transition → Recording
    is_recording_ = true;
    PostStateChange(UiRecordingState::Recording);

    // Start background recording thread
    recording_thread_ = std::jthread(
        [this, cfg = std::move(config), op = std::move(output_path)](std::stop_token) {
            RecordingThreadProc(cfg, op);
        });

    return true;
}

void RecordingCoordinator::StopRecording() {
    if (!is_recording_) return;
    PostStateChange(UiRecordingState::Stopping);
    session_.Stop();
}

// ---------------------------------------------------------------------------
// Recording thread procedure (background thread)
// ---------------------------------------------------------------------------

void RecordingCoordinator::RecordingThreadProc(
    recorder_core::RecorderConfig config,
    std::filesystem::path output_path) {

    auto result = session_.Record(config);
    is_recording_ = false;

    UiRecordingResult ui_result;
    ui_result.succeeded    = result.succeeded;
    ui_result.output_path  = output_path.wstring();
    ui_result.error_phase  = FormatErrorPhase(result.error_phase);
    ui_result.hresult_text = FormatHResult(result.error_code);
    ui_result.error_detail = ToWide(result.error_detail);

    PostResult(std::move(ui_result));
    PostStateChange(result.succeeded ? UiRecordingState::Completed : UiRecordingState::Failed);
}

// ---------------------------------------------------------------------------
// State accessors
// ---------------------------------------------------------------------------

UiRecordingState RecordingCoordinator::State() const noexcept {
    return state_;
}

std::wstring RecordingCoordinator::CapabilityStatusText() const {
    return capability_status_text_;
}

std::filesystem::path RecordingCoordinator::CurrentOutputPath() const {
    return current_output_path_;
}

// ---------------------------------------------------------------------------
// Callback registration
// ---------------------------------------------------------------------------

void RecordingCoordinator::SetStateChangedCallback(StateChangedCallback cb) {
    on_state_changed_ = std::move(cb);
}

void RecordingCoordinator::SetStatsUpdatedCallback(StatsUpdatedCallback cb) {
    on_stats_updated_ = std::move(cb);
}

void RecordingCoordinator::SetResultReadyCallback(ResultReadyCallback cb) {
    on_result_ready_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Post helpers — marshal to UI thread via TryEnqueue
// ---------------------------------------------------------------------------

void RecordingCoordinator::PostStateChange(UiRecordingState new_state) {
    state_ = new_state;
    if (!on_state_changed_) return;
    // Capture by value; silently drop if queue is gone.
    auto cb = on_state_changed_;
    dispatcher_.TryEnqueue([cb, new_state]() {
        cb(new_state);
    });
}

void RecordingCoordinator::PostResult(UiRecordingResult result) {
    if (!on_result_ready_) return;
    auto cb = on_result_ready_;
    dispatcher_.TryEnqueue([cb, r = std::move(result)]() {
        cb(r);
    });
}

void RecordingCoordinator::PostStats(recorder_core::SessionStats stats) {
    if (!on_stats_updated_) return;
    auto cb = on_stats_updated_;
    dispatcher_.TryEnqueue([cb, s = std::move(stats)]() {
        cb(s);
    });
}

// ---------------------------------------------------------------------------
// Static utilities
// ---------------------------------------------------------------------------

std::filesystem::path RecordingCoordinator::GenerateOutputPath() {
    // %USERPROFILE%\Videos\Exosnap\exosnap_YYYYMMDD_HHMMSS.mkv
    wchar_t profile[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    std::filesystem::path base_dir;
    if (len > 0 && len < MAX_PATH) {
        base_dir = std::filesystem::path(profile) / L"Videos" / L"Exosnap";
    } else {
        base_dir = std::filesystem::path(L"C:\\Users\\Public\\Videos\\Exosnap");
    }

    // Format timestamp
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);

    wchar_t ts[32] = {};
    wcsftime(ts, _countof(ts), L"%Y%m%d_%H%M%S", &tm);

    std::wstring filename = std::wstring(L"exosnap_") + ts + L".mkv";
    return base_dir / filename;
}

std::wstring RecordingCoordinator::FormatHResult(HRESULT hr) {
    if (hr == S_OK) return {};
    wchar_t buf[32] = {};
    _snwprintf_s(buf, _TRUNCATE, L"0x%08X", static_cast<unsigned int>(hr));
    return buf;
}

std::wstring RecordingCoordinator::FormatErrorPhase(recorder_core::ErrorPhase phase) {
    switch (phase) {
    case recorder_core::ErrorPhase::None:         return {};
    case recorder_core::ErrorPhase::Prepare:      return L"Prepare";
    case recorder_core::ErrorPhase::VideoCapture: return L"VideoCapture";
    case recorder_core::ErrorPhase::VideoEncode:  return L"VideoEncode";
    case recorder_core::ErrorPhase::AudioCapture: return L"AudioCapture";
    case recorder_core::ErrorPhase::AudioEncode:  return L"AudioEncode";
    case recorder_core::ErrorPhase::Mux:          return L"Mux";
    case recorder_core::ErrorPhase::Finalize:     return L"Finalize";
    case recorder_core::ErrorPhase::Shutdown:     return L"Shutdown";
    default:                                      return L"Unknown";
    }
}

} // namespace exosnap
