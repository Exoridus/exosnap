#include "RecordingCoordinator.h"

#include "../../../libs/recorder_core/src/loopback_meter_service.h"
#include "../../../libs/recorder_core/src/mic_meter_service.h"

#include <recorder_core/mp4_remuxer.h>

#include "../diagnostics/DiskSpaceThresholds.h"

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
#include <QUuid>

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
#include "../models/MarkerSidecar.h"
#include "../models/OutputPathValidator.h"
#include "../models/RecordingPreset.h"
#include "../settings/RecoveryManifestStore.h"

namespace exosnap {

// Disk-stop reason surfaced in UiRecordingResult::error_detail.
const wchar_t* RecordingCoordinator::kDiskSpaceStopReason =
    L"Recording stopped automatically: output drive is critically low on disk space.";

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
    case capability::AudioCodec::Pcm:
        config.audio_codec = recorder_core::AudioCodec::Pcm;
        return;
    case capability::AudioCodec::Flac:
        config.audio_codec = recorder_core::AudioCodec::Flac;
        return;
    case capability::AudioCodec::AacMf:
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
    case capability::AudioCodec::Flac:
        audio_name = L"FLAC";
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

void RecordingCoordinator::SetRecoveryManifestStore(RecoveryManifestStore* store) {
    recovery_manifest_store_ = store;
}

RecoveryManifestStore* RecordingCoordinator::GetRecoveryManifestStore() const noexcept {
    return recovery_manifest_store_;
}

void RecordingCoordinator::SetDiskSpaceProvider(diagnostics::IDiskSpaceProvider* provider) {
    disk_space_provider_ = provider;
}

// ---------------------------------------------------------------------------
// Low-disk guard (LOW-DISK-GUARD-R1)
// ---------------------------------------------------------------------------

void RecordingCoordinator::StartDiskMonitor(const std::filesystem::path& output_folder, bool is_mp4,
                                            const std::filesystem::path& transient_mkv) {
    session_is_mp4_ = is_mp4;
    session_transient_mkv_ = transient_mkv;
    disk_stop_triggered_.store(false);

    // Lazily construct the Win32 provider if no stub was injected.
    if (disk_space_provider_ == nullptr) {
        if (!default_disk_space_provider_) {
            default_disk_space_provider_ = std::make_unique<diagnostics::Win32DiskSpaceProvider>();
        }
        disk_space_provider_ = default_disk_space_provider_.get();
    }

    diagnostics::IDiskSpaceProvider* provider = disk_space_provider_;

    disk_monitor_thread_ =
        std::jthread([this, output_folder, is_mp4, transient_mkv, provider](std::stop_token stop_token) {
            // Poll every 5 seconds.
            constexpr auto kPollInterval = std::chrono::seconds(5);

            while (!stop_token.stop_requested()) {
                // Interruptible sleep.
                {
                    std::this_thread::sleep_for(kPollInterval);
                    if (stop_token.stop_requested())
                        break;
                }

                if (!is_recording_.load())
                    break;

                const uint64_t free_bytes = provider->FreeBytesForPath(output_folder);
                if (free_bytes == 0) {
                    // Query failed — skip this poll, do not trigger a false stop.
                    continue;
                }

                // For MP4 sessions compute the remux reserve as the sum of:
                //   (a) the current live transient MKV (segment being recorded now), and
                //   (b) all segment MKV files that have an outstanding background remux
                //       job (MP4-SPLIT-REMUX-R1: these coexist until each job finishes).
                // This is conservative but correct: at worst we require enough space for
                // all in-progress MKVs to have simultaneous output MP4 copies.
                uint64_t remux_reserve = 0;
                if (is_mp4) {
                    // (a) current live segment
                    if (!transient_mkv.empty()) {
                        std::error_code ec;
                        const auto mkv_size = std::filesystem::file_size(transient_mkv, ec);
                        if (!ec)
                            remux_reserve += static_cast<uint64_t>(mkv_size);
                    }
                    // (b) segments awaiting background remux
                    remux_reserve += this->PendingRemuxReserveBytes();
                }

                const uint64_t threshold = diagnostics::ComputeHardStopThreshold(remux_reserve);
                if (free_bytes <= threshold) {
                    OnDiskSpaceLow(free_bytes, threshold);
                    break; // stop monitoring — we already triggered the stop
                }
            }
        });
}

void RecordingCoordinator::StopDiskMonitor() {
    disk_monitor_thread_.request_stop();
    // Do NOT join here — the thread may be sleeping; we just request_stop and
    // let the jthread destructor join when it completes.  The jthread destructor
    // is called when disk_monitor_thread_ is replaced or the coordinator is
    // destroyed.
}

void RecordingCoordinator::OnDiskSpaceLow(uint64_t free_bytes, uint64_t threshold_bytes) {
    // Only fire once per session.
    bool expected = false;
    if (!disk_stop_triggered_.compare_exchange_strong(expected, true))
        return;

    {
        const double free_gb = static_cast<double>(free_bytes) / (1024.0 * 1024.0 * 1024.0);
        const double thresh_mb = static_cast<double>(threshold_bytes) / (1024.0 * 1024.0);
        diagnostics::AppLog::error(
            QStringLiteral("disk_guard"),
            QStringLiteral("auto-stop triggered free_bytes=%1 threshold_bytes=%2 free_gb=%3 threshold_mb=%4")
                .arg(static_cast<qint64>(free_bytes))
                .arg(static_cast<qint64>(threshold_bytes))
                .arg(free_gb, 0, 'f', 3)
                .arg(thresh_mb, 0, 'f', 0));
    }

    // Trigger a graceful stop on the Qt main thread (same path as user stop).
    if (QCoreApplication::instance() != nullptr) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [this, free_bytes, threshold_bytes]() {
                // Re-check is_recording_ on the main thread in case it already stopped.
                if (!is_recording_.load())
                    return;

                // Surface the reason via a synthetic result that will be delivered
                // after the engine stops.  We set the detail before stopping so the
                // result path can pick it up.
                disk_stop_reason_bytes_free_ = free_bytes;
                disk_stop_reason_threshold_ = threshold_bytes;

                StopRecording();
            },
            Qt::QueuedConnection);
    } else {
        // Test environment without Qt event loop — call directly.
        if (is_recording_.load()) {
            disk_stop_reason_bytes_free_ = free_bytes;
            disk_stop_reason_threshold_ = threshold_bytes;
            StopRecording();
        }
    }
}

