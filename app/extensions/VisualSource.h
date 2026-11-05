#pragma once

#include <exosnap/engine/visual_scene.h>
#include <toml++/toml.h>

namespace exosnap {

enum class SourcePhase { Dormant, Preparing, Ready, Waiting, Failed, Stopping };
enum class VisualConsumer : uint8_t { RecordPreview = 1, SettingsPreview = 2, Recording = 4 };

struct VisualDemand {
    uint8_t consumers = 0;
};

struct SourceRequest {
    engine::SourceToken token;
    engine::ConfigRevision configuration;
    VisualDemand demand;
    toml::table validated_settings;
};

struct SourceStatus {
    engine::SourceToken token;
    SourcePhase phase = SourcePhase::Dormant;
    std::optional<engine::EngineError> reason;
};

class VisualSource {
  public:
    virtual ~VisualSource() = default;
    virtual engine::Outcome<std::monostate> Apply(const SourceRequest&) = 0;
    // Only same-identity live changes may retain pixels stamped with the previous request.
    virtual bool RetainsAccepted(const toml::table&) const {
        return false;
    }
    virtual void RequestStop() noexcept = 0;
    virtual bool IsQuiescent() const noexcept = 0;
};

} // namespace exosnap
