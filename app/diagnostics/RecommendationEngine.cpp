#include "RecommendationEngine.h"

#include "DiskSpaceThresholds.h"

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
                                           uint64_t output_drive_free_bytes, bool is_profile_supported,
                                           std::string output_filesystem_name)
    : caps_(caps), config_(config), monitor_refresh_rate_(monitor_refresh_rate),
      output_drive_free_bytes_(output_drive_free_bytes), is_profile_supported_(is_profile_supported),
      output_filesystem_name_(std::move(output_filesystem_name)) {
}

DiagnosticChecklist RecommendationEngine::Generate() const {
    DiagnosticChecklist checklist;
    checkRefreshRateMismatch(checklist);
    checkMp4CrashResilience(checklist);
    checkCodecAvailability(checklist);
    checkOutputDriveSpace(checklist);
    checkOutputFilesystem(checklist);
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
    if (output_drive_free_bytes_ == 0) {
        // 0 means "not queried" — skip to avoid false positives.
        return;
    }

    const double free_gb = static_cast<double>(output_drive_free_bytes_) / (1024.0 * 1024.0 * 1024.0);
    const std::string free_gb_str = std::to_string(free_gb).substr(0, 4);

    if (output_drive_free_bytes_ <= kHardStopFreeBytes) {
        // rec.007: hard-stop blocker — recording is blocked until free space is recovered.
        DiagnosticResult r = MakeResult("rec.007", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker,
                                        "Insufficient disk space — recording blocked",
                                        "Less than 500 MB free on the output drive. Recording cannot start.",
                                        "Free space: " + free_gb_str +
                                            " GB. "
                                            "At least 500 MB must be available before recording can begin. "
                                            "Free up disk space or switch to a different output drive.",
                                        free_gb_str + " GB free",
                                        "Free up disk space or change the output folder to a drive with more space.",
                                        "Clear temporary files or change output folder in Output settings.");
        checklist.has_blocker = true;
        checklist.results.push_back(std::move(r));
        return;
    }

    if (output_drive_free_bytes_ < kWarnFreeBytes) {
        // rec.005: soft warning — recording is still allowed but space is getting low.
        DiagnosticResult r =
            MakeResult("rec.005", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
                       "Output drive is low on space", "Less than 2 GB free on the output drive.",
                       "Free space: " + free_gb_str +
                           " GB. "
                           "Recording may stop automatically if space runs out during a session.",
                       free_gb_str + " GB free", "Free up disk space or switch to a different output drive.",
                       "Clear temporary files or change output folder in Output settings.");
        checklist.has_notice = true;
        checklist.results.push_back(std::move(r));
    }
}

void RecommendationEngine::checkOutputFilesystem(DiagnosticChecklist& checklist) const {
    if (output_filesystem_name_.empty()) {
        // Empty means "not queried" — skip to avoid false positives.
        return;
    }

    // Only FAT32 requires a warning.  NTFS, exFAT, and any other filesystem
    // pass silently.  Unknown filesystems (unexpected names) also pass silently
    // rather than emitting a spurious warning for network drives or future
    // filesystems.
    if (output_filesystem_name_ != "FAT32") {
        return;
    }

    // rec.008: FAT32 output volume — Notice (not Blocker).
    //
    // Rationale: recordings under 4 GiB succeed on FAT32 without any issue.
    // The limit only matters for long sessions.  Blocking recording start would
    // prevent legitimate use of FAT32 volumes for short clips.  The user is
    // informed and can act before starting a long recording.
    DiagnosticResult r = MakeResult(
        "rec.008", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
        "Output volume uses FAT32 — 4 GiB file size limit",
        "FAT32 volumes cannot store files larger than 4 GiB. Long recordings will fail when this limit is reached.",
        "The configured output folder is on a FAT32 volume. A single recording file cannot exceed 4,294,967,295 bytes "
        "(~4 GiB). High-bitrate or long recordings will be cut off once the limit is reached.",
        "Filesystem: FAT32",
        "Move the output folder to an NTFS or exFAT volume to remove the 4 GiB per-file restriction.",
        "Change the output folder in Output settings to a drive formatted with NTFS or exFAT.");
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
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
    return {"rec.001", "rec.002", "rec.003", "rec.004", "rec.005", "rec.006", "rec.007", "rec.008"};
}

} // namespace exosnap::diagnostics
