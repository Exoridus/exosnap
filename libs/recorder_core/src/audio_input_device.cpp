#include <recorder_core/audio_input_device.h>

// clang-format off
#include <windows.h>

#include <mmdeviceapi.h>
#include <propidl.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
// clang-format on

#include <string_view>
#include <utility>
#include <vector>

namespace recorder_core {
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

    std::string default_device_id;
    IMMDevice* default_device = nullptr;
    LPWSTR default_id_wide = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &default_device);
    if (SUCCEEDED(hr) && default_device != nullptr) {
        hr = default_device->GetId(&default_id_wide);
        if (SUCCEEDED(hr) && default_id_wide != nullptr) {
            default_device_id = WideToUtf8(default_id_wide);
        }
    }
    if (default_id_wide != nullptr) {
        CoTaskMemFree(default_id_wide);
    }
    if (default_device != nullptr) {
        default_device->Release();
    }

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
        info.is_default = !default_device_id.empty() && info.device_id == default_device_id;

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

} // namespace recorder_core
