#pragma once

#include "DiagnosticResult.h"
#include "PresentProvider.h"
#include "WindowTargetFacts.h"

#include <capability/capability_set.h>
#include <capability/user_config.h>
#include <recorder_core/pipeline_diagnostics.h>

#include <optional>
#include <string>
#include <vector>

namespace exosnap::diagnostics {

struct DpcLatencyReading {
    double max_latency_us = 0.0;
    double avg_latency_us = 0.0;
    std::string worst_driver;
    bool available = false;
};

class RecommendationEngine {
  public:
    // output_drive_free_bytes: nullopt when the volume could not be queried, in
    // which case the disk checks stay silent rather than guessing. A queried 0
    // is a full disk and raises the blocker.
    RecommendationEngine(const capability::CapabilitySet& caps, const capability::UserRecorderConfig& config,
                         uint32_t monitor_refresh_rate = 0,
                         std::optional<uint64_t> output_drive_free_bytes = std::nullopt,
                         bool is_profile_supported = true, std::string output_filesystem_name = {},
                         const recorder_core::RecordingDiagnosticsSnapshot* live_snapshot = nullptr,
                         const PresentSample* present = nullptr);

    DiagnosticChecklist Generate() const;

    // Tier-4 environment facts (elevation baseline, live audio format). Kept on a
    // separate producer so facts never mix into the recommendation checklist or the
    // verdict count — they flow through the DiagnosticResult model but render only
    // in the Expert Environment panel.
    std::vector<DiagnosticResult> GenerateEnvironmentFacts() const;

    void SetDpcLatency(DpcLatencyReading reading) {
        dpc_ = std::move(reading);
    }

    // Output-folder writability is probed by the caller (DiagnosticsPage runs the actual
    // I/O probe) so the engine stays pure; it just emits the blocker when this is false.
    void SetOutputPathWritable(bool writable) {
        output_path_writable_ = writable;
    }

    // Whether the selected capture target's display currently has Windows HDR ON
    // (resolved by the caller from the selected target's HMONITOR ->
    // DisplayHdrFacts via capability::FindDisplayByName). Feeds the H.264 + HDR10
    // pre-flight blocker: on an SDR desktop the HDR10-native path never engages,
    // so the blocker stays silent. Default false (SDR) mirrors the SetOutputPathWritable
    // pattern — the engine stays pure and only emits when the caller supplies the fact.
    void SetCaptureTargetHdrActive(bool active) {
        capture_target_hdr_active_ = active;
    }

    // Whether a concretely-saved capture target could not be resolved to any
    // connected display at restore/re-resolve time (e.g. that monitor is
    // unplugged, or identical monitors were cable-swapped). Emits a calm Display
    // notice with a "choose a source" assisted fix. label describes the saved
    // (missing) display. Default false — the engine only emits when the caller
    // supplies the fact (mirrors SetOutputPathWritable).
    void SetSavedDisplayUnresolved(bool unresolved, std::string label = {}) {
        saved_display_unresolved_ = unresolved;
        saved_display_label_ = std::move(label);
    }

    // Honest, elevation-free exclusive-fullscreen facts for the SELECTED window
    // capture target, supplied by the caller from WindowEvidenceProbe (S2a). facts
    // == nullopt when no window target is selected (a monitor target, or nothing),
    // in which case the exclusive-window check stays silent. The engine combines
    // the window shape, the measured hub evidence and its own present sample into
    // the severity ladder (Suspected -> Notice, ProvenBlack -> Blocker). Default
    // silent, mirroring the other caller-supplied facts.
    void SetCaptureWindowEvidence(std::optional<WindowTargetFacts> facts, const WindowHubEvidence& hub) {
        capture_window_facts_ = std::move(facts);
        capture_window_hub_ = hub;
    }

    static std::vector<std::string> GetAllRecommendationCodes();

