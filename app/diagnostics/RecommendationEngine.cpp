#include "RecommendationEngine.h"

#include <capability/support_level.h>

#include <chrono>

namespace exosnap::diagnostics {

namespace {

uint64_t NowTimestamp() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

DiagnosticResult MakeResult(const std::string& id, DiagnosticGroup group, DiagnosticSeverity sev,
                            const std::string& title, const std::string& summary, const std::string& detail = "",
                            const std::string& current_value = "", const std::string& recommendation = "",
                            const std::string& optional_fix = "") {
    DiagnosticResult r;
    r.id = id;
    r.group = group;
    r.severity = sev;
    r.title = title;
    r.summary = summary;
    r.detail = detail;
    r.current_value = current_value;
    r.recommendation = recommendation;
    r.optional_fix = optional_fix;
    r.timestamp = NowTimestamp();
    return r;
}

} // namespace

RecommendationEngine::RecommendationEngine(const capability::CapabilitySet& caps,
                                           const capability::UserRecorderConfig& config, uint32_t monitor_refresh_rate,
                                           uint64_t output_drive_free_bytes, bool is_profile_supported)
    : caps_(caps), config_(config), monitor_refresh_rate_(monitor_refresh_rate),
      output_drive_free_bytes_(output_drive_free_bytes), is_profile_supported_(is_profile_supported) {
}

DiagnosticChecklist RecommendationEngine::Generate() const {
    DiagnosticChecklist checklist;
    checkRefreshRateMismatch(checklist);
    checkMp4CrashResilience(checklist);
    checkCodecAvailability(checklist);
    checkOutputDriveSpace(checklist);
    checkProfileSupport(checklist);
    return checklist;
}

void RecommendationEngine::checkRefreshRateMismatch(DiagnosticChecklist& checklist) const {
    if (monitor_refresh_rate_ >= 120 && config_.frame_rate_num == 60 && config_.frame_rate_den == 1) {
        DiagnosticResult r = MakeResult(
            "rec.001", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice, "Refresh rate / FPS mismatch",
            "144+ Hz monitor detected with 60 fps recording.",
            "Your monitor runs at " + std::to_string(monitor_refresh_rate_) +
                " Hz but recording is set to 60 fps. This can cause uneven frame pacing.",
            std::to_string(monitor_refresh_rate_) + " Hz monitor, " + std::to_string(config_.frame_rate_num) +
                " fps recording",
            "Cap your game at 60 or 120 fps for smoother pacing.", "Use in-game or driver-level frame rate limiter.");
        checklist.has_notice = true;
        checklist.results.push_back(std::move(r));
    }
}

void RecommendationEngine::checkMp4CrashResilience(DiagnosticChecklist& checklist) const {
    if (config_.container == capability::Container::Mp4) {
        DiagnosticResult r =
            MakeResult("rec.002", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
                       "MP4 is less crash-resilient than MKV",
                       "MP4 recordings may become unreadable if the app or system crashes during recording.",
                       "MP4 containers require finalization to write the moov atom. If recording is interrupted, "
                       "the file may be unrecoverable.",
                       "Container: MP4", "Consider switching to MKV for long or critical recordings.",
                       "Switch container to MKV in Output settings.");
        checklist.has_notice = true;
        checklist.results.push_back(std::move(r));
    }
}

void RecommendationEngine::checkCodecAvailability(DiagnosticChecklist& checklist) const {
    const auto& v_ann = caps_.QueryVideoCodec(config_.video_codec);
    if (!capability::IsSelectable(v_ann.level)) {
        std::string fallback = "H.264 (NVENC)";
        DiagnosticResult r = MakeResult(
            "rec.003", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker,
            "Selected video codec is unavailable", "The selected video codec is not available on this system.",
            "Codec: " + std::string(capability::ToString(config_.video_codec)) + ". Reason: " + v_ann.reason,
            "Unavailable", "Switch to " + fallback + " which is available.",
            "Change video codec in Format & Encoding settings.");
        checklist.has_blocker = true;
        checklist.results.push_back(std::move(r));
    }

    const auto& a_ann = caps_.QueryAudioCodec(config_.audio_codec);
    if (!capability::IsSelectable(a_ann.level)) {
        std::string fallback = "AAC";
        DiagnosticResult r = MakeResult(
            "rec.004", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker,
            "Selected audio codec is unavailable", "The selected audio codec is not available on this system.",
            "Codec: " + std::string(capability::ToString(config_.audio_codec)) + ". Reason: " + a_ann.reason,
            "Unavailable", "Switch to " + fallback + " which is available.",
            "Change audio codec in Format & Encoding settings.");
        checklist.has_blocker = true;
        checklist.results.push_back(std::move(r));
    }
}

void RecommendationEngine::checkOutputDriveSpace(DiagnosticChecklist& checklist) const {
    constexpr uint64_t kMinFreeBytes = 500ULL * 1024 * 1024; // 500 MB
    if (output_drive_free_bytes_ > 0 && output_drive_free_bytes_ < kMinFreeBytes) {
        const double free_gb = static_cast<double>(output_drive_free_bytes_) / (1024.0 * 1024.0 * 1024.0);
        DiagnosticResult r = MakeResult("rec.005", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
                                        "Output drive is low on space", "Less than 500 MB free on the output drive.",
                                        "Free space: " + std::to_string(free_gb).substr(0, 4) +
                                            " GB. "
                                            "Recording may fail if space runs out during a session.",
                                        std::to_string(free_gb).substr(0, 4) + " GB free",
                                        "Free up disk space or switch to a different output drive.",
                                        "Clear temporary files or change output folder in Output settings.");
        checklist.has_notice = true;
        checklist.results.push_back(std::move(r));
    }
}

void RecommendationEngine::checkProfileSupport(DiagnosticChecklist& checklist) const {
    if (!is_profile_supported_) {
        DiagnosticResult r = MakeResult(
            "rec.006", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker,
            "Recording profile is not supported",
            "The current recording profile cannot be used with available hardware.",
            "Your selected profile requires codecs or features not available on this system.", "Profile: unsupported",
            "Select an available profile or adjust settings to match available capabilities.",
            "Choose a different profile in Output settings.");
        checklist.has_blocker = true;
        checklist.results.push_back(std::move(r));
    }
}

std::vector<std::string> RecommendationEngine::GetAllRecommendationCodes() {
    return {"rec.001", "rec.002", "rec.003", "rec.004", "rec.005", "rec.006"};
}

} // namespace exosnap::diagnostics
