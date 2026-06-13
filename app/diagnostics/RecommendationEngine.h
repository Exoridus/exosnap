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
                         bool is_profile_supported = true);

    DiagnosticChecklist Generate() const;

    static std::vector<std::string> GetAllRecommendationCodes();

  private:
    void checkRefreshRateMismatch(DiagnosticChecklist& checklist) const;
    void checkMp4CrashResilience(DiagnosticChecklist& checklist) const;
    void checkCodecAvailability(DiagnosticChecklist& checklist) const;
    void checkOutputDriveSpace(DiagnosticChecklist& checklist) const;
    void checkProfileSupport(DiagnosticChecklist& checklist) const;

    const capability::CapabilitySet& caps_;
    const capability::UserRecorderConfig& config_;
    uint32_t monitor_refresh_rate_;
    uint64_t output_drive_free_bytes_;
    bool is_profile_supported_;

    // rec.007: hard-stop blocker threshold (500 MB).
    // rec.005: soft warning threshold (2 GB).
    // Both are defined in DiskSpaceThresholds.h.
};

} // namespace exosnap::diagnostics