void RecordingCoordinator::OnCapabilitiesReady(const exosnap::capability::CapabilitySet& caps,
                                               const exosnap::capability::ResolveResult& validation) {
    caps_ = caps;
    has_caps_ = true;
    validation_result_ = validation;
    resolved_user_config_ = validation.resolved_config;
    if (validation.succeeded) {
        capability_status_text_ = BuildCapabilityStatusText(validation.resolved_config);
        // Use PostStateChange (not a bare state_ assignment) so the UI's state-changed
        // callback fires. Without this the Record page stays stuck on "Checking…" forever
        // once the async HW probe lands — view_model_.state is only ever updated via this
        // callback (there is no pull). Regression introduced when the synchronous fallback
        // probe was removed in the fast-startup wave.
        PostStateChange(UiRecordingState::Ready);
    } else {
        capability_status_text_ =
            validation.invalidity.empty() ? L"Recording unavailable" : ToWide(validation.invalidity.front().message);
        PostStateChange(UiRecordingState::Blocked);
    }
}

void RecordingCoordinator::OnCapabilityFailure(std::wstring message) {
    diagnostics::AppLog::error(
        QStringLiteral("record.failure"),
        QStringLiteral("phase=Init category=CapabilityCheck detail=\"%1\"").arg(QString::fromStdWString(message)));
    has_caps_ = false;
    capability_status_text_ = std::move(message);
    PostStateChange(UiRecordingState::Blocked); // notify the UI (see OnCapabilitiesReady)
}

