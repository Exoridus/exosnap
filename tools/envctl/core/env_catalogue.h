// tools/envctl/core/env_catalogue.h -- the capability classification table.
//
// This is the single place where "may envctl change this?" is decided, and it is
// pure data so the decision is unit-testable on a headless CI runner with no
// display and no audio endpoint. tools/envctl/win32 builds its descriptors FROM
// this table; it never classifies inline.
//
// Reading the class column
// ------------------------
// CapabilityClass records WHO may change a property during a run, not merely
// whether we can observe it:
//
//   ENV_MUTATE_SAFE  documented, reversible setter -- envctl changes it and puts
//                    it back exactly.
//   ENV_HUMAN        observable, but the only way to change it is undocumented
//                    (IPolicyConfig, a registry write, an undocumented
//                    DISPLAYCONFIG_DEVICE_INFO_TYPE) or is UI automation of the
//                    Settings app. envctl reads it and reports it; the operator
//                    changes it. This is a policy stop, not a TODO.
//   PHYSICAL         needs somebody to touch the hardware (unplug/replug).
//   ENV_READ         nothing changes it during a run: it is enumeration-only or
//                    derived from other state.
//   UNAVAILABLE      this SDK/OS build cannot observe it at all.

#pragma once

#include <string>
#include <vector>

#include "env_types.h"

namespace exosnap::envctl {

// Device kinds an alias may bind to. Also the `kind` field of AliasBinding.
namespace device_kind {
inline constexpr const char* kDisplay = "display";
inline constexpr const char* kAudioRender = "audio-render";
inline constexpr const char* kAudioCapture = "audio-capture";
inline constexpr const char* kSystem = "system";
} // namespace device_kind

struct CatalogueEntry {
    std::string device_kind;
    std::string property;
    CapabilityClass capability{CapabilityClass::Unavailable};
    std::string value_kind;
    std::string read_mechanism;
    std::string mutate_mechanism; // the documented setter, or exactly why there is none
    std::string note;
};

// Stable order; the CLI's `describe` prints it verbatim.
const std::vector<CatalogueEntry>& WindowsCapabilityCatalogue();

// Entries for one device kind, in catalogue order.
std::vector<CatalogueEntry> CatalogueForKind(const std::string& kind);

// Instantiate the catalogue rows for one bound alias.
std::vector<PropertyDescriptor> DescriptorsForAlias(const std::string& alias, const std::string& kind);

} // namespace exosnap::envctl
