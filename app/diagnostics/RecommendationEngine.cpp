#include "RecommendationEngine.h"

#include "DiskSpaceThresholds.h"

#include <capability/codec_selection.h>
#include <capability/container_compat_registry.h>
#include <capability/support_level.h>

#include <chrono>
#include <cmath>
#include <string>

namespace exosnap::diagnostics {

namespace {

uint64_t NowTimestamp() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// Every diagnostic declares its own tier here, at the diagnosis site — the tier
// is part of the diagnosis, never inferred downstream by an id allowlist. The
// bucketing + honesty rules (IsAlwaysVisible / BundlesIntoTipChip) read this field.
DiagnosticResult MakeResult(const std::string& id, DiagnosticGroup group, DiagnosticSeverity sev, DiagnosticTier tier,
                            const std::string& title, const std::string& summary, const std::string& detail = "",
                            const std::string& current_value = "", const std::string& recommendation = "") {
    DiagnosticResult r;
    r.id = id;
    r.group = group;
    r.severity = sev;
    r.tier = tier;
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
                                           const capability::UserRecorderConfig& config,
                                           std::optional<uint64_t> output_drive_free_bytes, bool is_profile_supported,
                                           std::string output_filesystem_name,
                                           const exosnap::engine::RecordingDiagnosticsSnapshot* live_snapshot,
                                           const PresentSample* present)
    : caps_(caps), config_(config), output_drive_free_bytes_(output_drive_free_bytes),
      is_profile_supported_(is_profile_supported), output_filesystem_name_(std::move(output_filesystem_name)) {
    // Consume the optional live snapshot only when it carries a real present-cadence
    // measurement (DXGI OD path, past warm-up). Everything else stays neutral.
    if (live_snapshot != nullptr && live_snapshot->valid &&
        live_snapshot->capture.present_cadence_availability == exosnap::engine::MetricAvailability::Available) {
        live_present_available_ = true;
        live_cfr_ = live_snapshot->video_encoder.cfr;
        live_present_jitter_ms_ = live_snapshot->capture.source_present_jitter_ms;
    }
    // Consume the optional present-mode sample only when the provider has a real observation.
    if (present != nullptr && present->available) {
        present_ = *present;
    }
    // Consume live disk-write latency only when the writer reports it (streaming Matroska;
    // the MP4 post-stop remux marks it Unavailable rather than a fake zero).
    if (live_snapshot != nullptr && live_snapshot->valid &&
        live_snapshot->disk.latency_availability == exosnap::engine::MetricAvailability::Available) {
        live_disk_write_available_ = true;
        live_disk_peak_write_ms_ = live_snapshot->disk.peak_write_ms;
    }
    // Consume live audio health + format (ADR 0046). Available only while an audio
    // track is actually capturing; idle/no-audio recordings leave it neutral.
    if (live_snapshot != nullptr && live_snapshot->valid &&
        live_snapshot->lifecycle == exosnap::engine::DiagnosticsLifecycle::Recording) {
        live_capture_available_ = true;
        live_capture_starved_ = live_snapshot->capture.capture_starved;
        live_frames_emitted_ = live_snapshot->capture.frames_emitted;
        live_frames_duplicated_ = live_snapshot->capture.frames_duplicated;
        live_target_fps_ = live_snapshot->capture.target_fps;
    }
    if (live_snapshot != nullptr && live_snapshot->valid &&
        live_snapshot->bottleneck == exosnap::engine::PipelineBottleneck::Gpu) {
        live_gpu_contention_ = true;
        live_gpu_exec_p99_ms_ = live_snapshot->compositor.gpu_exec_p99_ms;
        live_target_fps_for_gpu_ = live_snapshot->capture.target_fps;
    }
    if (live_snapshot != nullptr && live_snapshot->valid &&
        live_snapshot->av_drift_availability == exosnap::engine::MetricAvailability::Available) {
        live_clock_saturated_ = live_snapshot->clock_slaving_saturated;
        live_clock_ppm_ = live_snapshot->clock_slaving_ppm;
    }
    if (live_snapshot != nullptr && live_snapshot->valid && live_snapshot->audio.active) {
        live_audio_available_ = true;
        live_audio_degraded_sources_ = live_snapshot->audio.degraded_sources;
        live_audio_endpoint_in_use_ = live_snapshot->audio.degraded_endpoint_in_use;
        live_audio_track_count_ = live_snapshot->audio.track_count;
        if (live_snapshot->audio.sample_rate > 0 && live_snapshot->audio.channels > 0) {
            live_audio_format_available_ = true;
            live_audio_sample_rate_ = live_snapshot->audio.sample_rate;
            live_audio_channels_ = live_snapshot->audio.channels;
        }
    }
}

DiagnosticChecklist RecommendationEngine::Generate() const {
    DiagnosticChecklist checklist;
    checkRefreshRateMismatch(checklist);
    checkExclusiveWindowTarget(checklist);
    checkExclusiveFullscreen(checklist);
    checkDiscardedPresents(checklist);
    checkPresentModeFlips(checklist);
    checkMp4CrashResilience(checklist);
    checkCodecAvailability(checklist);
    checkRecommendedCodec(checklist);
    checkColorRange(checklist);
    checkHdrH264Blocker(checklist);
    checkOutputDriveSpace(checklist);
    checkOutputFilesystem(checklist);
    checkProfileSupport(checklist);
    checkAudioContainerCompat(checklist);
    checkVideoBitDepthContainerCompat(checklist);
    checkDpcLatency(checklist);
    checkDiskWriteStall(checklist);
    checkUnresolvedSavedDisplay(checklist);
    checkAudioSourceDegraded(checklist);
    checkFramePacingDuplication(checklist);
    checkAudioClockSaturated(checklist);
    checkCaptureAdapterMismatch(checklist);
    checkOutputDriveKind(checklist);
    checkGpuContention(checklist);
    return checklist;
}

void RecommendationEngine::checkRefreshRateMismatch(DiagnosticChecklist& checklist) const {
    // Measured-symptom only — NO static config nag. The Smooth phase-correct frame selection
    // (default pacing, ADR 0035) already absorbs the common high-refresh / VRR → CFR case, so
    // a static "144 Hz + 60 fps" warning would just nag on a setup that records fine. We fire
    // ONLY on measured residual present-time jitter: irregular source delivery that even
    // best-frame selection at a fixed output rate cannot fully smooth. Sustained coalescing
    // (source presenting faster than the CFR tick) is NORMAL for high-refresh sources and is
    // exactly what the resampler handles — so it is no longer a trigger either.
    //
    //   kJitterMs = 8.0 ms — peak-minus-average present interval. Raised from the
    //     pre-resampler 4 ms: the resampler absorbs moderate jitter, so only a sustained
    //     spread approaching half a 60 fps output interval (~8.3 ms) signals judder it could
    //     not hide. Conservative + empirically calibratable.
    constexpr double kJitterMs = 8.0;
    const bool live_judder = live_present_available_ && live_cfr_ && live_present_jitter_ms_ > kJitterMs;
    if (!live_judder) {
        return;
    }

    const std::string jitter_str = std::to_string(live_present_jitter_ms_).substr(0, 4);
    const std::string detail =
        "Live capture telemetry shows present-time jitter of " + jitter_str +
        " ms during constant-frame-rate recording. The source presents with variable / refresh-driven "
        "timing (e.g. VRR) irregular enough that even phase-correct frame selection cannot fully smooth "
        "it at a fixed output rate, producing residual judder.";
    DiagnosticResult r =
        MakeResult("rec.001", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
                   DiagnosticTier::MeasuredProblem, "VRR / refresh-induced judder detected",
                   "Live present-pacing measurements indicate uneven frame delivery from the source.", detail,
                   "Measured present jitter " + jitter_str + " ms during CFR capture",
                   "Cap your game's frame rate (e.g. 60 or 120 fps) or disable VRR while recording for "
                   "smoother pacing.");
    r.measured_value = live_present_jitter_ms_;
    r.budget_value = kJitterMs;
    r.value_unit = "ms";

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
    if (config_.frame_pacing == exosnap::engine::FramePacingMode::Newest) {
        DiagnosticResult pr = MakeResult(
            "rec.pacing.smooth", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
            DiagnosticTier::MeasuredProblem, "Phase-correct frame pacing recommended",
            "Phase-correct pacing removes judder from high-refresh / VRR sources.",
            "Your recording uses Lowest latency frame pacing; the measured judder is exactly what "
            "Phase-correct pacing fixes.",
            "Frame pacing: Lowest latency", "Switch to Phase-correct frame pacing in Advanced Video settings.");
        FixAction pfa;
        pfa.id = "fix.frame_pacing.smooth";
        pfa.label = "Switch to Phase-correct pacing";
        pfa.safety = FixAction::Safety::Auto; // safe, reversible, config-only
        pfa.reversible = true;
        pfa.changes_summary = "Sets video frame pacing to Phase-correct. Reversible in Advanced Video settings.";
        pr.fix_action = pfa;
        checklist.has_notice = true;
        checklist.results.push_back(std::move(pr));
    }
}

void RecommendationEngine::checkMp4CrashResilience(DiagnosticChecklist& checklist) const {
    if (config_.container == capability::Container::Mp4) {
        DiagnosticResult r =
            MakeResult("rec.002", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
                       DiagnosticTier::Optimisation, "MP4 is less crash-resilient than MKV",
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
            "rec.003", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker, DiagnosticTier::Blocker,
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
            "rec.004", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker, DiagnosticTier::Blocker,
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

void RecommendationEngine::checkRecommendedCodec(DiagnosticChecklist& checklist) const {
    // Capability-truthful "use the best codec your GPU supports" recommendation.
    // Shares the ONE resolver (BestAvailableVideoCodec) with the MainWindow FixAction
    // handler so the recommended codec can never drift from the applied one.
    const std::optional<capability::VideoCodec> best = capability::BestAvailableVideoCodec(caps_, config_.container);
    if (!best.has_value()) {
        return; // nothing GPU-supported + container-valid — the availability checks own this case
    }

    // Quality/efficiency rank (best first). Only recommend a STRICTLY better codec, so the
    // default (AV1 on a modern GPU) stays silent.
    const auto rank = [](capability::VideoCodec v) -> int {
        switch (v) {
        case capability::VideoCodec::Av1:
            return 0;
        case capability::VideoCodec::Hevc:
            return 1;
        case capability::VideoCodec::H264:
            return 2;
        }
        return 99;
    };
    if (*best == config_.video_codec || rank(*best) >= rank(config_.video_codec)) {
        return;
    }

    const std::string best_label(capability::VisibleVideoCodecLabel(*best));
    const std::string current_label(capability::VisibleVideoCodecLabel(config_.video_codec));

    DiagnosticResult r = MakeResult(
        "rec.profile.codec", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice, DiagnosticTier::Optimisation,
        "A better GPU-supported codec is available",
        "Your GPU supports " + best_label + ", which encodes with better quality and efficiency than " + current_label +
            " at the same bitrate.",
        "This GPU can hardware-encode " + best_label + " for the current container, but the profile is set to " +
            current_label + ". " + best_label + " produces smaller files at equal quality.",
        "Video codec: " + current_label, "Switch the video codec to " + best_label + ".");
    FixAction fa;
    fa.id = "fix.profile.codec.best";
    fa.label = "Switch to " + best_label;
    fa.safety = FixAction::Safety::Auto; // config-only, reversible
    fa.reversible = true;
    fa.changes_summary = "Video codec: " + current_label + " -> " + best_label;
    r.fix_action = fa;
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
}

void RecommendationEngine::checkColorRange(DiagnosticChecklist& checklist) const {
    // rec.color.range: Full (0-255) colour range is a technically valid, expert-only choice,
    // but widely-used players (e.g. VLC) ignore the range flag and always expand the picture
    // as Limited (16-235), so Full-range recordings can appear crushed/too dark there. Limited
    // is the industry-standard, compatible-everywhere choice (matching OBS et al.) and is the
    // recommended pick unless the whole downstream playback/edit chain is known to honour Full.
    if (config_.color_range != capability::ColorRange::Full) {
        return;
    }
    DiagnosticResult r =
        MakeResult("rec.color.range", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
                   DiagnosticTier::Optimisation, "Full color range is set",
                   "Common players such as VLC display full-range video too dark. Limited is the compatible choice.",
                   "The recording is configured with Full (0-255) colour range. Several widely-used players, "
                   "including VLC, ignore the range flag and always expand playback as Limited (16-235), so "
                   "Full-range recordings can look crushed or too dark in those players. Limited range decodes "
                   "correctly everywhere, including players that do read the range flag.",
                   "Colour range: Full",
                   "Switch to Limited colour range for compatibility with players that ignore the range flag.");
    FixAction fa;
    fa.id = "fix.color.range";
    fa.label = "Switch to Limited";
    fa.safety = FixAction::Safety::Auto; // config-only, reversible
    fa.reversible = true;
    fa.changes_summary = "Colour range: Full -> Limited";
    r.fix_action = fa;
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
}

void RecommendationEngine::checkHdrH264Blocker(DiagnosticChecklist& checklist) const {
    // rec.hdr.h264: HDR10-native recording requires a 10-bit codec. H.264 has no
    // 10-bit/HDR10 path, so pairing it with HdrMode::Hdr10 would produce a broken
    // or silently-downgraded HDR output. This is a real, hard conflict — a blocker.
    //
    // Three gates, ALL required (calm-diagnostics line — only a real problem fires):
    //   1. HdrMode::Hdr10 selected (H.264 + TonemapSdr is explicitly NOT a conflict:
    //      that path outputs SDR 8-bit, which H.264 handles fine).
    //   2. The chosen codec is not HDR10-native — gated on the explicit capability
    //      annotation (caps_.QueryHdr10Native), never a codec-name compare.
    //   3. The capture target's display is HDR-active. HDR10 auto-detects: on an SDR
    //      desktop the native path never engages, so there is no real conflict. The
    //      caller supplies this via SetCaptureTargetHdrActive (default false).
    if (config_.hdr_mode != exosnap::engine::HdrMode::Hdr10) {
        return;
    }
    if (capability::IsSelectable(caps_.QueryHdr10Native(config_.video_codec).level)) {
        return; // codec can carry HDR10 (HEVC/AV1) — no conflict
    }
    if (!capture_target_hdr_active_) {
        return; // SDR desktop — HDR10-native path does not engage
    }

    // Prefer AV1 (best quality/efficiency); fall back to HEVC when AV1 is not a real
    // fix here. Both are hdr10_native-capable per the QueryHdr10Native gate above, so
    // either resolves the HDR conflict — but AV1 only qualifies when it is BOTH
    //   (a) GPU-selectable — otherwise applying it just lands the user in the
    //       codec-unavailable blocker (rec.003), and
    //   (b) a working combo in the current container — Recommended/Allowed, the same
    //       criterion ContainerCompatRegistry::ReconcileCodecs enforces. Without this,
    //       MP4 (where AV1+AAC is Experimental) confirms "H.264 -> AV1" and the
    //       MainWindow handler's ReconcileContainerCodecs silently reverts it to
    //       H.264 — the fix self-reverts and the blocker re-fires.
    const capability::ContainerCompatLevel av1_combo =
        capability::ContainerCompatRegistry::Query(config_.container, capability::VideoCodec::Av1, config_.audio_codec)
            .level;
    const bool av1_is_working_combo = av1_combo == capability::ContainerCompatLevel::Recommended ||
                                      av1_combo == capability::ContainerCompatLevel::Allowed;
    const capability::VideoCodec proposed_codec =
        capability::IsSelectable(caps_.QueryVideoCodec(capability::VideoCodec::Av1)) && av1_is_working_combo
            ? capability::VideoCodec::Av1
            : capability::VideoCodec::Hevc;
    const std::string current_label(capability::VisibleVideoCodecLabel(config_.video_codec));
    const std::string proposed_label(capability::VisibleVideoCodecLabel(proposed_codec));
    const std::string fix_id =
        proposed_codec == capability::VideoCodec::Av1 ? "fix.hdr.codec.av1" : "fix.hdr.codec.hevc";
    DiagnosticResult r = MakeResult(
        "rec.hdr.h264", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker, DiagnosticTier::Blocker,
        current_label + " cannot record HDR10",
        current_label + " has no 10-bit/HDR10 path. Switch to " + proposed_label + " to record the HDR signal.",
        "HDR10 recording is enabled and the capture target's display is in HDR, but " + current_label +
            " is an 8-bit-only codec with no HDR10 (10-bit/P010, PQ/BT.2020) path. AV1 and HEVC can "
            "carry the native HDR10 signal.",
        "Video codec: " + current_label + ", HDR: HDR10 (native)", "Switch the video codec to " + proposed_label + ".");
    FixAction fa;
    fa.id = fix_id;
    fa.label = "Switch to " + proposed_label;
    fa.safety = FixAction::Safety::Auto; // config-only, reversible
    fa.reversible = true;
    fa.changes_summary = "Video codec: " + current_label + " -> " + proposed_label;
    r.fix_action = fa;
    checklist.has_blocker = true;
    checklist.results.push_back(std::move(r));
}

void RecommendationEngine::checkOutputDriveSpace(DiagnosticChecklist& checklist) const {
    if (!output_path_writable_) {
        // Output folder cannot be written to — a hard blocker (the muxer can't produce a
        // file). Surfaced here as a COUNTED blocker so the Diagnostics header reflects it
        // (red container + Blockers count). Previously this only showed on the pipeline
        // Disk card and never propagated up to the verdict.
        DiagnosticResult r =
            MakeResult("rec.output.writable", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker,
                       DiagnosticTier::Blocker, "Output folder is not writable",
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

    if (!output_drive_free_bytes_.has_value()) {
        // The volume could not be queried (unreachable share, access denied).
        // Stay silent rather than guess; a zero here would be a full disk.
        return;
    }

    const uint64_t free_bytes = *output_drive_free_bytes_;
    const double free_gb = static_cast<double>(free_bytes) / (1024.0 * 1024.0 * 1024.0);
    const std::string free_gb_str = std::to_string(free_gb).substr(0, 4);

    if (free_bytes <= kHardStopFreeBytes) {
        // rec.007: hard-stop blocker — recording is blocked until free space is recovered.
        DiagnosticResult r = MakeResult("rec.007", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker,
                                        DiagnosticTier::Blocker, "Insufficient disk space — recording blocked",
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

    if (free_bytes < kWarnFreeBytes) {
        // rec.005: soft warning — recording is still allowed but space is getting low.
        DiagnosticResult r = MakeResult(
            "rec.005", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice, DiagnosticTier::MeasuredProblem,
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
        "rec.008", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice, DiagnosticTier::Optimisation,
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
            "rec.006", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker, DiagnosticTier::Blocker,
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
        DiagnosticResult r =
            MakeResult("rec.009", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker, DiagnosticTier::Blocker,
                       "FLAC is not supported in MP4",
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
                       DiagnosticTier::Optimisation, "Opus in MP4 has limited player compatibility",
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
    if (config_.video_codec == capability::VideoCodec::Hevc && config_.container == capability::Container::WebM) {
        DiagnosticResult r =
            MakeResult("rec.010", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker, DiagnosticTier::Blocker,
                       "HEVC is not supported in WebM",
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

    if (config_.video_codec == capability::VideoCodec::H264 && config_.container == capability::Container::WebM) {
        DiagnosticResult r =
            MakeResult("rec.010", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker, DiagnosticTier::Blocker,
                       "H.264 is not supported in WebM",
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

ExclusiveEvidence RecommendationEngine::exclusiveWindowEvidence() const {
    if (!capture_window_facts_.has_value()) {
        return ExclusiveEvidence::None;
    }
    const bool present_fse = present_.has_value() && present_->mode == PresentMode::ExclusiveFullscreen;
    return ResolveExclusiveEvidence(*capture_window_facts_, capture_window_hub_, present_fse);
}

void RecommendationEngine::checkExclusiveWindowTarget(DiagnosticChecklist& checklist) const {
    const ExclusiveEvidence ev = exclusiveWindowEvidence();
    if (ev == ExclusiveEvidence::None) {
        return;
    }
    const bool proven = ev == ExclusiveEvidence::ProvenBlack;

    DiagnosticResult r = MakeResult(
        "rec.capture.exclusive_window", DiagnosticGroup::Recommendation,
        proven ? DiagnosticSeverity::Blocker : DiagnosticSeverity::Notice,
        // Honest capture problem: proven-black gates the start (Tier-1), a suspected
        // exclusive window is a measured Tier-2 problem — never an optimisation/fact.
        proven ? DiagnosticTier::Blocker : DiagnosticTier::MeasuredProblem,
        proven ? "Selected window is in exclusive fullscreen and produces no frames"
               : "Selected window may be in exclusive fullscreen",
        proven ? "The selected window is in exclusive fullscreen; window capture records a black/frozen frame."
               : "The selected window looks like exclusive fullscreen, which window capture cannot reliably record.",
        proven ? "The window capture API (WGC) produced no usable frame for the selected window — a legacy "
                 "exclusive-fullscreen application bypasses the desktop compositor, so window capture records "
                 "a black or frozen picture. Record the monitor instead (which can capture exclusive "
                 "fullscreen), or switch the game to borderless / windowed fullscreen."
               : "The selected window covers its monitor with no border and a fullscreen signal is present, "
                 "which usually means legacy exclusive fullscreen. Window capture often records a black frame "
                 "in that mode. Record the monitor instead, or switch the game to borderless.",
        proven ? "Window capture: no frames (exclusive fullscreen)" : "Window looks like exclusive fullscreen",
        "Set the game to Borderless / Windowed Fullscreen to capture the window directly.");

    FixAction fa;
    fa.id = "fix.capture.monitor_instead";
    fa.label = "Record the monitor instead";
    // Auto (executable) but NEVER one-click: retargeting changes the recording
    // scope and track structure, so the confirm's changes_summary is mandatory.
    fa.safety = FixAction::Safety::Auto;
    fa.reversible = true;
    fa.changes_summary = "Records the whole monitor that hosts this window instead of the window itself. "
                         "The recording will include everything on that monitor (other windows, notifications), "
                         "and the per-application (APP) audio row is removed — only System and Microphone audio "
                         "remain. You can switch back to window capture at any time.";
    r.fix_action = fa;

    if (proven) {
        checklist.has_blocker = true;
    } else {
        checklist.has_notice = true;
    }
    checklist.results.push_back(std::move(r));
}

void RecommendationEngine::checkExclusiveFullscreen(DiagnosticChecklist& checklist) const {
    if (!present_.has_value() || present_->mode != PresentMode::ExclusiveFullscreen) {
        return;
    }
    // Dedupe: while the selected-window card is speaking, suppress this generic
    // present-mode card so one problem raises exactly one card.
    if (exclusiveWindowEvidence() != ExclusiveEvidence::None) {
        return;
    }
    DiagnosticResult r;
    r.id = "rec.present.exclusive";
    r.group = DiagnosticGroup::Recommendation;
    r.severity = DiagnosticSeverity::Notice;
    r.tier = DiagnosticTier::MeasuredProblem;
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
        "rec.dpc.latency", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice, DiagnosticTier::MeasuredProblem,
        "High kernel DPC/ISR latency detected",
        "Kernel driver latency can cause recording stutter even when the game feels smooth.",
        "Peak DPC latency reached " + max_str + " us, attributed to " + driver +
            ". High DPC latency causes recording stutter/audio crackle even when the game itself "
            "feels smooth.",
        "Max DPC: " + max_str + " us", "Update or roll back " + driver + " (GPU/audio/network/chipset driver).");
    r.measured_value = dpc_->max_latency_us;
    r.budget_value = kDpcThresholdUs;
    r.value_unit = "us";
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

void RecommendationEngine::checkDiscardedPresents(DiagnosticChecklist& checklist) const {
    // The desktop compositor (DWM) discarded a notable share of the source's presents.
    // Discarded presents never reach capture, so the recording looks choppier than the game.
    constexpr uint32_t kMinSamples = 200;           // ignore warm-up / tiny samples
    constexpr double kDiscardRatioThreshold = 0.05; // 5% discarded sustained
    // An unattributed sample spans every process on the desktop; what it says
    // about discards is not a statement about the recorded content.
    if (!present_.has_value() || !present_->attributed || present_->present_count < kMinSamples) {
        return;
    }
    const double ratio = static_cast<double>(present_->discarded_count) / static_cast<double>(present_->present_count);
    if (ratio < kDiscardRatioThreshold) {
        return;
    }
    const std::string pct = std::to_string(static_cast<long>(ratio * 100.0 + 0.5));
    DiagnosticResult r = MakeResult(
        "rec.present.discarded", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
        DiagnosticTier::MeasuredProblem, "Compositor is discarding presents",
        "The desktop compositor dropped a notable share of the source's frames before capture.",
        "About " + pct +
            "% of the captured source's presents were discarded by the compositor (DWM). "
            "Discarded presents never reach capture, so the recording can look choppier than the game. This "
            "usually means the source presents faster than the display refresh, or an overlay forces recomposition.",
        "Discarded presents: " + pct + "%",
        "Cap the source frame rate to the display refresh, or enable V-Sync in the captured app.");
    r.measured_value = ratio * 100.0;
    r.budget_value = kDiscardRatioThreshold * 100.0;
    r.value_unit = "%";
    FixAction fa;
    fa.id = "fix.present.discarded";
    fa.label = "Reduce discarded presents";
    fa.safety = FixAction::Safety::Assisted; // app cannot change a foreign source's pacing
    fa.reversible = true;
    fa.changes_summary = "Opens guidance for capping the source frame rate / enabling V-Sync so fewer presents "
                         "are discarded by the compositor.";
    r.fix_action = fa;
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
}

void RecommendationEngine::checkPresentModeFlips(DiagnosticChecklist& checklist) const {
    // The source repeatedly switched presentation mode (composed / independent-flip / exclusive).
    // A one-off enter/exit is benign; repeated flipping causes momentary capture hitches.
    constexpr uint32_t kFlipThreshold = 5;
    if (!present_.has_value() || !present_->attributed || present_->mode_flip_count < kFlipThreshold) {
        return;
    }
    const std::string n = std::to_string(present_->mode_flip_count);
    DiagnosticResult r = MakeResult(
        "rec.present.modeflip", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
        DiagnosticTier::MeasuredProblem, "Captured source keeps changing present mode",
        "The source repeatedly switched presentation mode, which can cause capture hitches.",
        "The captured source changed presentation mode " + n +
            " times this session (e.g. flipping between "
            "composed, independent-flip and exclusive fullscreen). Frequent mode changes can momentarily stutter "
            "or drop capture. A toggling overlay, alt-tabbing, or a borderless/fullscreen toggle is the usual cause.",
        "Present-mode changes: " + n,
        "Keep the captured app in one stable presentation mode (e.g. consistent borderless fullscreen).");
    // A count of mode changes, with no budget: kFlipThreshold is the entry
    // threshold for the card, not headroom the recording is spending.
    r.measured_value = static_cast<double>(present_->mode_flip_count);
    FixAction fa;
    fa.id = "fix.present.modeflip";
    fa.label = "Stabilize present mode";
    fa.safety = FixAction::Safety::Assisted;
    fa.reversible = true;
    fa.changes_summary = "Opens guidance for keeping the captured source in a single stable presentation mode.";
    r.fix_action = fa;
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
}

void RecommendationEngine::checkDiskWriteStall(DiagnosticChecklist& checklist) const {
    // A single buffered write of the recording blocked long enough to risk backing up the
    // encoder output queue and dropping frames. Streaming Matroska only (MP4 is post-stop remux).
    constexpr double kWriteStallMs = 100.0;
    if (!live_disk_write_available_ || live_disk_peak_write_ms_ <= kWriteStallMs) {
        return;
    }
    const std::string ms = std::to_string(static_cast<long>(live_disk_peak_write_ms_));
    DiagnosticResult r = MakeResult(
        "rec.disk.writestall", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
        DiagnosticTier::MeasuredProblem, "Disk write stalls detected",
        "Writing the recording to disk stalled, which can drop frames at high bitrates.",
        "A single write of the recording to disk took up to " + ms +
            " ms. When disk writes stall longer than "
            "the encoder can buffer, the mux queue backs up and frames can be dropped. A slow or busy drive, "
            "antivirus scanning the output, or a network/USB target is the usual cause.",
        "Peak disk write: " + ms + " ms",
        "Record to a fast local drive (SSD), or exclude the output folder from real-time antivirus scanning.");
    r.measured_value = live_disk_peak_write_ms_;
    r.budget_value = kWriteStallMs;
    r.value_unit = "ms";
    FixAction fa;
    fa.id = "fix.disk.writestall";
    fa.label = "Reduce disk write stalls";
    fa.safety = FixAction::Safety::Assisted;
    fa.reversible = true;
    fa.changes_summary = "Opens guidance for choosing a faster output drive / excluding the output folder from "
                         "antivirus scanning.";
    r.fix_action = fa;
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
}

// ---------------------------------------------------------------------------
// Saved display not found — calm Display notice (stable-display-identity).
// Fires ONLY when a concretely-saved capture target could not be matched to any
// connected display. Not a blocker: recording the primary/current display still
// works; only the stored preference is missing.
// ---------------------------------------------------------------------------
void RecommendationEngine::checkUnresolvedSavedDisplay(DiagnosticChecklist& checklist) const {
    if (!saved_display_unresolved_) {
        return;
    }

    const std::string which =
        saved_display_label_.empty() ? std::string("The saved display") : ("\"" + saved_display_label_ + "\"");

    DiagnosticResult r = MakeResult(
        "display.saved.unresolved", DiagnosticGroup::Display, DiagnosticSeverity::Notice,
        DiagnosticTier::MeasuredProblem, "Saved display not found",
        "The display this preset recorded from is not currently connected, so no capture source is selected.",
        which + " could not be matched to any connected display. This happens after that monitor is unplugged, or "
                "after swapping cables between two identical monitors that report no serial number. Recording still "
                "works — choose a source to record now, and it will be remembered.",
        which + " is unavailable", "Choose a capture source to record now.");

    FixAction fa;
    fa.id = "fix.display.reselect";
    fa.label = "Choose a source";
    fa.safety = FixAction::Safety::Assisted;
    fa.reversible = true;
    fa.changes_summary = "Opens the source picker so you can pick a display or window to record.";
    r.fix_action = fa;

    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
}

// ---------------------------------------------------------------------------
// Audio device loss — calm Tier-2 measured problem (ADR 0046). Fires ONLY while
// recording and at least one audio capture source is currently degraded (endpoint
// lost, contributing honest silence). NEVER a blocker: the recording keeps running
// and the source auto-reactivates when the device returns. The verdict must stay
// "recording" — this is measured, not predicted.
// ---------------------------------------------------------------------------
void RecommendationEngine::checkAudioSourceDegraded(DiagnosticChecklist& checklist) const {
    if (!live_audio_available_ || live_audio_degraded_sources_ == 0) {
        return;
    }
    const std::string n = std::to_string(live_audio_degraded_sources_);
    const bool plural = live_audio_degraded_sources_ != 1;
    const std::string source_word = plural ? "audio sources are" : "An audio source is";
    if (live_audio_endpoint_in_use_) {
        // The endpoint is still there; another application holds it exclusively.
        // "Reconnect the device" would send the user to the wrong place.
        DiagnosticResult taken = MakeResult(
            "rec.audio.endpoint_taken", DiagnosticGroup::Audio, DiagnosticSeverity::Notice,
            DiagnosticTier::MeasuredProblem, "Another application took the audio device",
            source_word + " silent because another application holds the audio device in exclusive mode.",
            "The device is still present but refused ExoSnap when it tried to reopen it, which is what happens "
            "while another application has taken exclusive control of it. Video and every other audio source "
            "are untouched; the source resumes on its own the moment the device is released.",
            n + " audio source" + (plural ? "s" : "") + " refused as in use",
            "Close the application that has taken the device, or in Windows Sound open the device's Properties, "
            "Advanced tab, and clear \"Allow applications to take exclusive control of this device\".");
        FixAction taken_fix;
        taken_fix.id = "fix.audio.check_devices";
        taken_fix.label = "Check audio devices";
        taken_fix.safety = FixAction::Safety::Assisted;
        taken_fix.reversible = true;
        taken_fix.changes_summary = "Opens Audio settings so you can reselect the capture device.";
        taken.fix_action = taken_fix;
        checklist.has_notice = true;
        checklist.results.push_back(std::move(taken));
        return;
    }
    DiagnosticResult r = MakeResult(
        "rec.audio.degraded", DiagnosticGroup::Audio, DiagnosticSeverity::Notice, DiagnosticTier::MeasuredProblem,
        "Audio device lost — recording continues",
        source_word + " silent because the capture device dropped out. The recording keeps running.",
        n + " of " +
            std::to_string(live_audio_track_count_ == 0 ? live_audio_degraded_sources_ : live_audio_track_count_) +
            " audio track(s) lost their capture device mid-recording and are contributing honest silence. Video and "
            "every other audio source are untouched; the source reactivates automatically the moment the device "
            "returns.",
        n + " audio source" + (plural ? "s" : "") + " degraded to silence",
        "Reconnect the audio device (or check Windows Sound settings). No action is required to keep recording — the "
        "gap is filled with silence and the source recovers on its own.");
    FixAction fa;
    fa.id = "fix.audio.check_devices";
    fa.label = "Check audio devices";
    fa.safety = FixAction::Safety::Assisted; // app cannot re-plug a device for the user
    fa.reversible = true;
    fa.changes_summary = "Opens Audio settings so you can reselect or reconnect the capture device.";
    r.fix_action = fa;
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
}

// ---------------------------------------------------------------------------
// Tier-4 environment facts. Capability/environment facts run through the model as
// real Fact-tier results (never inline hard-coded UI), but on a separate producer
// so they never pollute the recommendation checklist or the verdict. Neutral and
// Expert-only. Elevation baseline is always a fact; the audio format is a fact
// once a live audio track reports its rate/channels.
// ---------------------------------------------------------------------------
std::vector<DiagnosticResult> RecommendationEngine::GenerateEnvironmentFacts() const {
    std::vector<DiagnosticResult> facts;
    // Truthful, measured elevation baseline (queried by the caller via IElevationProvider —
    // the same gate PresentMonProvider uses). Elevated unlocks the PresentMon ETW present
    // diagnostics; Standard keeps the DXGI / NVAPI baseline (judder is still measured live).
    const std::string elevation_summary = elevated_
                                              ? "Elevated — PresentMon ETW present diagnostics available"
                                              : "Standard — DXGI / NVAPI baseline · present diagnostics need elevation";
    facts.push_back(MakeResult("fact.elevation", DiagnosticGroup::ConfigSnapshot, DiagnosticSeverity::Pass,
                               DiagnosticTier::Fact, "Elevation", elevation_summary));
    if (live_audio_format_available_) {
        const std::string value =
            std::to_string(live_audio_sample_rate_) + " Hz · " + std::to_string(live_audio_channels_) + " ch";
        facts.push_back(MakeResult("fact.audio.format", DiagnosticGroup::Audio, DiagnosticSeverity::Pass,
                                   DiagnosticTier::Fact, "Audio format", value));
    }
    return facts;
}

void RecommendationEngine::checkFramePacingDuplication(DiagnosticChecklist& checklist) const {
    // Repeats are the CFR pacer's answer to a source that produced nothing new
    // for a tick. A steady share of them means the source runs below the
    // recording rate; the file plays with judder no encoder setting can fix. A
    // full stall is the stall notice's story, not this card's.
    constexpr double kDuplicateRatioThreshold = 0.25;
    constexpr uint64_t kMinEmittedFrames = 300; // five seconds at 60 fps: past warm-up
    if (!live_capture_available_ || !live_cfr_ || live_capture_starved_ || live_frames_emitted_ < kMinEmittedFrames) {
        return;
    }
    const double ratio = static_cast<double>(live_frames_duplicated_) / static_cast<double>(live_frames_emitted_);
    if (ratio < kDuplicateRatioThreshold) {
        return;
    }
    const std::string pct = std::to_string(static_cast<long>(ratio * 100.0 + 0.5));
    const std::string fps = std::to_string(static_cast<long>(live_target_fps_ + 0.5));
    DiagnosticResult r = MakeResult(
        "rec.pacing.duplication", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
        DiagnosticTier::MeasuredProblem, "Source delivers fewer frames than the recording rate",
        "About " + pct + "% of the recorded frames are repeats of the previous one.",
        "The source produced no new frame for " + pct + "% of the " + fps +
            " fps recording ticks, so the recorder repeated the last picture to keep the file at a constant rate. "
            "The recording plays with judder that no encoder setting changes; the source itself is running below "
            "the recording rate.",
        pct + "% repeated frames at " + fps + " fps",
        "Lower the recording frame rate to what the source can sustain (30 fps for a source below 60), or raise "
        "the game's frame rate. If the source is a video player or a game capped below the recording rate, this "
        "is expected.");
    r.measured_value = ratio * 100.0;
    r.budget_value = kDuplicateRatioThreshold * 100.0;
    r.value_unit = "%";
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
}

void RecommendationEngine::checkAudioClockSaturated(DiagnosticChecklist& checklist) const {
    if (!live_clock_saturated_) {
        return;
    }
    // ppm is parts per million of playback rate: 1 ppm is 3.6 ms per hour.
    const double ms_per_hour = std::abs(live_clock_ppm_) * 3.6;
    const std::string rate = std::to_string(static_cast<long>(ms_per_hour + 0.5));
    DiagnosticResult r =
        MakeResult("rec.audio.clock_saturated", DiagnosticGroup::Audio, DiagnosticSeverity::Notice,
                   DiagnosticTier::MeasuredProblem, "Audio clock drifts faster than correction can absorb",
                   "The audio device's clock runs away from the system clock by more than the recorder can compensate.",
                   "Clock correction is at its limit (about " + rate +
                       " ms per hour) and the remaining offset keeps growing, so audio and video separate over a long "
                       "recording. This is the device's clock, not the recording settings.",
                   "Clock slaving saturated at " + rate + " ms/h",
                   "Keep recordings shorter, or use a different audio device (USB and Bluetooth devices with their own "
                   "clock are the usual cause).");
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
}

void RecommendationEngine::checkCaptureAdapterMismatch(DiagnosticChecklist& checklist) const {
    constexpr uint32_t kNvidiaVendorId = 0x10DE;
    const auto& a = capture_target_adapter_;
    if (!a.known || a.vendor_id == kNvidiaVendorId || !a.nvidia_adapter_present) {
        return;
    }
    const std::string driver = a.adapter_name.empty() ? std::string("another graphics adapter") : a.adapter_name;
    DiagnosticResult r = MakeResult(
        "rec.capture.adapter_mismatch", DiagnosticGroup::Recommendation, DiagnosticSeverity::Blocker,
        DiagnosticTier::MeasuredProblem, "The captured display is not driven by the NVIDIA GPU",
        "This display is driven by " + driver + "; the NVIDIA encoder cannot record it.",
        "Capture opens on the adapter that owns the display, and the hardware encoder opens on that same "
        "device. With the display on " +
            driver +
            " the NVIDIA encoder is never reachable, and the failure that follows would read as a codec or "
            "driver problem.",
        "Display adapter: " + driver,
        "In NVIDIA Control Panel, Manage 3D settings, set the preferred graphics processor to the NVIDIA GPU, "
        "or connect the display to the NVIDIA outputs, or record a display the NVIDIA GPU drives.");
    checklist.has_blocker = true;
    checklist.results.push_back(std::move(r));
}

void RecommendationEngine::checkOutputDriveKind(DiagnosticChecklist& checklist) const {
    const char* kind = nullptr;
    switch (output_drive_kind_) {
    case DriveKind::Remote:
        kind = "a network drive";
        break;
    case DriveKind::Removable:
        kind = "a removable drive";
        break;
    case DriveKind::CdRom:
        kind = "an optical drive";
        break;
    default:
        return;
    }
    DiagnosticResult r = MakeResult(
        "rec.output.drive_kind", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
        DiagnosticTier::Optimisation, "The output folder is on " + std::string(kind),
        "Writes there can stall long enough to drop frames.",
        "The recorder writes the file as it records; a volume whose write latency spikes (network shares, USB "
        "sticks, SD cards) back-pressures the capture. This is a property of the drive, not of the settings.",
        std::string("Output volume: ") + kind, "Record to a local SSD and copy the file afterwards.");
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
}

void RecommendationEngine::checkGpuContention(DiagnosticChecklist& checklist) const {
    if (!live_gpu_contention_) {
        return;
    }
    const std::string gpu_ms = std::to_string(live_gpu_exec_p99_ms_).substr(0, 4);
    const std::string budget =
        live_target_fps_for_gpu_ > 0.0 ? std::to_string(1000.0 / live_target_fps_for_gpu_).substr(0, 4) : "the";
    DiagnosticResult r =
        MakeResult("rec.gpu.contention", DiagnosticGroup::Recommendation, DiagnosticSeverity::Notice,
                   DiagnosticTier::MeasuredProblem, "The graphics card is saturated by the captured application",
                   "The GPU finishes the recorder's frame work later than the frame budget while the recorder's own "
                   "submissions stay cheap.",
                   "Measured on the GPU with timestamp queries: the recorder's passes take " + gpu_ms +
                       " ms (p99) against " + budget +
                       " ms per frame, while submitting them costs almost nothing. The card is busy with the captured "
                       "application's work; the recorder waits behind it.",
                   "GPU frame work " + gpu_ms + " ms p99 vs " + budget + " ms budget",
                   "Cap the game's frame rate, lower its graphics settings, or lower the recording resolution.");
    r.measured_value = live_gpu_exec_p99_ms_;
    if (live_target_fps_for_gpu_ > 0.0)
        r.budget_value = 1000.0 / live_target_fps_for_gpu_;
    r.value_unit = "ms";
    checklist.has_notice = true;
    checklist.results.push_back(std::move(r));
}

std::vector<std::string> RecommendationEngine::GetAllRecommendationCodes() {
    return {"rec.001",
            "rec.002",
            "rec.003",
            "rec.004",
            "rec.005",
            "rec.006",
            "rec.007",
            "rec.008",
            "rec.009",
            "rec.010",
            "rec.color.range",
            "rec.hdr.h264",
            "rec.audio.degraded",
            "rec.audio.endpoint_taken",
            "rec.pacing.duplication",
            "rec.audio.clock_saturated",
            "rec.capture.adapter_mismatch",
            "rec.output.drive_kind",
            "rec.gpu.contention",
            "rec.capture.exclusive_window",
            "display.saved.unresolved"};
}

bool RecommendationEngine::IsLiveMeasuredCheck(std::string_view id) noexcept {
    // An allow-list rather than a deny-list: a check that is forgotten here stays
    // out of the session ledger, which understates one recording. Forgetting it in
    // a deny-list would put a static readiness condition on the record as a problem
    // the recording caused, which is a claim about something that did not happen.
    //
    // Every id below is emitted only from a live measurement: the frame-pacing,
    // GPU, disk, present-path, DPC and audio-device checks above. The Tier-2 ids
    // deliberately absent are the ones a readiness pass answers just as well --
    // rec.005 (free space), rec.pacing.smooth (a pacing-mode recommendation),
    // display.saved.unresolved, rec.capture.adapter_mismatch, and the two
    // exclusive-fullscreen checks, which describe the selected target's mode.
    static constexpr std::string_view kLiveMeasuredIds[] = {
        "rec.001",                   // present-cadence judder
        "rec.gpu.contention",        // captured application saturating the GPU
        "rec.disk.writestall",       // write stalls during the run
        "rec.dpc.latency",           // kernel DPC/ISR latency during the run
        "rec.present.discarded",     // compositor discarding presents
        "rec.present.modeflip",      // source changing present mode
        "rec.audio.degraded",        // audio device lost mid-recording
        "rec.audio.endpoint_taken",  // audio device taken mid-recording
        "rec.audio.clock_saturated", // audio clock drifting faster than correction
        "rec.pacing.duplication",    // source delivering fewer frames than the rate
    };
    for (const std::string_view live : kLiveMeasuredIds) {
        if (live == id)
            return true;
    }
    return false;
}

} // namespace exosnap::diagnostics
