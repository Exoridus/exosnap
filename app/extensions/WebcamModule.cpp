#include "WebcamModule.h"

namespace exosnap {
namespace {

bool HasDemand(const SourceRequest& request, VisualConsumer consumer) {
    return (request.demand.consumers & static_cast<uint8_t>(consumer)) != 0;
}

bool SameMode(const WebcamSettings& a, const WebcamSettings& b) {
    return a.device_id == b.device_id && a.width == b.width && a.height == b.height && a.fps == b.fps;
}

} // namespace

WebcamSource::WebcamSource(SourceCreateRequest creation, WebcamControlHooks hooks)
    : creation_(std::move(creation)), hooks_(std::move(hooks)) {
}

engine::Outcome<std::monostate> WebcamSource::Apply(const SourceRequest& request) {
    if (retired_ || request.token.address != creation_.token.address ||
        request.token.request < creation_.token.request ||
        (applied_ &&
         (request.token.request < applied_->token.request || request.configuration < applied_->configuration)))
        return engine::EngineError{.code = engine::EngineErrorCode::StaleRevision,
                                   .context = "Stale webcam source request"};
    if (applied_ && request.token == applied_->token &&
        (request.validated_settings != applied_->validated_settings ||
         (applied_->demand.consumers == 0 && request.demand.consumers != 0)))
        return engine::EngineError{.code = engine::EngineErrorCode::StaleRevision,
                                   .context = "Changed webcam content requires a new request token"};
    auto projected = hooks_.project_settings(request.validated_settings);
    if (!projected.succeeded())
        return *projected.error();
    WebcamSettings settings = std::move(*projected.value());
    settings.enabled = true;
    const bool session_owns_device = hooks_.session_owns_device();
    if (active_ && session_owns_device) {
        settings.device_id = active_->device_id;
        settings.width = active_->width;
        settings.height = active_->height;
        settings.fps = active_->fps;
    }
    const bool verification = hooks_.has_verification_source();
    if (verification && request.demand.consumers != 0) {
        if (running_requested_)
            hooks_.request_stop();
        running_requested_ = false;
        if (verification_token_ != request.token) {
            verification_token_ = request.token;
            hooks_.request_verification(request.token, creation_.publication, creation_.post_status);
        }
        applied_ = request;
        return std::monostate{};
    }
    if (request.demand.consumers != 0 && active_ && applied_ && request.token == applied_->token &&
        !SameMode(settings, *active_))
        return engine::EngineError{.code = engine::EngineErrorCode::StaleRevision,
                                   .context = "Changed effective camera mode requires a new request token"};
    const bool wanted = ShouldRunWebcamDevice(
        verification, settings.enabled, !settings.device_id.empty(), HasDemand(request, VisualConsumer::Recording),
        false, HasDemand(request, VisualConsumer::RecordPreview), HasDemand(request, VisualConsumer::SettingsPreview));
    if (!wanted) {
        if (running_requested_)
            hooks_.request_stop();
        running_requested_ = false;
        if (!session_owns_device)
            active_.reset();
    } else if (!running_requested_ || !SameMode(settings, *active_)) {
        has_work_ = true;
        hooks_.request_start(request.token, settings, creation_.publication, creation_.post_status);
        running_requested_ = true;
        active_ = settings;
    } else if (!applied_ || request.token != applied_->token || settings.controls != active_->controls) {
        hooks_.request_update(request.token, settings, creation_.publication);
        active_ = settings;
    }
    applied_ = request;
    return std::monostate{};
}

void WebcamSource::RequestStop() noexcept {
    if (retired_)
        return;
    retired_ = true;
    if (running_requested_)
        hooks_.request_stop();
    running_requested_ = false;
}

bool WebcamSource::IsQuiescent() const noexcept {
    return !has_work_ || hooks_.is_quiescent();
}

engine::Outcome<std::monostate> RegisterWebcamModule(CapabilityRegistry& registry, WebcamControlHooks hooks,
                                                     CachedVisualOptions devices) {
    if (!hooks.project_settings || !hooks.request_start || !hooks.request_update || !hooks.request_stop ||
        !hooks.is_quiescent || !hooks.has_verification_source || !hooks.session_owns_device ||
        !hooks.request_verification)
        return engine::EngineError{.context = "Webcam control hooks are incomplete"};
    SourceFactory factory =
        [hooks = std::move(hooks)](SourceCreateRequest request) -> engine::Outcome<std::unique_ptr<VisualSource>> {
        return std::unique_ptr<VisualSource>(std::make_unique<WebcamSource>(std::move(request), hooks));
    };
    auto type = MakeWebcamVisualSourceType(std::move(factory), std::move(devices));
    return registry.Register({{"org.exosnap.webcam"}, {std::move(type)}});
}

} // namespace exosnap
