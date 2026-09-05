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

// The four-tier honesty model (diag-model.jsx DTIER). Each diagnostic declares
// its own tier — the tier is part of the diagnosis, never inferred downstream by
// an id allowlist. Colour AND default visibility follow the tier:
//
//   Blocker (1)          coral   — always visible; gates the record start
//   MeasuredProblem (2)  amber   — always visible; only fires when actually measured
//   Optimisation (3)     mint    — bundled into ONE quiet tip chip; never a warn colour
//   Fact (4)             neutral — capability / environment fact; Expert-only
//
// The honesty rail: hiding is ONLY ever for noise (Tier 3 + 4). Tier 1 + 2 are
// always shown, in both Simple and Expert. See IsAlwaysVisible / BundlesIntoTipChip.
enum class DiagnosticTier {
    Blocker = 1,
    MeasuredProblem = 2,
    Optimisation = 3,
    Fact = 4,
};

// Honesty rule (the rail): a real problem is never hidden. Blockers and measured
// problems are always visible; optimisations and facts may be collapsed away.
[[nodiscard]] constexpr bool IsAlwaysVisible(DiagnosticTier tier) noexcept {
    return tier == DiagnosticTier::Blocker || tier == DiagnosticTier::MeasuredProblem;
}

// Tier-3 optimisations are the ONLY diagnostics that bundle into the quiet tip
// chip ("better, but it runs"). Everything else earns its own surface.
[[nodiscard]] constexpr bool BundlesIntoTipChip(DiagnosticTier tier) noexcept {
    return tier == DiagnosticTier::Optimisation;
}

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
    // The honesty tier (diag-model.jsx DTIER). Declared by each check at the
    // diagnosis site. Defaults to Fact — a bare result carries no alarm until a
    // check states otherwise.
    DiagnosticTier tier = DiagnosticTier::Fact;
    std::string title;
    std::string summary;
    std::string detail;
    std::string current_value;
    // Numeric twin of current_value for checks that compare a measurement with a
    // budget; the session ledger and the value tint read these, the card reads the
    // string. Empty for checks that measure nothing (a capability or config fact),
    // and budget_value is empty for a measurement that is a count rather than a
    // spend against headroom.
    std::optional<double> measured_value;
    std::optional<double> budget_value;
    std::string value_unit; // "ms" | "us" | "%" | "x", or empty
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
