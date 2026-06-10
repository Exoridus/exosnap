#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace exosnap::diagnostics {

enum class DiagnosticSeverity {
    Pass,
    Notice,
    Blocker,
};

enum class DiagnosticGroup {
    Overview,
    OperatingSystem,
    GpuEncoder,
    Display,
    Audio,
    Storage,
    Pipeline,
    SettingsCompatibility,
    CapabilityProbe,
    ConfigSnapshot,
    Recommendation,
    Performance,
    SelfTest,
};

struct DiagnosticResult {
    std::string id;
    DiagnosticGroup group = DiagnosticGroup::Overview;
    DiagnosticSeverity severity = DiagnosticSeverity::Pass;
    std::string title;
    std::string summary;
    std::string detail;
    std::string current_value;
    std::string recommendation;
    std::string optional_fix;
    std::vector<std::string> affected_features;
    uint64_t timestamp = 0;
};

struct DiagnosticChecklist {
    std::vector<DiagnosticResult> results;
    bool has_blocker = false;
    bool has_notice = false;

    DiagnosticSeverity worst_severity() const;
};

inline DiagnosticSeverity DiagnosticChecklist::worst_severity() const {
    DiagnosticSeverity worst = DiagnosticSeverity::Pass;
    for (const auto& r : results) {
        if (static_cast<int>(r.severity) > static_cast<int>(worst))
            worst = r.severity;
    }
    return worst;
}

} // namespace exosnap::diagnostics
