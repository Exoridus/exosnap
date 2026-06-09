#pragma once

#include <string>
#include <vector>

namespace recorder_core {

struct AudioInputDeviceInfo {
    std::string device_id;    // WASAPI endpoint ID, UTF-8
    std::string display_name; // PKEY_Device_FriendlyName, UTF-8
    bool is_default = false;
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

} // namespace recorder_core
