// tools/envctl/win32/env_win32_audio.h -- read-only WASAPI endpoint inventory.
//
// Nothing in this translation unit mutates anything. The default-endpoint
// selection is reachable only through the undocumented IPolicyConfig interface,
// and the shared-mode "Default Format" drop-down has no public setter at all --
// both are ENV_HUMAN by policy, so they are read, reported, and left alone.
//
// The two format fields are deliberately separate:
//   device_format  IPropertyStore PKEY_AudioEngine_DeviceFormat -- the ENDPOINT's
//                  shared-mode format, i.e. what the "Default Format" drop-down
//                  shows.
//   mix_format     IAudioClient::GetMixFormat -- the AUDIO ENGINE's mix format.
// They routinely differ in bit depth and channel mask. Conflating them produces
// confident, wrong diagnoses about capture formats, so they never share a field.

#pragma once

#include <string>
#include <vector>

namespace exosnap::envctl::win32 {

struct AudioEndpoint {
    std::string endpoint_id;   // IMMDevice::GetId -- the matching key
    std::string friendly_name; // PKEY_Device_FriendlyName -- display only
    std::string kind;          // "audio-render" | "audio-capture"
    std::string state;         // "active" | "disabled" | "notpresent" | "unplugged" | "unknown"
    std::string device_format; // "<rate>/<bits>/<channels>" or "" when unreadable
    std::string mix_format;    // "<rate>/<bits>/<channels>" or "" when unreadable
    std::string default_roles; // "console,multimedia,communications", or "-" for none
};

// One COM pass over every endpoint in every state. `error` is non-empty only for
// a failure that prevented enumeration entirely.
std::vector<AudioEndpoint> EnumerateAudioEndpoints(std::string& error);

} // namespace exosnap::envctl::win32
