#include "CapabilityRegistry.h"

#include <algorithm>
#include <set>

namespace exosnap {

engine::Outcome<std::monostate> CapabilityRegistry::Register(ModuleRegistration registration) {
    if (sealed_)
        return engine::EngineError{.context = "The visual capability registry is sealed"};
    if (!IsVisualIdentifier(registration.module.id) || registration.visual_sources.empty())
        return engine::EngineError{.context = "Invalid visual module registration"};
    if (std::any_of(modules_.begin(), modules_.end(),
                    [&](const auto& module) { return module.descriptor.id == registration.module.id; }))
        return engine::EngineError{.context = "Duplicate visual module ID"};

    std::set<std::string_view> ids;
    for (const auto& type : registration.visual_sources) {
        if (!IsVisualIdentifier(type.id) || type.label.context.empty() || type.label.source_text.empty() ||
            type.settings_version == 0 || type.instance_limit == 0 || !type.create || FindVisualSource(type.id) ||
            !ids.insert(type.id).second)
            return engine::EngineError{.context = "Invalid or duplicate visual source type"};
        auto validated = ValidateVisualSettings(type);
        if (!validated.succeeded())
            return *validated.error();
    }

    auto modules = modules_;
    auto sources = sources_;
    modules.push_back({std::move(registration.module), sources.size(), registration.visual_sources.size()});
    for (auto& type : registration.visual_sources)
        sources.push_back(std::move(type));
    modules_.swap(modules);
    sources_.swap(sources);
    return std::monostate{};
}

engine::Outcome<std::monostate> CapabilityRegistry::Seal() {
    sealed_ = true;
    return std::monostate{};
}

bool CapabilityRegistry::IsSealed() const noexcept {
    return sealed_;
}

const VisualSourceType* CapabilityRegistry::FindVisualSource(std::string_view id) const noexcept {
    const auto found =
        std::find_if(sources_.begin(), sources_.end(), [id](const auto& source) { return source.id == id; });
    return found == sources_.end() ? nullptr : &*found;
}

std::span<const VisualSourceType> CapabilityRegistry::VisualSources() const noexcept {
    return sources_;
}

} // namespace exosnap
