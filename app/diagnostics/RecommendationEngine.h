#pragma once

#include "DiagnosticResult.h"
#include "PresentProvider.h"

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
    RecommendationEngine(const capability::CapabilitySet& caps, const capability::UserRecorderConfig& config,
                         uint32_t monitor_refresh_rate = 0, uint64_t output_drive_free_bytes = 0,
                         bool is_profile_supported = true, std::string output_filesystem_name = {},
                         const recorder_core::RecordingDiagnosticsSnapshot* live_snapshot = nullptr,
                         const PresentSample* present = nullptr);

    DiagnosticChecklist Generate() const;

    void SetDpcLatency(DpcLatencyReading reading) {
        dpc_ = std::move(reading);
    }

    // Output-folder writability is probed by the caller (DiagnosticsPage runs the actual
    // I/O probe) so the engine stays pure; it just emits the blocker when this is false.
    void SetOutputPathWritable(bool writable) {
        output_path_writable_ = writable;
    }

    static std::vector<std::string> GetAllRecommendationCodes();

  private:
    void checkRefreshRateMismatch(DiagnosticChecklist& checklist) const;
    void checkMp4CrashResilience(DiagnosticChecklist& checklist) const;
    void checkCodecAvailability(DiagnosticChecklist& checklist) const;
    void checkRecommendedCodec(DiagnosticChecklist& checklist) const;
    void checkOutputDriveSpace(DiagnosticChecklist& checklist) const;
    void checkOutputFilesystem(DiagnosticChecklist& checklist) const;
    void checkProfileSupport(DiagnosticChecklist& checklist) const;
    void checkAudioContainerCompat(DiagnosticChecklist& checklist) const;
    void checkVideoBitDepthContainerCompat(DiagnosticChecklist& checklist) const;
    void checkExclusiveFullscreen(DiagnosticChecklist& checklist) const;
    void checkDiscardedPresents(DiagnosticChecklist& checklist) const;
    void checkPresentModeFlips(DiagnosticChecklist& checklist) const;
    void checkDpcLatency(DiagnosticChecklist& checklist) const;
    void checkDiskWriteStall(DiagnosticChecklist& checklist) const;

    const capability::CapabilitySet& caps_;
    const capability::UserRecorderConfig& config_;
    uint32_t monitor_refresh_rate_;
    uint64_t output_drive_free_bytes_;
    bool is_profile_supported_;
    std::string output_filesystem_name_; // e.g. "FAT32", "NTFS"; empty = not queried
    bool output_path_writable_ = true;   // false => emit the not-writable blocker (set by caller)

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
