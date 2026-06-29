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
                            const std::string& current_value = "", const std::string& recommendation = "") {
    DiagnosticResult r;
    r.id = id;
    r.group = group;
    r.severity = sev;
    r.title = title;
    r.summary = summary;
    r.detail = detail;
    r.current_value = current_value;
    r.recommendation = recommendation;
    r.timestamp = NowTimestamp();
    return r;
}

} // namespace

RecommendationEngine::RecommendationEngine(const capability::CapabilitySet& caps,
                                           const capability::UserRecorderConfig& config, uint32_t monitor_refresh_rate,
                                           uint64_t output_drive_free_bytes, bool is_profile_supported,
                                           std::string output_filesystem_name,
                                           const recorder_core::RecordingDiagnosticsSnapshot* live_snapshot,
                                           const PresentSample* present)
    : caps_(caps), config_(config), monitor_refresh_rate_(monitor_refresh_rate),
      output_drive_free_bytes_(output_drive_free_bytes), is_profile_supported_(is_profile_supported),
      output_filesystem_name_(std::move(output_filesystem_name)) {
    // Consume the optional live snapshot only when it carries a real present-cadence
    // measurement (DXGI OD path, past warm-up). Everything else stays neutral.
    if (live_snapshot != nullptr && live_snapshot->valid &&
        live_snapshot->capture.present_cadence_availability == recorder_core::MetricAvailability::Available) {
        live_present_available_ = true;
        live_cfr_ = live_snapshot->video_encoder.cfr;
        live_present_jitter_ms_ = live_snapshot->capture.source_present_jitter_ms;
        live_coalesce_ratio_ = live_snapshot->capture.source_coalesce_ratio;
    }
    // Consume the optional present-mode sample only when the provider has a real observation.
    if (present != nullptr && present->available) {
        present_ = *present;
    }
}

DiagnosticChecklist RecommendationEngine::Generate() const {
    DiagnosticChecklist checklist;
    checkRefreshRateMismatch(checklist);
    checkExclusiveFullscreen(checklist);
    checkMp4CrashResilience(checklist);
    checkCodecAvailability(checklist);
    checkOutputDriveSpace(checklist);
    checkOutputFilesystem(checklist);
    checkProfileSupport(checklist);
    checkAudioContainerCompat(checklist);
    checkVideoBitDepthContainerCompat(checklist);
    checkDpcLatency(checklist);
    return checklist;
}

