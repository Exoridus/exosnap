#pragma once

#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"

#include <capability/capability_set.h>

#include <string>
#include <string_view>

// FixAction routing, extracted from MainWindow's if/else chain.
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
    NavigateSourcePicker,
    // Assisted fixes: open Settings and scroll to the named section.
    NavigateSettingsOutput,
    NavigateSettingsFormat,
    NavigateFramePacing,
    NavigateFrameRate,
    NavigateResolution,
    NavigateQuality,
    NavigateMicrophone,
    NavigateClockSlaving,
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

// Resolves a supported Assisted action to its existing control. Unknown ids fail closed.
[[nodiscard]] FixResult ResolveAssistedFix(std::string_view fix_id);

// Stable Settings target for a navigation outcome, or empty for a non-navigation action.
[[nodiscard]] std::string_view SettingsSectionFor(FixOutcome outcome) noexcept;

} // namespace exosnap::diagnostics
