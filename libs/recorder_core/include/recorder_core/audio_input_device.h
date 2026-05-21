#pragma once

#include <string>
#include <vector>

namespace recorder_core {

struct AudioInputDeviceInfo {
    std::string device_id;    // WASAPI endpoint ID, UTF-8
    std::string display_name; // PKEY_Device_FriendlyName, UTF-8
    bool is_default = false;
};

// Enumerates active WASAPI capture endpoints.
// Returns an empty list on COM failure.
// Does not throw.
// No COM types in this public header.
[[nodiscard]] std::vector<AudioInputDeviceInfo> EnumerateAudioInputDevices();

} // namespace recorder_core