void RecommendationEngine::checkRefreshRateMismatch(DiagnosticChecklist& checklist) const {
    // Static heuristic: a high-refresh monitor paired with a 60 fps capture is a common
    // source of uneven pacing. monitor_refresh_rate_ == 0 means "unknown" and suppresses
    // the static arm (the live arm below can still fire on its own).
    const bool static_mismatch =
        monitor_refresh_rate_ >= 120 && config_.frame_rate_num == 60 && config_.frame_rate_den == 1;

    // Live correlation (v0.8.0 / ADR 0033): during CFR capture, measured present-time jitter
    // or sustained coalescing is direct evidence that the source presents at a rate (or with
    // a variability, e.g. VRR) that the fixed capture cadence cannot follow smoothly.
    //
    // Thresholds are deliberately conservative and empirically calibratable:
    //   kJitterMs = 4.0 ms — peak-minus-average present interval. Normal scheduler/QPC
    //                        sampling noise over the 2 s window stays well under this even at
    //                        120 Hz (~8.3 ms nominal interval); a sustained >4 ms spread
    //                        signals irregular present pacing (judder), not measurement noise.
    //   kCoalesceRatio = 1.5 — mean desktop updates coalesced per acquire. >1.5 means the
    //                        source consistently presents faster than the CFR sampling rate
    //                        (1.5 rather than 1.0 to ignore the occasional double-update).
    constexpr double kJitterMs = 4.0;
    constexpr double kCoalesceRatio = 1.5;
    const bool live_judder = live_present_available_ && live_cfr_ &&
                             (live_present_jitter_ms_ > kJitterMs || live_coalesce_ratio_ > kCoalesceRatio);

    if (!static_mismatch && !live_judder) {
        return;
    }

    const std::string refresh_str =
        monitor_refresh_rate_ > 0 ? std::to_string(monitor_refresh_rate_) + " Hz" : std::string("High-refresh");

    DiagnosticResult r;
    if (live_judder) {
        // Higher-confidence diagnosis backed by live present-pacing measurement.
        const std::string jitter_str = std::to_string(live_present_jitter_ms_).substr(0, 4);
        const std::string coalesce_str = std::to_string(live_coalesce_ratio_).substr(0, 4);
        std::string detail = "Live capture telemetry shows ";
        bool wrote = false;
        if (live_present_jitter_ms_ > kJitterMs) {
            detail += "present-time jitter of " + jitter_str + " ms";
            wrote = true;
        }
        if (live_coalesce_ratio_ > kCoalesceRatio) {
            if (wrote)
                detail += " and ";
            detail += "sustained frame coalescing (" + coalesce_str + " source updates per captured frame)";
        }
        detail += " during constant-frame-rate recording. This indicates the source presents with "
                  "variable / refresh-driven timing (e.g. VRR) that the fixed capture rate cannot follow "
                  "smoothly, producing judder.";
        r = MakeResult("rec.001", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
                       "VRR / refresh-induced judder detected",
                       "Live present-pacing measurements indicate uneven frame delivery from the source.", detail,
                       "Measured present jitter " + jitter_str + " ms during CFR capture",
                       "Cap your game's frame rate (e.g. 60 or 120 fps) or disable VRR while recording for "
                       "smoother pacing.");
    } else {
        r = MakeResult("rec.001", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
                       "Refresh rate / FPS mismatch", "144+ Hz monitor detected with 60 fps recording.",
                       "Your monitor runs at " + refresh_str +
                           " but recording is set to 60 fps. This can cause uneven frame pacing.",
                       refresh_str + " monitor, " + std::to_string(config_.frame_rate_num) + " fps recording",
                       "Cap your game at 60 or 120 fps for smoother pacing.");
    }

    // Present-mode attribution (PresentMon, ADR 0033): when available, name *how* the source
    // presents so the diagnosis reads as a root cause, not just a number.
    if (present_.has_value()) {
        switch (present_->mode) {
        case PresentMode::IndependentFlip:
            r.detail += " The source is presenting via independent flip (variable-rate "
                        "flip-model), which the fixed CFR cadence cannot phase-match.";
            break;
        case PresentMode::ExclusiveFullscreen:
            r.detail += " The source is in exclusive fullscreen; its present cadence is "
                        "independent of the desktop refresh.";
            break;
        default:
            break;
        }
    }

    FixAction fa;
    fa.id = "fix.fps.cap";
    fa.label = "Set recording FPS to match monitor";
    fa.safety = FixAction::Safety::Assisted;
    fa.reversible = true;
    fa.changes_summary =
        "Opens Video settings to adjust the recording frame rate to better match your monitor's refresh rate.";
    r.fix_action = fa;
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));

    // ADR 0035 / Task 6: when judder fires AND the user is on Newest pacing, offer a second
    // result (one primary fix_action per result) to switch to Smooth (phase-correct) pacing.
    // Smooth is the default and already eliminates this class of judder, so no fix is needed
    // when the user is already on Smooth.
    if (config_.frame_pacing == recorder_core::FramePacingMode::Newest) {
        DiagnosticResult pr = MakeResult(
            "rec.pacing.smooth", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
            "Smooth frame pacing recommended", "Phase-correct pacing removes judder from high-refresh / VRR sources.",
            "Your recording uses Newest frame pacing; the measured judder is exactly what "
            "Smooth (phase-correct) pacing fixes.",
            "Frame pacing: Newest", "Switch to Smooth frame pacing in Advanced Video settings.");
        FixAction pfa;
        pfa.id = "fix.frame_pacing.smooth";
        pfa.label = "Switch to Smooth pacing";
        pfa.safety = FixAction::Safety::Auto; // safe, reversible, config-only
        pfa.reversible = true;
        pfa.changes_summary =
            "Sets video frame pacing to Smooth (phase-correct). Reversible in Advanced Video settings.";
        pr.fix_action = pfa;
        checklist.has_notice = true;
        checklist.results.push_back(std::move(pr));
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
                       "Container: MP4", "Consider switching to MKV for long or critical recordings.");
        FixAction fa;
        fa.id = "fix.container.mkv";
        fa.label = "Switch to MKV";
        fa.safety = FixAction::Safety::Assisted;
        fa.reversible = true;
        fa.changes_summary =
            "Opens Output settings to change the recording container to MKV for better crash resilience.";
        r.fix_action = fa;
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
            "Unavailable", "Switch to " + fallback + " which is available.");
        FixAction fa;
        fa.id = "fix.codec.video.default";
        fa.label = "Switch to H.264 (NVENC)";
        fa.safety = FixAction::Safety::Auto;
        fa.reversible = true;
        fa.changes_summary = "Switches the video codec to H.264 (NVENC), which is available on this system.";
        r.fix_action = fa;
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
            "Unavailable", "Switch to " + fallback + " which is available.");
        FixAction fa;
        fa.id = "fix.codec.audio.default";
        fa.label = "Switch to AAC";
        fa.safety = FixAction::Safety::Auto;
        fa.reversible = true;
        fa.changes_summary = "Switches the audio codec to AAC, which is available on this system.";
        r.fix_action = fa;
        checklist.has_blocker = true;
        checklist.results.push_back(std::move(r));
    }
}

