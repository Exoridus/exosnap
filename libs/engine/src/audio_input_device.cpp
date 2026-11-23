#include <array>
#include <cstddef>
#include <exosnap/engine/audio_input_device.h>
#include <exosnap/engine/audio_routing_model.h>

// clang-format off
#include <string>
#include <windows.h>

#include <mmdeviceapi.h>
#include <propidl.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <wrl/client.h>
// clang-format on

#include <exosnap/engine/util/com_apartment.h>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace exosnap::engine {
namespace {

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }

    const int required =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }

    std::string converted(static_cast<size_t>(required), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), converted.data(),
                                            required, nullptr, nullptr);
    if (written != required) {
        return {};
    }

    return converted;
}

std::string DefaultEndpointId(IMMDeviceEnumerator& enumerator, EDataFlow flow, ERole role) {
    Microsoft::WRL::ComPtr<IMMDevice> device;
    if (FAILED(enumerator.GetDefaultAudioEndpoint(flow, role, device.GetAddressOf())) || !device) {
        return {};
    }
    LPWSTR raw_id = nullptr;
    const HRESULT result = device->GetId(&raw_id);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> id(raw_id, CoTaskMemFree);
    return SUCCEEDED(result) && id ? WideToUtf8(id.get()) : std::string{};
}

// Shared helper: enumerate WASAPI endpoints for a given data flow direction.
// flow must be eCapture or eRender. Returns {} on any COM failure.
std::vector<AudioInputDeviceInfo> EnumerateAudioEndpoints(EDataFlow flow) {
    const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool should_uninitialize = init_hr == S_OK || init_hr == S_FALSE;
    const bool com_ready = SUCCEEDED(init_hr) || init_hr == RPC_E_CHANGED_MODE;
    if (!com_ready) {
        return {};
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || enumerator == nullptr) {
        if (should_uninitialize) {
            CoUninitialize();
        }
        return {};
    }

    std::array<std::string, 3> default_device_ids;
    for (size_t role = 0; role < default_device_ids.size(); ++role)
        default_device_ids[role] = DefaultEndpointId(*enumerator, flow, static_cast<ERole>(role));

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || collection == nullptr) {
        enumerator->Release();
        if (should_uninitialize) {
            CoUninitialize();
        }
        return {};
    }

    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        collection->Release();
        enumerator->Release();
        if (should_uninitialize) {
            CoUninitialize();
        }
        return {};
    }

    std::vector<AudioInputDeviceInfo> devices;
    devices.reserve(static_cast<size_t>(count));

    for (UINT index = 0; index < count; ++index) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(index, &device)) || device == nullptr) {
            continue;
        }

        LPWSTR device_id_wide = nullptr;
        IPropertyStore* properties = nullptr;
        PROPVARIANT name_value;
        PropVariantInit(&name_value);

        AudioInputDeviceInfo info;

        hr = device->GetId(&device_id_wide);
        if (SUCCEEDED(hr) && device_id_wide != nullptr) {
            info.device_id = WideToUtf8(device_id_wide);
        }

        hr = device->OpenPropertyStore(STGM_READ, &properties);
        if (SUCCEEDED(hr) && properties != nullptr) {
            hr = properties->GetValue(PKEY_Device_FriendlyName, &name_value);
            if (SUCCEEDED(hr) && name_value.vt == VT_LPWSTR && name_value.pwszVal != nullptr) {
                info.display_name = WideToUtf8(name_value.pwszVal);
            }
        }

        if (info.display_name.empty()) {
            info.display_name = info.device_id;
        }
        for (size_t role = 0; role < default_device_ids.size(); ++role)
            info.is_default_by_role[role] =
                !default_device_ids[role].empty() && info.device_id == default_device_ids[role];
        info.is_default = info.is_default_by_role[0];

        if (!info.device_id.empty()) {
            devices.push_back(std::move(info));
        }

        PropVariantClear(&name_value);
        if (properties != nullptr) {
            properties->Release();
        }
        if (device_id_wide != nullptr) {
            CoTaskMemFree(device_id_wide);
        }
        device->Release();
    }

    collection->Release();
    enumerator->Release();
    if (should_uninitialize) {
        CoUninitialize();
    }
    return devices;
}

} // namespace

std::vector<AudioInputDeviceInfo> EnumerateAudioInputDevices() {
    return EnumerateAudioEndpoints(eCapture);
}

std::vector<AudioInputDeviceInfo> EnumerateAudioOutputDevices() {
    return EnumerateAudioEndpoints(eRender);
}

std::string DefaultAudioEndpointId(AudioBindingKind kind, AudioEndpointRole role) {
    if (kind != AudioBindingKind::MicrophoneEndpoint && kind != AudioBindingKind::PlaybackEndpoint)
        return {};

    const ComApartment apartment(COINIT_MULTITHREADED);
    if (!apartment.usable())
        return {};

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(enumerator.GetAddressOf()))) ||
        !enumerator)
        return {};

    const auto flow = kind == AudioBindingKind::MicrophoneEndpoint ? eCapture : eRender;
    return DefaultEndpointId(*enumerator.Get(), flow, static_cast<ERole>(role));
}

} // namespace exosnap::engine
