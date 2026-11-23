#pragma once

#include <array>
#include <string>
#include <vector>

namespace exosnap::engine {

enum class AudioBindingKind;
enum class AudioEndpointRole;

struct AudioInputDeviceInfo {
    std::string device_id;    // WASAPI endpoint ID, UTF-8
    std::string display_name; // PKEY_Device_FriendlyName, UTF-8
    bool is_default = false;
    // Console, Multimedia and Communications, in that order.
    std::array<bool, 3> is_default_by_role{};
};

// Enumerates active WASAPI capture endpoints (eCapture).
// Returns an empty list on COM failure.
// Does not throw.
// No COM types in this public header.
[[nodiscard]] std::vector<AudioInputDeviceInfo> EnumerateAudioInputDevices();

// Enumerates active WASAPI render endpoints (eRender).
// AudioInputDeviceInfo is reused: device_id, display_name, is_default fields
// carry the same semantics as for capture devices.
// Returns an empty list on COM failure.
// Does not throw.
// No COM types in this public header.
[[nodiscard]] std::vector<AudioInputDeviceInfo> EnumerateAudioOutputDevices();

// Returns the current role default's UTF-8 endpoint ID, or an empty string if unavailable.
[[nodiscard]] std::string DefaultAudioEndpointId(AudioBindingKind kind, AudioEndpointRole role);

} // namespace exosnap::engine
