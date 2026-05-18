#pragma once

#include "resolver.h"

#include <string>
#include <vector>

namespace exosnap::capability {

struct OptionEntry {
    std::string          label;
    SupportLevel         level      = SupportLevel::Invalid;
    bool                 selectable = false;
    std::string          reason;
    std::vector<Warning> warnings;
};

class OptionQuery {
public:
    explicit OptionQuery(const CapabilitySet& caps);

    std::vector<OptionEntry> GetContainerOptions(const UserRecorderConfig& current) const;
    std::vector<OptionEntry> GetVideoCodecOptions(const UserRecorderConfig& current) const;
    std::vector<OptionEntry> GetAudioCodecOptions(const UserRecorderConfig& current) const;
    std::vector<OptionEntry> GetChromaOptions(const UserRecorderConfig& current) const;
    std::vector<OptionEntry> GetBitDepthOptions(const UserRecorderConfig& current) const;

    ResolveResult PreviewChange(
        const UserRecorderConfig& current,
        const RequestedChange& change) const;

private:
    const CapabilitySet& caps_;
};

} // namespace exosnap::capability

