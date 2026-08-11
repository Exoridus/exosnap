#pragma once

#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"

#include <capability/capability_set.h>

#include <string>
#include <string_view>

// FixAction routing (ADR 0033), extracted from MainWindow's if/else chain.
//
// A FixAction is declared by the check that raised the diagnosis; the app only
// decides what applying it MEANS. That decision is pure: given a fix id and the
// current settings, either mutate the settings or report which navigation the host
// must perform. An unknown id is reported, never silently swallowed.
//
// Confirmation stays the host's job. Auto fixes must never apply without one, but
// a confirm dialog is a frontend concern, so this dispatcher assumes the caller has
// already obtained consent.
namespace exosnap::diagnostics {

enum class FixOutcome {
    // The id is not a known fix; the caller must do nothing.
    Unknown,
    // output/video settings were mutated in place; propagate them like a user edit.
    SettingsChanged,
    // rec.capture.exclusive_window: retarget capture to the window's hosting monitor.
    RetargetToHostingMonitor,
    // Assisted fixes: open Settings and scroll to the named section.
    NavigateSettingsOutput,
    NavigateSettingsFormat,
};

struct FixResult {
    FixOutcome outcome = FixOutcome::Unknown;

    [[nodiscard]] bool handled() const noexcept {
        return outcome != FixOutcome::Unknown;
    }
};

// Applies an Auto (or capture-retarget) fix. `output` and `video` are mutated only
// when the result is FixOutcome::SettingsChanged.
[[nodiscard]] FixResult ApplyAutoFix(std::string_view fix_id, const capability::CapabilitySet& caps,
                                     OutputSettingsModel& output, VideoSettingsModel& video);

// Resolves which Settings section an Assisted fix should open. Every assisted id
// resolves to a section — output-path fixes to Output, everything else to Format —
// so the user always lands somewhere relevant.
[[nodiscard]] FixResult ResolveAssistedFix(std::string_view fix_id);

// The Settings section anchor for a navigation outcome ("settings/output" /
// "settings/format"), or an empty string for a non-navigating outcome.
[[nodiscard]] std::string_view SettingsSectionFor(FixOutcome outcome) noexcept;

} // namespace exosnap::diagnostics
