// tools/envctl/core/env_display_mode.h -- the vocabulary a refresh-rate
// transaction is allowed to speak.
//
// Why this file exists
// --------------------
// Windows is asymmetric about refresh rates. `ChangeDisplaySettingsExW` accepts
// dmDisplayFrequency = 60 for a mode that physically runs at 59.94 Hz, reports
// DISP_CHANGE_SUCCESSFUL, and then `EnumDisplaySettingsExW(ENUM_CURRENT_SETTINGS)`
// reports the truncated integer 59. The same happens around the 24/30/120/240
// families on many panels. A caller who asks for "60" is therefore asking for a
// value this display will never report back, and the transaction's
// write -> read -> compare rule correctly refuses and rolls back.
//
// The fix is NOT a tolerance on the read-back. Accepting "close enough" would
// hollow out the single guarantee the whole transaction model rests on. The fix
// is that the desired value must be expressible in the same vocabulary the
// read-back uses -- so a caller picks a rate from what the display actually
// ENUMERATES, never from a datasheet.
//
// Everything here is pure: it holds only what EnumDisplaySettingsEx reported, so
// the filtering rule is unit-testable on a headless runner with no display.

#pragma once

#include <string>
#include <vector>

namespace exosnap::envctl {

// One display mode, EXACTLY as Windows reported it.
//
// `refresh_hz` is dmDisplayFrequency verbatim -- an integer, already truncated
// by Windows. Nothing in this file rounds it, corrects it, or maps 59 back to a
// nominal 60: that integer IS the value space the read-back compares against.
struct DisplayModeFacts {
    unsigned long width{0};
    unsigned long height{0};
    unsigned long refresh_hz{0};
    unsigned long bits_per_pixel{0};
    // Degrees (0 / 90 / 180 / 270), matching the rendered mode fingerprint. An
    // undocumented dmDisplayOrientation value is carried through unmapped rather
    // than hidden behind a placeholder.
    unsigned long orientation_degrees{0};
};

bool operator==(const DisplayModeFacts& lhs, const DisplayModeFacts& rhs);
bool operator!=(const DisplayModeFacts& lhs, const DisplayModeFacts& rhs);

// "<w>x<h>@<hz>x<bpp>/<orientation>" -- the auditable full-mode fingerprint that
// the `mode` property and the journal already use.
std::string FormatModeFacts(const DisplayModeFacts& mode);

// True when every field EXCEPT the refresh rate matches.
bool SameGeometry(const DisplayModeFacts& lhs, const DisplayModeFacts& rhs);

// The modes a refresh-rate transaction may target on this display: the
// enumerated modes whose width/height/bpp/orientation equal `current`,
// de-duplicated and sorted by refresh rate ascending.
//
// Restricted to the current geometry on purpose. A caller changing the refresh
// rate must not be offered a resolution change, and SetRefreshHz's coupled-field
// guard would refuse one anyway -- offering it would only produce a confusing
// apply_rejected later.
//
// `current` is NOT injected. If Windows does not enumerate the mode the display
// is currently in, this list does not contain it, and that absence is a fact
// about the machine worth seeing rather than one worth papering over.
std::vector<DisplayModeFacts> RefreshRateCandidates(const DisplayModeFacts& current,
                                                    const std::vector<DisplayModeFacts>& enumerated);

} // namespace exosnap::envctl