void RecommendationEngine::checkOutputDriveSpace(DiagnosticChecklist& checklist) const {
    if (!output_path_writable_) {
        // Output folder cannot be written to — a hard blocker (the muxer can't produce a
        // file). Surfaced here as a COUNTED blocker so the Diagnostics header reflects it
        // (red container + Blockers count). Previously this only showed on the pipeline
        // Disk card and never propagated up to the verdict.
        DiagnosticResult r =
            MakeResult("rec.output.writable", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker,
                       "Output folder is not writable",
                       "Recording cannot start — the selected output folder cannot be written to.",
                       "The writability probe failed to create a file in the output folder. Choose a different "
                       "folder or fix the folder's permissions.",
                       "Not writable", "Change the output folder to a writable location.");
        FixAction fa;
        fa.id = "fix.output.change_folder";
        fa.label = "Change output folder";
        fa.safety = FixAction::Safety::Assisted;
        fa.reversible = true;
        fa.changes_summary = "Opens Output settings to select a writable output folder.";
        r.fix_action = fa;
        checklist.has_blocker = true;
        checklist.results.push_back(std::move(r));
    }

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
                                        "Free up disk space or change the output folder to a drive with more space.");
        FixAction fa;
        fa.id = "fix.output.change_folder";
        fa.label = "Change output folder";
        fa.safety = FixAction::Safety::Assisted;
        fa.reversible = true;
        fa.changes_summary = "Opens Output settings to select an output folder on a drive with more free space.";
        r.fix_action = fa;
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
                       free_gb_str + " GB free", "Free up disk space or switch to a different output drive.");
        FixAction fa;
        fa.id = "fix.output.change_folder";
        fa.label = "Change output folder";
        fa.safety = FixAction::Safety::Assisted;
        fa.reversible = true;
        fa.changes_summary = "Opens Output settings to select an output folder on a drive with more free space.";
        r.fix_action = fa;
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
        "Move the output folder to an NTFS or exFAT volume to remove the 4 GiB per-file restriction.");
    FixAction fa;
    fa.id = "fix.output.fat32_folder";
    fa.label = "Change output folder";
    fa.safety = FixAction::Safety::Assisted;
    fa.reversible = true;
    fa.changes_summary =
        "Opens Output settings to move the output folder to an NTFS or exFAT volume (no 4 GiB file size limit).";
    r.fix_action = fa;
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
            "Select an available profile or adjust settings to match available capabilities.");
        FixAction fa;
        fa.id = "fix.profile.select";
        fa.label = "Choose a supported profile";
        fa.safety = FixAction::Safety::Assisted;
        fa.reversible = true;
        fa.changes_summary = "Opens Settings to select a recording profile supported by your hardware.";
        r.fix_action = fa;
        checklist.has_blocker = true;
        checklist.results.push_back(std::move(r));
    }
}

