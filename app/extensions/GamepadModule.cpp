#include "GamepadModule.h"

namespace exosnap {

GamepadSource::GamepadSource(SourceCreateRequest creation, GamepadControlHooks hooks)
    : creation_(std::move(creation)), hooks_(std::move(hooks)) {
}

engine::Outcome<std::monostate> GamepadSource::Apply(const SourceRequest& request) {
    if (request.token.address.source == engine::VisualSourceId{} || request.token.address.runtime.value == 0 ||
        request.token.request.value == 0 || request.configuration.value == 0)
        return engine::EngineError{.context = "Invalid gamepad source request identity"};
    if (retired_ || request.token.address != creation_.token.address ||
        request.token.request < creation_.token.request ||
        (applied_ &&
         (request.token.request < applied_->token.request || request.configuration < applied_->configuration)))
        return engine::EngineError{.code = engine::EngineErrorCode::StaleRevision,
                                   .context = "Stale gamepad source request"};
    const auto& previous_token = applied_ ? applied_->token : creation_.token;
    const auto& previous_settings = applied_ ? applied_->validated_settings : creation_.validated_settings;
    if (request.token == previous_token &&
        (request.validated_settings != previous_settings ||
         (applied_ && ((applied_->demand.consumers == 0) != (request.demand.consumers == 0)))))
        return engine::EngineError{.code = engine::EngineErrorCode::StaleRevision,
                                   .context = "Changed gamepad content or demand requires a new request token"};
    auto projected = ProjectGamepadSettings(request.validated_settings);
    if (!projected.succeeded())
        return *projected.error();
    const bool wanted = request.demand.consumers != 0;
    if (wanted && (!running_requested_ || !applied_ || request.token != applied_->token)) {
        auto settings = std::move(*projected.value());
        settings.enabled = true;
        has_work_ = true;
        hooks_.request_prepare(request.token, std::move(settings), creation_.post_status);
        running_requested_ = true;
    } else if (!wanted && running_requested_) {
        hooks_.request_stop();
        running_requested_ = false;
    }
    applied_ = request;
    return std::monostate{};
}

void GamepadSource::RequestStop() noexcept {
    if (retired_)
        return;
    retired_ = true;
    if (running_requested_)
        hooks_.request_stop();
    running_requested_ = false;
}

bool GamepadSource::IsQuiescent() const noexcept {
    // The shared owner can be busy for another adapter before this one starts.
    return !has_work_ || hooks_.is_quiescent();
}

engine::Outcome<std::monostate> RegisterGamepadModule(CapabilityRegistry& registry, GamepadControlHooks hooks,
                                                      CachedVisualOptions devices, CachedVisualOptions layouts,
                                                      CachedVisualOptions views) {
    if (!hooks.request_prepare || !hooks.request_stop || !hooks.is_quiescent)
        return engine::EngineError{.context = "Gamepad control hooks are incomplete"};
    SourceFactory factory =
        [hooks = std::move(hooks)](SourceCreateRequest request) -> engine::Outcome<std::unique_ptr<VisualSource>> {
        auto projected = ProjectGamepadSettings(request.validated_settings);
        if (!projected.succeeded())
            return *projected.error();
        return std::unique_ptr<VisualSource>(std::make_unique<GamepadSource>(std::move(request), hooks));
    };
    auto type =
        MakeGamepadVisualSourceType(std::move(factory), std::move(devices), std::move(layouts), std::move(views));
    return registry.Register({{"org.exosnap.gamepad"}, {std::move(type)}});
}

} // namespace exosnap
