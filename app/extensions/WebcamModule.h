#pragma once

#include "BuiltinVisualSettings.h"
#include "models/WebcamSettings.h"

namespace exosnap {

[[nodiscard]] constexpr bool ShouldRunWebcamDevice(bool has_verification_source, bool webcam_enabled,
                                                   bool has_device_id, bool recording, bool preparing,
                                                   bool record_preview_active, bool settings_preview_active) noexcept {
    return !has_verification_source && webcam_enabled && has_device_id &&
           (recording || preparing || record_preview_active || settings_preview_active);
}

// Enqueue operations return before native work begins. Captured producer ownership
// and receiver-guarded completions must survive until is_quiescent reports true.
struct WebcamControlHooks {
    std::function<engine::Outcome<WebcamSettings>(const toml::table&)> project_settings;
    std::function<void(engine::SourceToken, WebcamSettings, engine::VisualPublicationWriter,
                       std::function<void(SourceStatus)>)>
        request_start;
    std::function<void(engine::SourceToken, WebcamSettings, engine::VisualPublicationWriter)> request_update;
    std::function<void()> request_stop;
    std::function<bool()> is_quiescent;
    std::function<bool()> has_verification_source;
    std::function<bool()> session_owns_device;
    std::function<void(engine::SourceToken, engine::VisualPublicationWriter, std::function<void(SourceStatus)>)>
        request_verification;
};

class WebcamSource final : public VisualSource {
  public:
    WebcamSource(SourceCreateRequest, WebcamControlHooks);
    engine::Outcome<std::monostate> Apply(const SourceRequest&) override;
    void RequestStop() noexcept override;
    bool IsQuiescent() const noexcept override;

  private:
    SourceCreateRequest creation_;
    WebcamControlHooks hooks_;
    std::optional<SourceRequest> applied_;
    std::optional<WebcamSettings> active_;
    bool running_requested_ = false;
    bool has_work_ = false;
    bool retired_ = false;
    std::optional<engine::SourceToken> verification_token_;
};

engine::Outcome<std::monostate> RegisterWebcamModule(CapabilityRegistry&, WebcamControlHooks,
                                                     CachedVisualOptions devices);

} // namespace exosnap
