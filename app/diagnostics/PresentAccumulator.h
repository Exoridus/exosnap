#pragma once

#include "PresentProvider.h"

#include <cstdint>

namespace exosnap::diagnostics {

// Pure, reader-side accumulator for the session's present aggregates (ADR 0033
// extra-checks): total presents, compositor-discarded presents, and classified
// present-mode transitions. Held by PresentMonEtwSession and Reset() at every
// attribution boundary (recording start/stop, or an idle capture-target change)
// so the statistics describe ONLY the currently-targeted source — never carrying
// a prior recording's or the idle desktop's totals across the boundary.
//
// Extracted as a pure struct (no Win32/ETW dependency) so the per-recording reset
// semantics are unit-testable without a live ETW session.
struct PresentAccumulator {
    uint64_t present_count = 0;   // total matched presents observed since the last Reset()
    uint64_t discarded_count = 0; // presents the compositor discarded (FinalState == Discarded)
    uint64_t mode_flip_count = 0; // classified present-mode transitions (instability proxy)
    PresentMode last_mode = PresentMode::Unknown;

    // Fold one classified present into the totals. A flip is counted only on a real
    // change of the CLASSIFIED mode, so a sub-variant change (e.g. Composed_Flip ->
    // Composed_Copy that both classify as Composed) does not register as a flip.
    void Observe(PresentMode classified_mode, bool discarded) {
        ++present_count;
        if (discarded)
            ++discarded_count;
        if (last_mode != PresentMode::Unknown && classified_mode != PresentMode::Unknown &&
            classified_mode != last_mode)
            ++mode_flip_count;
        if (classified_mode != PresentMode::Unknown)
            last_mode = classified_mode;
    }

    // Return to the empty state at an attribution boundary so the next recording's
    // statistics start from zero.
    void Reset() {
        *this = PresentAccumulator{};
    }
};

} // namespace exosnap::diagnostics
