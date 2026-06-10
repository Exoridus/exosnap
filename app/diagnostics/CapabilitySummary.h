#pragma once

#include <capability/capability_set.h>
#include <capability/config_types.h>
#include <capability/runtime_snapshot.h>

#include <string>
#include <vector>

namespace exosnap::diagnostics {

struct CapabilityEntry {
    std::string label;
    std::string value;
    std::string status;
    bool available = false;
};

struct CapabilitySummary {
    std::vector<CapabilityEntry> entries;

    static CapabilitySummary FromCapabilitySet(const capability::CapabilitySet& caps);
};

std::string SupportLevelString(capability::SupportLevel level);
std::string VideoCodecDisplayName(capability::VideoCodec v);
std::string AudioCodecDisplayName(capability::AudioCodec a);
std::string ContainerDisplayName(capability::Container c);

} // namespace exosnap::diagnostics