void RecordingCoordinator::RevalidateCapabilities() {
    if (!has_caps_)
        return;
    const bool busy = state_ == UiRecordingState::Preparing || state_ == UiRecordingState::Recording ||
                      state_ == UiRecordingState::Paused || state_ == UiRecordingState::Stopping ||
                      state_ == UiRecordingState::ArmedFromRecovery;
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
    const std::filesystem::path effective_folder = EffectiveOutputFolder();

    // Pre-start disk-space guard (LOW-DISK-GUARD-R1).
    // Block recording when free space is at or below the hard-stop threshold.
    // Uses the injected provider if available, otherwise the Win32 implementation.
    {
        if (disk_space_provider_ == nullptr) {
            if (!default_disk_space_provider_) {
                default_disk_space_provider_ = std::make_unique<diagnostics::Win32DiskSpaceProvider>();
            }
            disk_space_provider_ = default_disk_space_provider_.get();
        }
        const uint64_t free_bytes = disk_space_provider_->FreeBytesForPath(effective_folder);
        // Use the base threshold (no remux reserve) for the pre-start check;
        // at this point we do not yet know the MKV size.
        const uint64_t threshold = diagnostics::kHardStopFreeBytes;
        if (free_bytes > 0 && free_bytes <= threshold) {
            const double free_gb = static_cast<double>(free_bytes) / (1024.0 * 1024.0 * 1024.0);
            diagnostics::AppLog::error(QStringLiteral("disk_guard"),
                                       QStringLiteral("pre-start blocked: free_bytes=%1 threshold_bytes=%2 free_gb=%3")
                                           .arg(static_cast<qint64>(free_bytes))
                                           .arg(static_cast<qint64>(threshold))
                                           .arg(free_gb, 0, 'f', 3));

            PostStateChange(UiRecordingState::Failed);
            UiRecordingResult result;
            result.succeeded = false;
            result.error_phase = L"DiskSpace";
            result.error_detail = kDiskSpaceStopReason;
            PostResult(std::move(result));
            return false;
        }
    }

    const auto folder_check = ValidateOutputFolder(effective_folder);
    if (folder_check != FolderValidationResult::Ok) {
        diagnostics::AppLog::error(QStringLiteral("record.failure"),
                                   QStringLiteral("phase=Prepare category=OutputFolder output_folder=\"%1\" detail=%2")
                                       .arg(QString::fromStdWString(effective_folder.wstring()),
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
        state_ != UiRecordingState::Failed && state_ != UiRecordingState::ArmedFromRecovery) {
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
                                       .arg(QString::fromStdWString(effective_folder.wstring())));

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
    config.nvenc_rate_control = video_settings_.rate_control;
    config.nvenc_bitrate_kbps = video_settings_.bitrate_kbps;
    config.frame_rate_num = video_settings_.frame_rate_num;
    config.frame_rate_den = video_settings_.frame_rate_den;
    config.cfr = video_settings_.cfr;
    config.cfr_pacing_mode = video_settings_.frame_pacing;
    // Map keyframe interval mode to seconds for NVENC GOP configuration.
    switch (video_settings_.keyframe_interval) {
    case KeyframeIntervalMode::Seconds2:
        config.keyframe_interval_secs = 2.0f;
        break;
    case KeyframeIntervalMode::Seconds1:
        config.keyframe_interval_secs = 1.0f;
        break;
    case KeyframeIntervalMode::Seconds0_5:
        config.keyframe_interval_secs = 0.5f;
        break;
    }
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
    // Audio encoding parameters (ADR 0019).
    config.audio_bitrate_kbps = plan.audio_bitrate_kbps;
    config.opus_frame_duration = plan.opus_frame_duration;
    config.opus_complexity = plan.opus_complexity;
    // Brickwall limiter (Audio v2).
    config.audio_limiter_enabled = plan.limiter_enabled;
    config.audio_limiter_ceiling_db = plan.limiter_ceiling_db;
    // Microphone high-pass filter (Audio v2).
    config.mic_hpf_enabled = plan.mic_hpf_enabled;
    config.mic_hpf_cutoff_hz = plan.mic_hpf_cutoff_hz;
    // Microphone noise gate (Audio v2).
    config.mic_gate_enabled = plan.mic_gate_enabled;
    config.mic_gate_threshold_db = plan.mic_gate_threshold_db;
    // Microphone automatic gain control (Audio v2).
    config.mic_agc_enabled = plan.mic_agc_enabled;
    config.mic_agc_target_db = plan.mic_agc_target_db;
    // Microphone RNNoise neural noise suppression (Audio v2).
    config.mic_rnnoise_enabled = plan.mic_rnnoise_enabled;
    // Channel / sample-format model (ADR 0030 — 0.6.0).
    config.audio_sample_rate = plan.audio_sample_rate;
    config.audio_channels = plan.audio_channels;
    config.audio_bit_depth = plan.audio_bit_depth;
    config.flac_compression_level = plan.flac_compression_level;

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
    session_.SetDiagnosticsCallback(
        [this](const recorder_core::RecordingDiagnosticsSnapshot& snapshot) { PostDiagnostics(snapshot); });
    // Show an "initializing" diagnostics state until the engine emits live snapshots.
    EmitInitializingDiagnostics();
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
    disk_stop_reason_bytes_free_ = 0;
    disk_stop_reason_threshold_ = 0;

    {
        std::lock_guard<std::mutex> lock(markers_mutex_);
        markers_.clear();
        last_elapsed_seconds_ = 0.0;
        last_media_time_ns_ = 0;
        markers_limit_reported_ = false;
    }

    // MP4-SPLIT-REMUX-R1: clear per-segment remux state from any prior session.
    {
        std::lock_guard<std::mutex> lock(segment_remux_mutex_);
        segment_remux_jobs_.clear();
        pending_segment_manifest_id_.clear();
    }

    // Recovery manifest entry — written before the engine starts so a hard crash
    // leaves a traceable artefact. The artefact_path is the file the engine will
    // actually write (transient .mkv.tmp for MP4 target, final .mkv for MKV target).
    current_manifest_id_.clear();
    if (recovery_manifest_store_ != nullptr) {
        const bool is_mp4 = (config.container == recorder_core::Container::Mp4);
        RecoveryManifestEntry entry;
        entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        entry.artefact_path =
            is_mp4 ? QString::fromStdWString(recorder_core::DeriveTransientMkvPath(output_path).wstring())
                   : QString::fromStdWString(output_path.wstring());
        entry.intended_container = is_mp4 ? QStringLiteral("mp4") : QStringLiteral("mkv");
        entry.final_output_path = QString::fromStdWString(output_path.wstring());
        entry.started_at = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        entry.finalized = false;
        recovery_manifest_store_->Add(entry);
        current_manifest_id_ = entry.id;
    }

    PostStateChange(UiRecordingState::Recording);

    {
        const bool is_monitor = (target.kind == recorder_core::CaptureTarget::Kind::Monitor);
        const QString backend = is_monitor ? QStringLiteral("dxgi_od") : QStringLiteral("wgc");
        const QString target_desc = QString::fromStdString(target.description);
        diagnostics::AppLog::info(QStringLiteral("record"),
                                  QStringLiteral("start backend=%1 target=\"%2\"").arg(backend, target_desc));
    }

    // Start the low-disk guard monitor thread (LOW-DISK-GUARD-R1).
    {
        const bool is_mp4 = (config.container == recorder_core::Container::Mp4);
        const std::filesystem::path transient_mkv =
            is_mp4 ? recorder_core::DeriveTransientMkvPath(output_path) : std::filesystem::path{};
        StartDiskMonitor(EffectiveOutputFolder(), is_mp4, transient_mkv);
    }

    recording_thread_ = std::jthread([this, cfg = std::move(config), op = std::move(output_path)](std::stop_token) {
        RecordingThreadProc(cfg, op);
    });

    return true;
}

// ---------------------------------------------------------------------------
// ADR-0015: armed-from-recovery state
// ---------------------------------------------------------------------------

bool RecordingCoordinator::ArmFromRecovery(const RecoverySessionInfo& info) {
    // Only valid when not actively recording and in a stable UI state.
    const auto st = State();
    if (st == UiRecordingState::Preparing || st == UiRecordingState::Recording || st == UiRecordingState::Stopping ||
        st == UiRecordingState::Saving) {
        diagnostics::AppLog::warning(QStringLiteral("recovery"),
                                     QStringLiteral("ArmFromRecovery rejected: state=%1").arg(static_cast<int>(st)));
        return false;
    }

    // Multi-recovery replacement rule: if another candidate is already armed,
    // finalize it first (its slices become a finished recording; the background
    // remux is already in flight or completed by the overlay's Finish action).
    if (is_armed_from_recovery_) {
        diagnostics::AppLog::info(QStringLiteral("recovery"),
                                  QStringLiteral("ArmFromRecovery: finalizing previous armed session id=%1")
                                      .arg(armed_recovery_session_.manifest_entry.id));
        FinalizeArmedRecovery();
    }

    is_armed_from_recovery_ = true;
    armed_recovery_session_ = info;
    armed_recovery_slice_count_ = 0;

    diagnostics::AppLog::info(QStringLiteral("recovery"), QStringLiteral("ArmFromRecovery: armed id=%1 target_valid=%2")
                                                              .arg(info.manifest_entry.id)
                                                              .arg(info.target_valid));

    PostStateChange(UiRecordingState::ArmedFromRecovery);
    return true;
}

void RecordingCoordinator::FinalizeArmedRecovery() {
    if (!is_armed_from_recovery_)
        return;

    diagnostics::AppLog::info(QStringLiteral("recovery"), QStringLiteral("FinalizeArmedRecovery: id=%1 slices=%2")
                                                              .arg(armed_recovery_session_.manifest_entry.id)
                                                              .arg(armed_recovery_slice_count_));

    is_armed_from_recovery_ = false;
    armed_recovery_session_ = {};
    armed_recovery_slice_count_ = 0;

    // Transition back to Ready (if capabilities loaded) or the pre-existing state.
    const auto st = State();
    if (st == UiRecordingState::ArmedFromRecovery) {
        if (has_caps_ && validation_result_.succeeded) {
            PostStateChange(UiRecordingState::Ready);
        } else if (has_caps_) {
            PostStateChange(UiRecordingState::Blocked);
        }
        // If no caps yet, stay in LoadingCapabilities (OnCapabilitiesReady will update).
    }
}

bool RecordingCoordinator::IsArmedFromRecovery() const noexcept {
    return is_armed_from_recovery_;
}

const RecordingCoordinator::RecoverySessionInfo& RecordingCoordinator::ArmedRecoverySession() const noexcept {
    return armed_recovery_session_;
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
    case recorder_core::SplitTriggerSource::AutomaticSize:
        return "size";
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
    // True only when this finalize was triggered by a split request (a new segment
    // follows). The final session-end finalize has no pending request, so it must
    // not produce a spurious "Started segment N" toast.
    const bool was_split_boundary = split_pending_.exchange(false);

    diagnostics::AppLog::info(QStringLiteral("split"),
                              QStringLiteral("segment finalized index=%1 duration_ms=%2 bytes=%3 ok=%4 path=%5")
                                  .arg(segment.index)
                                  .arg(segment.duration_ms)
                                  .arg(segment.file_size_bytes)
                                  .arg(segment.succeeded)
                                  .arg(QString::fromStdWString(segment.path.filename().wstring())));

    // Per-segment marker sidecar (SPLIT-RECORDING-R1): partition session markers
    // into this segment, rebased to segment-local time. Only for successfully
    // finalized segments (a quarantined file gets no orphan sidecar).
    if (segment.succeeded)
        WriteSegmentMarkerSidecar(segment);

    // MP4-SPLIT-REMUX-R1: for intermediate segments in an MP4 session, kick off
    // a background remux while recording continues into the next segment.
    //
    // This is a split boundary (not the final segment), the engine succeeded for
    // this segment, and the session container is MP4. The coordinator already
    // knows this via session_is_mp4_ set at StartDiskMonitor / StartRecording time.
    //
    // Manifest lifecycle:
    //   - current_manifest_id_ was created (at StartRecording or the previous
    //     OnSegmentCompleted) and represents THIS segment's recovery entry.
    //   - We finalize it (UpdateFinalized=true) synchronously here since the MKV
    //     is cleanly closed by the engine before this callback fires.
    //   - We create a NEW manifest entry for the NEXT segment (segment N+1 just
    //     started writing) and store it in pending_segment_manifest_id_.
    //   - The recording thread will read pending_segment_manifest_id_ and use it
    //     as current_manifest_id_ when it processes the final segment.
    if (was_split_boundary && segment.succeeded && session_is_mp4_) {
        // Derive the expected MP4 output path for this segment from the transient MKV path.
        // segment.path is a .mkv.tmp (or _part-NNN.mkv.tmp) file; derive the .mp4 side.
        // The base output path is current_output_path_ (the .mp4 path requested by the user).
        // Use DeriveSegmentPath on the MP4 base to get the corresponding MP4 segment path.
        const std::filesystem::path mp4_segment = recorder_core::DeriveSegmentPath(current_output_path_, segment.index);

        // Capture the manifest ID for this segment before mutating current_manifest_id_.
        QString this_segment_manifest_id;
        QString next_segment_manifest_id;

        {
            // We use segment_remux_mutex_ to protect segment_remux_jobs_ and
            // pending_segment_manifest_id_ which are shared with the recording thread.
            std::lock_guard<std::mutex> lock(segment_remux_mutex_);

            // The manifest ID for THIS segment is the one current at this point.
            // For segment 0 this is current_manifest_id_ set at StartRecording.
            // For segment N (N>0) this was stored in pending_segment_manifest_id_
            // when segment N-1 completed, and adopted below.
            this_segment_manifest_id = current_manifest_id_;

            // Finalize the manifest entry: the MKV is clean (engine closed it).
            if (recovery_manifest_store_ != nullptr && !this_segment_manifest_id.isEmpty()) {
                recovery_manifest_store_->UpdateFinalized(this_segment_manifest_id, true);
            }

            // Create a recovery manifest entry for the NEXT segment (now recording).
            if (recovery_manifest_store_ != nullptr) {
                const std::filesystem::path next_mkv =
                    recorder_core::DeriveSegmentPath(session_transient_mkv_, segment.index + 1);
                RecoveryManifestEntry next_entry;
                next_entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                next_entry.artefact_path = QString::fromStdWString(next_mkv.wstring());
                next_entry.intended_container = QStringLiteral("mp4");
                next_entry.final_output_path = QString::fromStdWString(
                    recorder_core::DeriveSegmentPath(current_output_path_, segment.index + 1).wstring());
                next_entry.started_at = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
                next_entry.finalized = false;
                recovery_manifest_store_->Add(next_entry);
                next_segment_manifest_id = next_entry.id;
            }

            // Update current_manifest_id_ so the recording thread sees the next segment's ID.
            current_manifest_id_ = next_segment_manifest_id;
            pending_segment_manifest_id_ = next_segment_manifest_id;

            // Schedule the background remux for THIS segment.
            auto job = std::make_unique<SegmentRemuxJob>();
            job->transient_mkv = segment.path;
            job->output_mp4 = mp4_segment;
            job->manifest_id = this_segment_manifest_id;
            StartSegmentRemuxThread(*job);
            segment_remux_jobs_.push_back(std::move(job));
        }

        diagnostics::AppLog::info(
            QStringLiteral("remux"),
            QStringLiteral("split segment remux scheduled index=%1 transient=\"%2\" output=\"%3\"")
                .arg(segment.index)
                .arg(QString::fromStdWString(segment.path.filename().wstring()),
                     QString::fromStdWString(mp4_segment.filename().wstring())));
    }

    // Feedback names the segment that just *started* (the one after the boundary).
    // Only on an actual split boundary (not the final session-end finalize) and
    // only for a successfully finalized prior segment. total counts finalized
    // segments; the new live segment's human-friendly part number is total+1.
    if (was_split_boundary && is_recording_.load() && segment.succeeded) {
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

    // Stop the disk monitor now that recording has ended.  The monitor thread
    // may already have stopped itself (if it triggered the auto-stop), or it may
    // be in a sleep.  request_stop() is idempotent.
    StopDiskMonitor();

    UiRecordingResult ui_result;
    // For MP4 sessions, output_path is the final .mp4; the engine actually wrote
    // to the transient .mkv.tmp path (ADR-0014). We expose the final path to the UI.
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
    // Report the user-requested container, not the internal Matroska container.
    ui_result.container = config.container;
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

    // Single-file recordings keep the legacy base sidecar. For multi-segment
    // recordings each segment's sidecar was already written (partitioned, segment-
    // local) by OnSegmentCompleted, so do not clobber segment 0 with all markers.
    const bool multi_segment = ui_result.segments.size() > 1;
    if (result.succeeded && !ui_result.markers.empty() && !multi_segment) {
        WriteMarkerSidecar();
        diagnostics::AppLog::info(QStringLiteral("marker"),
                                  QStringLiteral("sidecar finalized markers=%1 path=%2")
                                      .arg(ui_result.markers.size())
                                      .arg(QString::fromStdWString(MarkerSidecarPath().wstring())));
    }

    // LOW-DISK-GUARD-R1: if the auto-stop fired, override the error_detail and
    // mark the result as a disk-space stop (not a hard engine failure).  The
    // recording may still have succeeded up to the stop point — we leave
    // result.succeeded as-is and only enrich the detail string.
    if (disk_stop_triggered_.load()) {
        const double free_gb = static_cast<double>(disk_stop_reason_bytes_free_) / (1024.0 * 1024.0 * 1024.0);
        const double thresh_mb = static_cast<double>(disk_stop_reason_threshold_) / (1024.0 * 1024.0);
        wchar_t buf[256] = {};
        _snwprintf_s(buf, _TRUNCATE, L"%ls (%.3f GB free, %.0f MB threshold)", kDiskSpaceStopReason, free_gb,
                     thresh_mb);
        ui_result.error_detail = buf;
        // Surface the disk-stop reason even when the engine itself "succeeded"
        // (graceful stop produces succeeded=true for the engine's perspective).
        ui_result.error_phase = L"DiskSpace";
        ui_result.succeeded = false;
    }

    if (!result.succeeded && !disk_stop_triggered_.load()) {
        diagnostics::AppLog::error(QStringLiteral("record.failure"),
                                   QStringLiteral("phase=%1 output_path=\"%2\" detail=\"%3\"")
                                       .arg(QString::fromStdWString(ui_result.error_phase),
                                            QString::fromStdWString(ui_result.output_path),
                                            QString::fromStdWString(ui_result.error_detail)));
    }

    // ADR-0014 + MP4-SPLIT-REMUX-R1: remux-on-stop for MP4 sessions.
    //
    // For single-file recordings (no splits or MKV/WebM): use RunRemuxJob as before.
    // For split recordings: one or more intermediate segment remux jobs may already
    // be running in the background (kicked off by OnSegmentCompleted).  We need to
    // also remux the FINAL segment and then wait for ALL jobs to complete.
    const bool needs_remux = result.succeeded && (config.container == recorder_core::Container::Mp4);
    if (needs_remux) {
        // Determine whether any intermediate segments were already scheduled.
        bool has_split_segments = false;
        {
            std::lock_guard<std::mutex> lock(segment_remux_mutex_);
            has_split_segments = !segment_remux_jobs_.empty();
        }

        if (!has_split_segments) {
            // ── Single-file MP4 path (no splits) — unchanged from the original flow ──
            const std::filesystem::path transient_mkv = recorder_core::DeriveTransientMkvPath(output_path);
            // Mark the manifest entry as finalized before remux.
            if (recovery_manifest_store_ != nullptr && !current_manifest_id_.isEmpty())
                recovery_manifest_store_->UpdateFinalized(current_manifest_id_, true);
            PostStateChange(UiRecordingState::Saving);
            SyncWebcamService(false);
            RunRemuxJob(transient_mkv, output_path, std::move(ui_result));
            return;
        }

        // ── Split MP4 path: final segment + drain all background jobs ──
        //
        // The final segment's manifest ID is current_manifest_id_ (set either at
        // StartRecording for a single-segment session, or updated by the last
        // OnSegmentCompleted call for the preceding segment).
        //
        // The segments_ list accumulated from the engine includes ALL segments,
        // including the final one (the engine fires SegmentCallback for the last
        // segment when Record() stops).  Identify the final segment.
        recorder_core::CompletedSegment final_seg;
        {
            std::lock_guard<std::mutex> lock(segments_mutex_);
            if (!segments_.empty())
                final_seg = segments_.back();
        }

        // Finalize the manifest entry for the final segment.
        if (recovery_manifest_store_ != nullptr && !current_manifest_id_.isEmpty())
            recovery_manifest_store_->UpdateFinalized(current_manifest_id_, true);

        // Schedule the final segment remux (same as intermediate segments).
        if (final_seg.succeeded && !final_seg.path.empty()) {
            const std::filesystem::path mp4_final = recorder_core::DeriveSegmentPath(output_path, final_seg.index);
            {
                std::lock_guard<std::mutex> lock(segment_remux_mutex_);
                auto job = std::make_unique<SegmentRemuxJob>();
                job->transient_mkv = final_seg.path;
                job->output_mp4 = mp4_final;
                job->manifest_id = current_manifest_id_;
                StartSegmentRemuxThread(*job);
                segment_remux_jobs_.push_back(std::move(job));
            }
            current_manifest_id_.clear(); // now owned by the job
        } else {
            // Final segment failed — keep the manifest entry for recovery.
            current_manifest_id_.clear();
        }

        PostStateChange(UiRecordingState::Saving);
        SyncWebcamService(false);

        // Drain: join all segment remux jobs (intermediate + final).
        const bool all_ok = DrainSegmentRemuxJobs(/*cancel=*/false);

        // Build the final UI result.
        if (all_ok) {
            // Rewrite each segment's file_path from the transient MKV to the output MP4,
            // and accumulate the total output bytes across all segments.
            uint64_t total_bytes = 0;
            for (auto& seg : ui_result.segments) {
                const std::filesystem::path seg_mp4 =
                    recorder_core::DeriveSegmentPath(output_path, static_cast<uint32_t>(seg.index));
                seg.file_path = QString::fromStdWString(seg_mp4.wstring());
                std::error_code sz_ec;
                const auto sz = std::filesystem::file_size(seg_mp4, sz_ec);
                if (!sz_ec)
                    total_bytes += static_cast<uint64_t>(sz);
            }
            ui_result.output_file_bytes = total_bytes;
            // Point the top-level output_path at the first segment for UI consistency
            // (same convention as single-file: the path the user asked for).
            ui_result.output_path = output_path.wstring();
            PostResult(std::move(ui_result));
            PostStateChange(UiRecordingState::Completed);
        } else {
            // At least one segment remux failed or was cancelled.
            const bool cancelled = remux_cancel_requested_.load();
            ui_result.succeeded = false;
            ui_result.error_phase = L"Remux";
            ui_result.error_detail = cancelled
                                         ? L"Remux cancelled — recording segments saved as MKV"
                                         : L"One or more segment remuxes failed — recording segments saved as MKV";
            PostResult(std::move(ui_result));
            PostStateChange(UiRecordingState::Failed);
        }
        SyncWebcamService(false);
        return;
    }

    // MKV target: engine succeeded → artefact is the final file; remove entry.
    // Engine failed → leave the entry so recovery UI can offer repair.
    if (recovery_manifest_store_ != nullptr && !current_manifest_id_.isEmpty()) {
        if (result.succeeded)
            recovery_manifest_store_->Remove(current_manifest_id_);
        // On failure the entry stays — recovery UI will surface it.
        current_manifest_id_.clear();
    }

    // 0.9.0 S1: for MKV recordings the output file IS the edit master.
    if (result.succeeded)
        ui_result.mkv_master_path = output_path.wstring();

    PostResult(std::move(ui_result));
    PostStateChange(result.succeeded ? UiRecordingState::Completed : UiRecordingState::Failed);
    // is_recording_ is already false here; restore the idle preview capture if the
    // Record page still wants a live PiP, otherwise release the device.
    SyncWebcamService(false);
}

void RecordingCoordinator::RunRemuxJob(const std::filesystem::path& transient_mkv,
                                       const std::filesystem::path& final_mp4, UiRecordingResult base_result) {
    is_remuxing_.store(true);
    remux_cancel_requested_.store(false);
    transient_mkv_path_ = transient_mkv;
    final_mp4_path_ = final_mp4;

    // Emit indeterminate start.
    PostRemuxProgress(-1.0f);

    diagnostics::AppLog::info(QStringLiteral("remux"), QStringLiteral("start transient=\"%1\" output=\"%2\"")
                                                           .arg(QString::fromStdWString(transient_mkv.wstring()),
                                                                QString::fromStdWString(final_mp4.wstring())));

    remux_thread_ = std::jthread([this, transient_mkv, final_mp4, base = std::move(base_result)](std::stop_token) {
        // Build the progress callback. Returns false to cancel when requested.
        auto progress_cb = [this](float fraction) -> bool {
            if (remux_cancel_requested_.load())
                return false;
            PostRemuxProgress(fraction);
            return true;
        };

        const auto remux_result = recorder_core::RemuxToProgressiveMp4(transient_mkv, final_mp4, progress_cb);

        // Back on the recording thread; marshal everything to the Qt main thread.
        UiRecordingResult final_result = base;
        is_remuxing_.store(false);

        if (remux_result.success) {
            // Retain the transient MKV as the edit master by renaming it to .edit.mkv.
            // The companion file lives alongside the final MP4 for the edit surface.
            std::filesystem::path edit_master = transient_mkv;
            edit_master.replace_extension(L".edit.mkv");
            std::error_code ec;
            std::filesystem::rename(transient_mkv, edit_master, ec);
            if (ec) {
                // Rename failed (e.g. cross-device); fall back to copy+delete.
                std::error_code copy_ec;
                std::filesystem::copy_file(transient_mkv, edit_master,
                                           std::filesystem::copy_options::overwrite_existing, copy_ec);
                if (!copy_ec) {
                    std::error_code del_ec;
                    std::filesystem::remove(transient_mkv, del_ec);
                    ec = del_ec; // report deletion error if any, clear on success
                }
            }
            if (!ec) {
                mkv_master_path_ = edit_master;
                final_result.mkv_master_path = edit_master.wstring();
                diagnostics::AppLog::info(
                    QStringLiteral("remux"),
                    QStringLiteral("edit master retained: \"%1\"").arg(QString::fromStdWString(edit_master.wstring())));
            } else {
                // Retention failed — edit surface degrades gracefully (no master path).
                mkv_master_path_.clear();
                diagnostics::AppLog::warning(QStringLiteral("remux"),
                                             QStringLiteral("edit master retention failed: %1")
                                                 .arg(QString::fromStdWString(ToWide(ec.message()))));
                // Best-effort cleanup of the original transient path.
                std::error_code del_ec;
                std::filesystem::remove(transient_mkv, del_ec);
            }

            // Update file size to reflect the final MP4.
            std::error_code size_ec;
            const auto mp4_size = std::filesystem::file_size(final_mp4, size_ec);
            if (!size_ec)
                final_result.output_file_bytes = mp4_size;

            diagnostics::AppLog::info(QStringLiteral("remux"),
                                      QStringLiteral("complete bytes=%1").arg(static_cast<qint64>(mp4_size)));

            // Remux complete — remove the manifest entry.
            if (recovery_manifest_store_ != nullptr && !current_manifest_id_.isEmpty()) {
                recovery_manifest_store_->Remove(current_manifest_id_);
                current_manifest_id_.clear();
            }

            PostResult(std::move(final_result));
            PostStateChange(UiRecordingState::Completed);
        } else {
            // Remux failed or was cancelled. The transient MKV is a valid, playable
            // file — keep it and report the error pointing to it.
            // The cancel flag is set before the progress callback returns false,
            // so checking it is sufficient.  AVERROR(ECANCELED) detection is not
            // needed because the flag is the canonical cancel indicator.
            const bool cancelled = remux_cancel_requested_.load();
            if (cancelled) {
                diagnostics::AppLog::info(QStringLiteral("remux"),
                                          QStringLiteral("cancelled — transient MKV retained: \"%1\"")
                                              .arg(QString::fromStdWString(transient_mkv.wstring())));
            } else {
                diagnostics::AppLog::error(
                    QStringLiteral("remux"),
                    QStringLiteral("failed av_err=%1 detail=\"%2\" — transient MKV retained: \"%3\"")
                        .arg(remux_result.av_error_code)
                        .arg(QString::fromStdString(remux_result.message),
                             QString::fromStdWString(transient_mkv.wstring())));
            }

            // Manifest entry stays (finalized=true was set before the remux started).
            // The recovery UI will offer re-export or keep-as-MKV at next startup.
            current_manifest_id_.clear();

            final_result.succeeded = false;
            final_result.output_path = transient_mkv.wstring();
            final_result.error_phase = L"Remux";
            final_result.error_detail =
                cancelled ? L"Remux cancelled — recording saved as MKV: " + transient_mkv.wstring()
                          : ToWide(remux_result.message) + L" — recording saved as MKV: " + transient_mkv.wstring();

            PostResult(std::move(final_result));
            PostStateChange(UiRecordingState::Failed);
        }

        SyncWebcamService(false);
    });
}

// ---------------------------------------------------------------------------
// MP4-SPLIT-REMUX-R1: per-segment background remux helpers
// ---------------------------------------------------------------------------

void RecordingCoordinator::StartSegmentRemuxThread(SegmentRemuxJob& job) {
    // job must be fully initialised before this call.
    // The thread writes job.succeeded / job.av_error_code / job.error_message
    // and then exits; DrainSegmentRemuxJobs joins it.
    job.thread = std::thread([this, &job]() {
        const std::filesystem::path transient_mkv = job.transient_mkv;
        const std::filesystem::path output_mp4 = job.output_mp4;
        const QString manifest_id = job.manifest_id;

        diagnostics::AppLog::info(QStringLiteral("remux"),
                                  QStringLiteral("segment start transient=\"%1\" output=\"%2\"")
                                      .arg(QString::fromStdWString(transient_mkv.filename().wstring()),
                                           QString::fromStdWString(output_mp4.filename().wstring())));

        auto progress_cb = [this](float /*fraction*/) -> bool { return !remux_cancel_requested_.load(); };
        const auto result = recorder_core::RemuxToProgressiveMp4(transient_mkv, output_mp4, progress_cb);

        job.succeeded = result.success;
        job.av_error_code = result.av_error_code;
        job.error_message = result.message;

        if (result.success) {
            // Delete the transient MKV for this segment.
            std::error_code ec;
            std::filesystem::remove(transient_mkv, ec);
            if (ec) {
                diagnostics::AppLog::warning(QStringLiteral("remux"),
                                             QStringLiteral("segment transient MKV removal failed: %1 path=\"%2\"")
                                                 .arg(QString::fromStdWString(ToWide(ec.message())),
                                                      QString::fromStdWString(transient_mkv.filename().wstring())));
            }

            std::error_code size_ec;
            const auto mp4_size = std::filesystem::file_size(output_mp4, size_ec);
            diagnostics::AppLog::info(QStringLiteral("remux"),
                                      QStringLiteral("segment complete bytes=%1 output=\"%2\"")
                                          .arg(static_cast<qint64>(size_ec ? 0 : mp4_size))
                                          .arg(QString::fromStdWString(output_mp4.filename().wstring())));

            // Remove the manifest entry on success.
            if (recovery_manifest_store_ != nullptr && !manifest_id.isEmpty()) {
                recovery_manifest_store_->Remove(manifest_id);
            }
        } else {
            const bool cancelled = remux_cancel_requested_.load();
            if (cancelled) {
                diagnostics::AppLog::info(QStringLiteral("remux"),
                                          QStringLiteral("segment cancelled — transient MKV retained: \"%1\"")
                                              .arg(QString::fromStdWString(transient_mkv.filename().wstring())));
            } else {
                diagnostics::AppLog::error(
                    QStringLiteral("remux"),
                    QStringLiteral("segment failed av_err=%1 detail=\"%2\" — transient MKV retained: \"%3\"")
                        .arg(result.av_error_code)
                        .arg(QString::fromStdString(result.message),
                             QString::fromStdWString(transient_mkv.filename().wstring())));
            }
            // Manifest entry stays; recovery UI will offer re-export.
        }
    });
}

bool RecordingCoordinator::DrainSegmentRemuxJobs(bool cancel) {
    if (cancel) {
        remux_cancel_requested_.store(true);
    }

    std::vector<std::unique_ptr<SegmentRemuxJob>> jobs;
    {
        std::lock_guard<std::mutex> lock(segment_remux_mutex_);
        jobs = std::move(segment_remux_jobs_);
    }

    bool all_succeeded = true;
    for (auto& job : jobs) {
        if (job->thread.joinable()) {
            job->thread.join();
        }
        if (!job->succeeded) {
            all_succeeded = false;
        }
    }
    return all_succeeded;
}

uint64_t RecordingCoordinator::PendingRemuxReserveBytes() const {
    // Sum the on-disk sizes of all transient MKV files for jobs that are still
    // in flight (the thread has not yet exited or cleaned up the file).
    std::lock_guard<std::mutex> lock(segment_remux_mutex_);
    uint64_t total = 0;
    for (const auto& job : segment_remux_jobs_) {
        if (job->thread.joinable()) {
            // Thread still running — include the transient MKV in the reserve.
            std::error_code ec;
            const auto sz = std::filesystem::file_size(job->transient_mkv, ec);
            if (!ec)
                total += static_cast<uint64_t>(sz);
        }
    }
    return total;
}

void RecordingCoordinator::CancelRemux() {
    remux_cancel_requested_.store(true);
}

bool RecordingCoordinator::IsRemuxing() const noexcept {
    return is_remuxing_.load();
}

void RecordingCoordinator::SetRemuxProgressCallback(RemuxProgressCallback cb) {
    on_remux_progress_ = std::move(cb);
}

void RecordingCoordinator::PostRemuxProgress(float fraction) {
    if (!on_remux_progress_)
        return;
    auto cb = on_remux_progress_;
    if (QCoreApplication::instance() == nullptr) {
        cb(fraction);
        return;
    }
    QMetaObject::invokeMethod(QCoreApplication::instance(), [cb, fraction]() { cb(fraction); }, Qt::QueuedConnection);
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
    // Video bit depth (0.7.0): flows to UserRecorderConfig.bit_depth so the engine
    // tags 10-bit (HEVC Main10 / AV1 10-bit P010) when selected. ReconcileContainerCodecs
    // above may have forced H.264 for the container; reset to 8-bit when the resolved
    // codec cannot carry 10-bit (the resolver applies the same Bit8 fallback).
    if (output_settings_.bit_depth == capability::BitDepth::Bit10 &&
        output_settings_.video_codec != capability::VideoCodec::HevcNvenc &&
        output_settings_.video_codec != capability::VideoCodec::Av1Nvenc) {
        output_settings_.bit_depth = capability::BitDepth::Bit8;
    }
    resolved_user_config_.bit_depth = output_settings_.bit_depth;
    // Colour range (0.7.0): always valid for every codec/container, so it flows
    // straight through (no reconcile) to UserRecorderConfig.color_range and on to
    // the engine's ColorMetadata.range.
    resolved_user_config_.color_range = output_settings_.color_range;
    ApplyOutputSettingsToUserConfig(resolved_user_config_, output_settings_);

    // Translate the UI split policy into the engine settings applied at start.
    // Both duration_ms and size_bytes are independent thresholds (ADR 0021);
    // 0 means that dimension is disabled. Whichever is hit first triggers the split.
    SanitizeSplitSettings(output_settings_.split);
    split_settings_.duration_ms = SplitDurationMs(output_settings_.split);
    split_settings_.size_bytes = SplitSizeBytes(output_settings_.split);
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
void RecordingCoordinator::SetDiagnosticsCallback(DiagnosticsUpdatedCallback cb) {
    on_diagnostics_updated_ = std::move(cb);
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
        const std::wstring folder = EffectiveOutputFolder().wstring();
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

        const std::wstring folder = EffectiveOutputFolder().wstring();
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
        last_media_time_ns_ = stats.video_duration_ns;
    }
    if (!on_stats_updated_)
        return;
    auto cb = on_stats_updated_;
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [cb, s = std::move(stats)]() { cb(s); }, Qt::QueuedConnection);
}

void RecordingCoordinator::PostDiagnostics(recorder_core::RecordingDiagnosticsSnapshot snapshot) {
    if (!on_diagnostics_updated_) {
        return;
    }
    // Generation guard: drop snapshots from an older session so a stale recording's
    // late callback can never update a newer recording's view. Applied on the posting
    // side (never capturing `this` into the queued lambda, matching the other Post*).
    {
        std::lock_guard<std::mutex> lock(diagnostics_guard_mutex_);
        if (!diagnostics_guard_.Accept(snapshot)) {
            return;
        }
    }
    auto cb = on_diagnostics_updated_;
    if (QCoreApplication::instance() == nullptr) {
        cb(snapshot);
        return;
    }
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [cb, s = std::move(snapshot)]() { cb(s); }, Qt::QueuedConnection);
}

void RecordingCoordinator::EmitInitializingDiagnostics() {
    recorder_core::RecordingDiagnosticsSnapshot init;
    init.lifecycle = recorder_core::DiagnosticsLifecycle::Initializing;
    init.valid = false;
    {
        std::lock_guard<std::mutex> lock(diagnostics_guard_mutex_);
        init.session_generation = diagnostics_guard_.max_generation();
    }
    PostDiagnostics(std::move(init));
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

std::filesystem::path RecordingCoordinator::EffectiveOutputFolder() const {
    // EXOSNAP_OUTPUT_DIR runtime override: when set to a non-empty value it takes
    // precedence over the user-configured output_settings_.output_folder.  This
    // mirrors the EXOSNAP_CONFIG_DIR pattern (ConfigPaths.h) and lets the capture
    // harness and CI redirect recordings into a temp directory without modifying any
    // persisted settings.  The override is never written back to disk.
    const QString override_dir = qEnvironmentVariable("EXOSNAP_OUTPUT_DIR");
    if (!override_dir.isEmpty())
        return std::filesystem::path(override_dir.toStdWString());
    return output_settings_.output_folder;
}

std::filesystem::path RecordingCoordinator::GenerateOutputPath() const {
    FilenameTargetContext context = output_target_context_;
    context.video_codec = output_settings_.video_codec;
    context.audio_codec = output_settings_.audio_codec;
    return BuildOutputPath(EffectiveOutputFolder(), output_settings_.naming_pattern, output_settings_.container,
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

    const uint64_t time_ms = last_media_time_ns_ / 1000000ULL;

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

    // Delegate to the canonical serializer (models/MarkerSidecar.h) so the edit
    // surface and the coordinator produce one identical format at one path.
    const QString media = QString::fromStdWString(current_output_path_.filename().wstring());
    if (!exosnap::WriteMarkerSidecar(sidecar_path, snapshot, media)) {
        diagnostics::AppLog::warning(
            QStringLiteral("marker"),
            QStringLiteral("sidecar write failed: %1").arg(QString::fromStdWString(sidecar_path.wstring())));
    }
}

void RecordingCoordinator::WriteSegmentMarkerSidecar(const recorder_core::CompletedSegment& segment) {
    std::vector<RecordingMarker> snapshot;
    {
        std::lock_guard<std::mutex> lock(markers_mutex_);
        snapshot = markers_;
    }
    if (snapshot.empty())
        return;

    // Partition + rebase to segment-local time. The half-open window excludes the
    // boundary marker so a paused-split marker is never duplicated into both
    // segments (it lands in the next segment at 0 ms). duration_ms == 0 yields an
    // empty window -> no sidecar.
    const std::vector<RecordingMarker> local =
        PartitionSegmentMarkers(snapshot, segment.session_start_ms, segment.duration_ms);

    // Zero-marker segment => no sidecar at all (no orphan files).
    if (local.empty())
        return;

    auto sidecar_path = segment.path;
    sidecar_path.replace_extension(L".markers.json");

    // Same canonical serializer, with the per-segment index field set.
    const QString media = QString::fromStdWString(segment.path.filename().wstring());
    const QString sidecar_qstr = QString::fromStdWString(sidecar_path.wstring());
    if (exosnap::WriteMarkerSidecar(sidecar_path, local, media, static_cast<int>(segment.index))) {
        diagnostics::AppLog::info(QStringLiteral("marker"),
                                  QStringLiteral("segment sidecar markers=%1 index=%2 path=%3")
                                      .arg(local.size())
                                      .arg(segment.index)
                                      .arg(sidecar_qstr));
    } else {
        diagnostics::AppLog::warning(QStringLiteral("marker"),
                                     QStringLiteral("segment sidecar write failed: %1").arg(sidecar_qstr));
    }
}

} // namespace exosnap
