#pragma once

#include <cstdint>
#include <optional>
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

// A typed action that can be offered alongside a DiagnosticResult.
// Safety meanings:
//   Auto     — config-only change, shown with confirm/preview (never silent)
//   Assisted — opens a settings section, folder, or copies a command; user does the last step
//   External — cannot be performed by the app (e.g. driver install): show version + deep-link only
struct FixAction {
    enum class Safety {
        Auto,
        Assisted,
        External,
    };

    std::string id;    // e.g. "fix.container.mkv"
    std::string label; // button text, e.g. "Switch to MKV"
    Safety safety = Safety::Auto;
    bool reversible = true;
    std::string changes_summary; // shown in a confirm dialog before applying
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
    std::optional<FixAction> fix_action;
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