void RecommendationEngine::checkAudioContainerCompat(DiagnosticChecklist& checklist) const {
    if (config_.audio_codec == capability::AudioCodec::Flac && config_.container == capability::Container::Mp4) {
        DiagnosticResult r = MakeResult(
            "rec.009", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker, "FLAC is not supported in MP4",
            "FLAC audio cannot be muxed into an MP4 container. Switch to MKV or change the audio "
            "codec to AAC.",
            "FLAC audio cannot be muxed into an MP4 container. Switch to MKV or change the audio "
            "codec to AAC.",
            "Audio: FLAC, Container: MP4", "Switch the container to MKV or select a different audio codec.");
        FixAction fa;
        fa.id = "fix.audio.flac_to_mkv";
        fa.label = "Switch container to MKV";
        fa.safety = FixAction::Safety::Assisted;
        fa.reversible = true;
        fa.changes_summary =
            "Opens Output settings to change the recording container to MKV, which supports FLAC audio.";
        r.fix_action = fa;
        checklist.has_blocker = true;
        checklist.results.push_back(std::move(r));
        return;
    }

    if (config_.audio_codec == capability::AudioCodec::Opus && config_.container == capability::Container::Mp4) {
        DiagnosticResult r =
            MakeResult("rec.009", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
                       "Opus in MP4 has limited player compatibility",
                       "Opus audio in MP4 is not widely supported. AAC is the recommended audio codec for MP4.",
                       "Opus audio in MP4 is not widely supported. AAC is the recommended audio codec for MP4.",
                       "Audio: Opus, Container: MP4",
                       "Switch the audio codec to AAC for better compatibility with MP4 containers.");
        FixAction fa;
        fa.id = "fix.audio.opus_to_aac";
        fa.label = "Switch audio codec to AAC";
        fa.safety = FixAction::Safety::Auto;
        fa.reversible = true;
        fa.changes_summary = "Switches the audio codec to AAC for better compatibility with MP4 containers.";
        r.fix_action = fa;
        checklist.has_notice = true;
        checklist.results.push_back(std::move(r));
    }
}

void RecommendationEngine::checkVideoBitDepthContainerCompat(DiagnosticChecklist& checklist) const {
    if (config_.video_codec == capability::VideoCodec::HevcNvenc && config_.container == capability::Container::WebM) {
        DiagnosticResult r = MakeResult(
            "rec.010", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker, "HEVC is not supported in WebM",
            "WebM only supports AV1 and VP9 video codecs. HEVC (H.265) cannot be muxed into a "
            "WebM container.",
            "WebM only supports AV1 and VP9 video codecs. HEVC (H.265) cannot be muxed into a "
            "WebM container.",
            "Video: HEVC, Container: WebM", "Switch the container to MKV, which supports HEVC video.");
        FixAction fa;
        fa.id = "fix.video.hevc_webm";
        fa.label = "Switch container to MKV";
        fa.safety = FixAction::Safety::Assisted;
        fa.reversible = true;
        fa.changes_summary =
            "Opens Output settings to change the recording container to MKV, which supports HEVC video.";
        r.fix_action = fa;
        checklist.has_blocker = true;
        checklist.results.push_back(std::move(r));
        return;
    }

    if (config_.video_codec == capability::VideoCodec::H264Nvenc && config_.container == capability::Container::WebM) {
        DiagnosticResult r = MakeResult(
            "rec.010", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker, "H.264 is not supported in WebM",
            "WebM only supports AV1 and VP9 video codecs. H.264 cannot be muxed into a WebM "
            "container.",
            "WebM only supports AV1 and VP9 video codecs. H.264 cannot be muxed into a WebM "
            "container.",
            "Video: H.264, Container: WebM", "Switch the container to MKV, which supports H.264 video.");
        FixAction fa;
        fa.id = "fix.video.h264_webm";
        fa.label = "Switch container to MKV";
        fa.safety = FixAction::Safety::Assisted;
        fa.reversible = true;
        fa.changes_summary =
            "Opens Output settings to change the recording container to MKV, which supports H.264 video.";
        r.fix_action = fa;
        checklist.has_blocker = true;
        checklist.results.push_back(std::move(r));
    }
}

