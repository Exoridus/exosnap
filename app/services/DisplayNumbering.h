#pragma once

// Sequential display numbering — the ONE home of the "Display N" label.
//
// Windows' GDI device names survive plug/unplug cycles by skipping numbers: a
// two-display desktop can be \\.\DISPLAY6 and \\.\DISPLAY7. Users think in
// "Display 1" and "Display 2", so every label re-sequences the ATTACHED
// displays in EnumDisplayDevices order. The source picker and the recording
// coordinator (Record header, output filename) must agree on that numbering;
// they both resolve through here.

#include <string>
#include <unordered_map>

namespace exosnap {

// DeviceName (e.g. L"\\.\DISPLAY6") -> stable sequential 1-based index, built
// from the currently attached displays in EnumDisplayDevices order.
std::unordered_map<std::wstring, int> BuildDisplaySequenceMap();

// Turn a monitor target's raw description (the UTF-8 GDI device name,
// "\\.\DISPLAY6") into the user-facing label using the sequence map:
// "Display 2". A device not in the map (just unplugged, stale target) falls
// back to the raw trailing number ("Display 6"); a description that is not a
// GDI device name at all is returned trimmed, as-is; an empty one yields
// "Display". Pure given the map — the seam the tests pin.
std::string SequentialDisplayLabel(const std::string& raw_description,
                                   const std::unordered_map<std::wstring, int>& sequence_map);

} // namespace exosnap