  private:
    void checkRefreshRateMismatch(DiagnosticChecklist& checklist) const;
    void checkMp4CrashResilience(DiagnosticChecklist& checklist) const;
    void checkCodecAvailability(DiagnosticChecklist& checklist) const;
    void checkRecommendedCodec(DiagnosticChecklist& checklist) const;
    void checkColorRange(DiagnosticChecklist& checklist) const;
    void checkHdrH264Blocker(DiagnosticChecklist& checklist) const;
    void checkOutputDriveSpace(DiagnosticChecklist& checklist) const;
    void checkOutputFilesystem(DiagnosticChecklist& checklist) const;
    void checkProfileSupport(DiagnosticChecklist& checklist) const;
    void checkAudioContainerCompat(DiagnosticChecklist& checklist) const;
    void checkVideoBitDepthContainerCompat(DiagnosticChecklist& checklist) const;
    void checkExclusiveFullscreen(DiagnosticChecklist& checklist) const;
    void checkExclusiveWindowTarget(DiagnosticChecklist& checklist) const;
    // The combined exclusive-fullscreen verdict for the selected window (None when
    // no window target / no evidence). Shared by checkExclusiveWindowTarget and the
    // dedupe in checkExclusiveFullscreen so one problem raises exactly one card.
    ExclusiveEvidence exclusiveWindowEvidence() const;
    void checkDiscardedPresents(DiagnosticChecklist& checklist) const;
    void checkPresentModeFlips(DiagnosticChecklist& checklist) const;
    void checkDpcLatency(DiagnosticChecklist& checklist) const;
    void checkDiskWriteStall(DiagnosticChecklist& checklist) const;
    void checkUnresolvedSavedDisplay(DiagnosticChecklist& checklist) const;
    void checkAudioSourceDegraded(DiagnosticChecklist& checklist) const;

    const capability::CapabilitySet& caps_;
    const capability::UserRecorderConfig& config_;
    uint32_t monitor_refresh_rate_;
    std::optional<uint64_t> output_drive_free_bytes_; // nullopt = volume not queryable
    bool is_profile_supported_;
    std::string output_filesystem_name_;     // e.g. "FAT32", "NTFS"; empty = not queried
    bool output_path_writable_ = true;       // false => emit the not-writable blocker (set by caller)
    bool capture_target_hdr_active_ = false; // true => capture target's display has Windows HDR ON (set by caller)
    bool saved_display_unresolved_ = false;  // true => saved capture target could not be matched (set by caller)
    std::string saved_display_label_;        // friendly name / label of the saved (missing) display

    // Selected-window exclusive-fullscreen facts (S2a probe). nullopt => no window
    // target selected: the exclusive-window check stays silent.
    std::optional<WindowTargetFacts> capture_window_facts_;
    WindowHubEvidence capture_window_hub_;

    // Live present-cadence correlation (v0.8.0 / ADR 0033). Extracted from an optional live
    // RecordingDiagnosticsSnapshot; all false/neutral when no live measurement is available
    // (e.g. idle, or WGC capture which has no present timestamp).
    bool live_present_available_ = false;
    bool live_cfr_ = true;
    double live_present_jitter_ms_ = 0.0;

    // Live disk-write latency (ADR 0033 extra-checks). Extracted from the live snapshot's
    // DiskDiagnostics; available only for the streaming Matroska writer (MP4 remux is post-stop).
    bool live_disk_write_available_ = false;
    double live_disk_peak_write_ms_ = 0.0;

    // Live audio device-loss health (ADR 0046). Extracted from the live snapshot's
    // AudioDiagnostics: how many capture sources are currently degraded (endpoint
    // lost, contributing honest silence) out of the total. A calm Tier-2 measured
    // problem while recording — NEVER a blocker; the recording keeps running and the
    // source auto-reactivates when the device returns.
    bool live_audio_available_ = false;
    uint32_t live_audio_degraded_sources_ = 0;
    uint32_t live_audio_track_count_ = 0;
    // Live audio format (Tier-4 environment fact): sample rate / channels / codec.
    uint32_t live_audio_sample_rate_ = 0;
    uint32_t live_audio_channels_ = 0;
    bool live_audio_format_available_ = false;

    // Present-mode observation (v0.8.0 / ADR 0033). Available only when the present provider
    // is active (elevation + ETW session open). Empty when not available.
    std::optional<PresentSample> present_;

    // DPC/ISR latency reading. Populated via SetDpcLatency() before Generate().
    // Empty when not yet measured.
    std::optional<DpcLatencyReading> dpc_;

    // rec.007: hard-stop blocker threshold (500 MB).
    // rec.005: soft warning threshold (2 GB).
    // Both are defined in DiskSpaceThresholds.h.
    //
    // rec.008: FAT32 output volume — 4 GiB max file size warning.
};

} // namespace exosnap::diagnostics
