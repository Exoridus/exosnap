#pragma once

#include "DiagnosticResult.h"

#include <capability/capability_set.h>
#include <capability/user_config.h>

#include <string>
#include <vector>

namespace exosnap::diagnostics {

class RecommendationEngine {
  public:
    RecommendationEngine(const capability::CapabilitySet& caps, const capability::UserRecorderConfig& config,
                         uint32_t monitor_refresh_rate = 0, uint64_t output_drive_free_bytes = 0,
                         bool is_profile_supported = true, std::string output_filesystem_name = {});

    DiagnosticChecklist Generate() const;

    static std::vector<std::string> GetAllRecommendationCodes();

  private:
    void checkRefreshRateMismatch(DiagnosticChecklist& checklist) const;
    void checkMp4CrashResilience(DiagnosticChecklist& checklist) const;
    void checkCodecAvailability(DiagnosticChecklist& checklist) const;
    void checkOutputDriveSpace(DiagnosticChecklist& checklist) const;
    void checkOutputFilesystem(DiagnosticChecklist& checklist) const;
    void checkProfileSupport(DiagnosticChecklist& checklist) const;
    void checkAudioContainerCompat(DiagnosticChecklist& checklist) const;
    void checkVideoBitDepthContainerCompat(DiagnosticChecklist& checklist) const;

    const capability::CapabilitySet& caps_;
    const capability::UserRecorderConfig& config_;
    uint32_t monitor_refresh_rate_;
    uint64_t output_drive_free_bytes_;
    bool is_profile_supported_;
    std::string output_filesystem_name_; // e.g. "FAT32", "NTFS"; empty = not queried

    // rec.007: hard-stop blocker threshold (500 MB).
    // rec.005: soft warning threshold (2 GB).
    // Both are defined in DiskSpaceThresholds.h.
    //
    // rec.008: FAT32 output volume — 4 GiB max file size warning.
};

} // namespace exosnap::diagnostics
