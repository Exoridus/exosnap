#include "env_win32_provider.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "env_catalogue.h"
#include "env_win32_system.h"

namespace exosnap::envctl::win32 {
namespace {

ReadResult Missing(const std::string& message) {
    ReadResult result;
    result.ok = false;
    result.device_present = false;
    result.error = message;
    return result;
}

ReadResult Failed(const std::string& message) {
    ReadResult result;
    result.ok = false;
    result.device_present = true;
    result.error = message;
    return result;
}

ReadResult Value(std::string value) {
    ReadResult result;
    result.ok = true;
    result.device_present = true;
    result.value = std::move(value);
    return result;
}

ApplyResult Refuse(const std::string& message) {
    ApplyResult result;
    result.accepted = false;
    result.error = message;
    return result;
}

bool ParseOnOff(const std::string& value, bool& enabled) {
    if (value == "on") {
        enabled = true;
        return true;
    }
    if (value == "off") {
        enabled = false;
        return true;
    }
    return false;
}

} // namespace

Win32EnvironmentProvider::Win32EnvironmentProvider(AliasProfile profile) : profile_(std::move(profile)) {
    Refresh();
}

void Win32EnvironmentProvider::Refresh() {
    inventory_error_.clear();
    std::string display_error;
    displays_ = EnumerateDisplays(display_error);
    std::string audio_error;
    audio_ = EnumerateAudioEndpoints(audio_error);
    if (!display_error.empty()) {
        inventory_error_ = display_error;
    }
    if (!audio_error.empty()) {
        if (!inventory_error_.empty()) {
            inventory_error_ += "; ";
        }
        inventory_error_ += audio_error;
    }
}

const DisplayTarget* Win32EnvironmentProvider::FindDisplay(const std::string& alias) const {
    const auto binding = profile_.Find(alias);
    if (!binding.has_value()) {
        return nullptr;
    }
    const DisplayTarget* match = nullptr;
    for (const auto& display : displays_) {
        if (display.stable_id != binding->stable_id) {
            continue;
        }
        if (match != nullptr) {
            // Two devices claiming one stable id: never choose.
            return nullptr;
        }
        match = &display;
    }
    return match;
}

const AudioEndpoint* Win32EnvironmentProvider::FindAudioEndpoint(const std::string& alias) const {
    const auto binding = profile_.Find(alias);
    if (!binding.has_value()) {
        return nullptr;
    }
    const AudioEndpoint* match = nullptr;
    for (const auto& endpoint : audio_) {
        if (endpoint.endpoint_id != binding->stable_id) {
            continue;
        }
        if (match != nullptr) {
            return nullptr;
        }
        match = &endpoint;
    }
    return match;
}

std::vector<PropertyDescriptor> Win32EnvironmentProvider::Describe() const {
    std::vector<PropertyDescriptor> descriptors;
    for (const auto& binding : profile_.Bindings()) {
        auto entries = DescriptorsForAlias(binding.alias, binding.kind);
        descriptors.insert(descriptors.end(), entries.begin(), entries.end());
    }
    return descriptors;
}

bool Win32EnvironmentProvider::DevicePresent(const std::string& device_alias) const {
    const auto binding = profile_.Find(device_alias);
    if (!binding.has_value()) {
        return false;
    }
    if (binding->kind == device_kind::kSystem) {
        return true;
    }
    if (binding->kind == device_kind::kDisplay) {
        return FindDisplay(device_alias) != nullptr;
    }
    return FindAudioEndpoint(device_alias) != nullptr;
}

ReadResult Win32EnvironmentProvider::Read(const PropertyId& id) const {
    const auto binding = profile_.Find(id.device_alias);
    if (!binding.has_value()) {
        return Missing(UnboundAliasInstruction(id.device_alias));
    }

    if (binding->kind == device_kind::kSystem) {
        std::string error;
        if (id.property == "apps-theme") {
            const auto value = ReadAppsTheme(error);
            return error.empty() ? Value(value) : Failed(error);
        }
        if (id.property == "system-theme") {
            const auto value = ReadSystemTheme(error);
            return error.empty() ? Value(value) : Failed(error);
        }
        return Failed("unknown system property '" + id.property + "'");
    }

    if (binding->kind == device_kind::kDisplay) {
        const DisplayTarget* display = FindDisplay(id.device_alias);
        if (display == nullptr) {
            return Missing("no display matches stable id '" + binding->stable_id + "' (or it matches more than one)");
        }
        if (id.property == "hdr" || id.property == "advanced-color-mode" || id.property == "acm") {
            const AdvancedColorInfo info = ReadAdvancedColor(*display);
            if (!info.ok) {
                return Failed(info.error);
            }
            if (id.property == "hdr") {
                if (!info.hdr_supported) {
                    return Value("unsupported");
                }
                return Value(info.hdr_user_enabled ? "on" : "off");
            }
            if (id.property == "advanced-color-mode") {
                return Value(info.active_color_mode);
            }
            return Value(info.acm);
        }
        if (id.property == "refresh-hz" || id.property == "mode") {
            const DisplayMode mode = ReadCurrentMode(*display);
            if (!mode.ok) {
                return Failed(mode.error);
            }
            if (id.property == "mode") {
                return Value(FormatMode(mode.devmode));
            }
            return Value(std::to_string(static_cast<unsigned long>(mode.devmode.dmDisplayFrequency)));
        }
        if (id.property == "dpi-scale") {
            std::string error;
            const auto value = ReadDpiScalePercent(*display, error);
            return error.empty() ? Value(value) : Failed(error);
        }
        if (id.property == "topology") {
            std::string error;
            const auto value = ReadTopology(error);
            return error.empty() ? Value(value) : Failed(error);
        }
        return Failed("unknown display property '" + id.property + "'");
    }

    const AudioEndpoint* endpoint = FindAudioEndpoint(id.device_alias);
    if (endpoint == nullptr) {
        return Missing("no audio endpoint matches stable id '" + binding->stable_id +
                       "' (or it matches more than one)");
    }
    if (id.property == "endpoint-id") {
        return Value(endpoint->endpoint_id);
    }
    if (id.property == "friendly-name") {
        return Value(endpoint->friendly_name);
    }
    if (id.property == "endpoint-state") {
        return Value(endpoint->state);
    }
    if (id.property == "device-format") {
        return endpoint->device_format.empty() ? Failed("PKEY_AudioEngine_DeviceFormat unreadable")
                                               : Value(endpoint->device_format);
    }
    if (id.property == "mix-format") {
        return endpoint->mix_format.empty() ? Failed("IAudioClient::GetMixFormat unavailable (endpoint not active)")
                                            : Value(endpoint->mix_format);
    }
    if (id.property == "default-roles") {
        return Value(endpoint->default_roles);
    }
    return Failed("unknown audio property '" + id.property + "'");
}

ApplyResult Win32EnvironmentProvider::Apply(const PropertyId& id, const std::string& value) {
    const auto binding = profile_.Find(id.device_alias);
    if (!binding.has_value()) {
        return Refuse(UnboundAliasInstruction(id.device_alias));
    }

    // The classification lives in one place. If the catalogue does not call this
    // property mutable, no code path below can write it.
    const auto descriptors = DescriptorsForAlias(binding->alias, binding->kind);
    const auto descriptor = std::find_if(descriptors.begin(), descriptors.end(),
                                         [&](const PropertyDescriptor& entry) { return entry.id == id; });
    if (descriptor == descriptors.end()) {
        return Refuse("unknown property '" + id.Key() + "'");
    }
    if (!IsMutable(descriptor->capability)) {
        return Refuse("'" + id.Key() + "' is classified " + std::string(ToKey(descriptor->capability)) +
                      "; envctl never writes it. " + descriptor->mutate_mechanism);
    }

    const DisplayTarget* display = FindDisplay(id.device_alias);
    if (display == nullptr) {
        return Refuse("no display matches stable id '" + binding->stable_id + "'");
    }

    if (id.property == "hdr") {
        bool enable = false;
        if (!ParseOnOff(value, enable)) {
            return Refuse("hdr takes 'on' or 'off', not '" + value + "'");
        }
        std::string error;
        // Copy the target: the re-enumeration below invalidates `display`.
        const DisplayTarget snapshot = *display;
        const bool accepted = SetHdrState(snapshot, enable, error);
        Refresh();
        return accepted ? ApplyResult{true, {}} : Refuse(error);
    }

    if (id.property == "refresh-hz") {
        char* end = nullptr;
        const unsigned long hz = std::strtoul(value.c_str(), &end, 10);
        if (end == value.c_str() || *end != '\0' || hz == 0) {
            return Refuse("refresh-hz takes a positive integer, not '" + value + "'");
        }
        std::string error;
        const DisplayTarget snapshot = *display;
        const bool accepted = SetRefreshHz(snapshot, static_cast<DWORD>(hz), error);
        Refresh();
        return accepted ? ApplyResult{true, {}} : Refuse(error);
    }

    return Refuse("no mutation implemented for '" + id.Key() + "'");
}

std::vector<DeviceCandidate> Win32EnvironmentProvider::Inventory() const {
    std::vector<DeviceCandidate> candidates;
    for (const auto& display : displays_) {
        candidates.push_back({device_kind::kDisplay, display.stable_id, display.friendly_name,
                              display.gdi_name + " " + display.session_id});
    }
    for (const auto& endpoint : audio_) {
        candidates.push_back({endpoint.kind, endpoint.endpoint_id, endpoint.friendly_name,
                              "state=" + endpoint.state + " deviceFormat=" + endpoint.device_format +
                                  " mixFormat=" + endpoint.mix_format + " defaultRoles=" + endpoint.default_roles});
    }
    candidates.push_back({device_kind::kSystem, "system.appearance", "Windows appearance", "light/dark"});
    return candidates;
}

} // namespace exosnap::envctl::win32
