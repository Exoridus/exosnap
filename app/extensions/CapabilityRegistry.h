#pragma once

#include "VisualSettings.h"
#include "VisualSource.h"

#include <exosnap/engine/visual_scene_runtime.h>

#include <span>

namespace exosnap {

struct ModuleDescriptor {
    std::string id;
};

struct SourceCreateRequest {
    engine::SourceToken token;
    toml::table validated_settings;
    engine::VisualPublicationWriter publication;
    // Queue to a live receiver on the control thread and recheck the token at dispatch.
    std::function<void(SourceStatus)> post_status;
};

using SourceFactory = std::function<engine::Outcome<std::unique_ptr<VisualSource>>(SourceCreateRequest)>;

struct VisualSourceType {
    std::string id;
    LocalizedText label;
    uint32_t settings_version = 1;
    uint32_t instance_limit = 1;
    SettingsDefinition settings;
    std::vector<ActionDescriptor> actions;
    std::vector<OptionsRegistration> options;
    SourceFactory create;
};

struct ModuleRegistration {
    ModuleDescriptor module;
    std::vector<VisualSourceType> visual_sources;
};

class CapabilityRegistry final {
  public:
    engine::Outcome<std::monostate> Register(ModuleRegistration);
    engine::Outcome<std::monostate> Seal();
    const VisualSourceType* FindVisualSource(std::string_view) const noexcept;
    std::span<const VisualSourceType> VisualSources() const noexcept;

  private:
    struct RegisteredModule {
        ModuleDescriptor descriptor;
        size_t first_source = 0;
        size_t source_count = 0;
    };
    std::vector<RegisteredModule> modules_;
    std::vector<VisualSourceType> sources_;
    bool sealed_ = false;
};

} // namespace exosnap
