#pragma once

#include "capability_set.h"
#include "user_config.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace exosnap::capability {

struct Adjustment {
    std::string field;
    std::string from;
    std::string to;
    std::string reason;
};

struct Warning {
    std::string code;
    std::string message;
};

struct InvalidReason {
    std::string field;
    std::string message;
};

struct ResolveResult {
    bool                     succeeded = false;
    UserRecorderConfig       resolved_config;
    std::vector<Adjustment>  adjustments;
    std::vector<Warning>     warnings;
    std::vector<InvalidReason> invalidity;
};

class RequestedChange {
public:
    struct ResolutionValue {
        uint32_t width  = 0;
        uint32_t height = 0;
    };

    using Value = std::variant<Container, VideoCodec, AudioCodec, ChromaSubsampling, BitDepth, ResolutionValue>;

    static RequestedChange ForContainer(Container value);
    static RequestedChange ForVideoCodec(VideoCodec value);
    static RequestedChange ForAudioCodec(AudioCodec value);
    static RequestedChange ForChroma(ChromaSubsampling value);
    static RequestedChange ForBitDepth(BitDepth value);
    static RequestedChange ForResolution(uint32_t width, uint32_t height);

    const Value& value() const noexcept;

private:
    explicit RequestedChange(Value value);

    Value value_;
};

class SettingsResolver {
public:
    explicit SettingsResolver(const CapabilitySet& caps);

    ResolveResult ResolveChange(
        const UserRecorderConfig& current,
        const RequestedChange& change) const;

    ResolveResult ValidateConfig(const UserRecorderConfig& config) const;

private:
    const CapabilitySet& caps_;
};

} // namespace exosnap::capability