void RecommendationEngine::checkExclusiveFullscreen(DiagnosticChecklist& checklist) const {
    if (!present_.has_value() || present_->mode != PresentMode::ExclusiveFullscreen) {
        return;
    }
    DiagnosticResult r;
    r.id = "rec.present.exclusive";
    r.group = DiagnosticGroup::Recommendation;
    r.severity = DiagnosticSeverity::Notice;
    r.title = "Captured source is in exclusive fullscreen";
    r.summary = "Captured source is in exclusive fullscreen";
    r.detail = "The source presents in legacy exclusive fullscreen. Desktop/window capture often records "
               "a black frame in this mode. Switch the game to borderless (windowed-fullscreen) so the "
               "compositor can present it for capture.";
    r.current_value = "Present mode: Exclusive fullscreen";
    r.recommendation = "Set the game to Borderless / Windowed Fullscreen.";
    r.timestamp = NowTimestamp();

    FixAction fa;
    fa.id = "fix.present.borderless";
    fa.label = "How to switch to borderless";
    fa.safety = FixAction::Safety::Assisted; // app cannot flip a foreign game's display mode
    fa.reversible = true;
    fa.changes_summary = "Opens guidance for switching the captured game to borderless fullscreen (the app cannot "
                         "change another application's display mode for you).";
    r.fix_action = fa;
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
}

void RecommendationEngine::checkDpcLatency(DiagnosticChecklist& checklist) const {
    constexpr double kDpcThresholdUs = 1000.0; // 1 ms sustained DPC = audible/stutter risk
    if (!dpc_.has_value() || !dpc_->available || dpc_->max_latency_us <= kDpcThresholdUs) {
        return;
    }
    const std::string driver = dpc_->worst_driver.empty() ? "an unidentified kernel driver" : dpc_->worst_driver;
    const std::string max_str = std::to_string(static_cast<long>(dpc_->max_latency_us));
    DiagnosticResult r = MakeResult(
        "rec.dpc.latency", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
        "High kernel DPC/ISR latency detected",
        "Kernel driver latency can cause recording stutter even when the game feels smooth.",
        "Peak DPC latency reached " + max_str + " us, attributed to " + driver +
            ". High DPC latency causes recording stutter/audio crackle even when the game itself "
            "feels smooth.",
        "Max DPC: " + max_str + " us", "Update or roll back " + driver + " (GPU/audio/network/chipset driver).");
    FixAction fa;
    fa.id = "fix.dpc.driver";
    fa.label = "Driver latency guidance";
    fa.safety = FixAction::Safety::External; // app cannot change kernel drivers
    fa.reversible = false;
    fa.changes_summary = "Shows which driver to update/roll back; the app cannot change it for you.";
    r.fix_action = fa;
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
}

std::vector<std::string> RecommendationEngine::GetAllRecommendationCodes() {
    return {"rec.001", "rec.002", "rec.003", "rec.004", "rec.005",
            "rec.006", "rec.007", "rec.008", "rec.009", "rec.010"};
}

} // namespace exosnap::diagnostics
