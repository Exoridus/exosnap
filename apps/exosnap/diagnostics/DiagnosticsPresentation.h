#pragma once

#include "DiagnosticResult.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace exosnap::diagnostics {

inline std::string InvalidFieldDisplayName(std::string_view field) {
    if (field == "container")
        return "Container";
    if (field == "video_codec")
        return "Video codec";
    if (field == "audio_codec")
        return "Audio codec";
    if (field == "chroma")
        return "Chroma mode";
    if (field == "bit_depth")
        return "Bit depth";
    if (field == "output_width")
        return "Output width";
    if (field == "output_height")
        return "Output height";
    if (field == "frame_rate")
        return "Frame rate";
    if (field == "config")
        return "Setting combination";
    return "Configuration";
}

inline std::string InvalidFieldActionHint(std::string_view field) {
    if (field == "output_width" || field == "output_height")
        return "Adjust this value in Output settings.";
    if (field == "frame_rate")
        return "Adjust this value in Format & Encoding settings.";
    return "Adjust the selected profile in Output or Video settings.";
}

inline void StableSortBySeverityDesc(std::vector<DiagnosticResult>& results) {
    std::stable_sort(results.begin(), results.end(), [](const DiagnosticResult& a, const DiagnosticResult& b) {
        return static_cast<int>(a.severity) > static_cast<int>(b.severity);
    });
}

inline std::vector<DiagnosticResult> BuildTopIssueRecommendations(const DiagnosticChecklist& recommendations,
                                                                  bool suppress_profile_support_blocker) {
    std::vector<DiagnosticResult> ordered = recommendations.results;
    StableSortBySeverityDesc(ordered);
    ordered.erase(std::remove_if(ordered.begin(), ordered.end(),
                                 [suppress_profile_support_blocker](const DiagnosticResult& result) {
                                     if (result.severity == DiagnosticSeverity::Pass)
                                         return true;
                                     if (suppress_profile_support_blocker && result.id == "rec.006")
                                         return true;
                                     return false;
                                 }),
                  ordered.end());
    return ordered;
}

} // namespace exosnap::diagnostics
