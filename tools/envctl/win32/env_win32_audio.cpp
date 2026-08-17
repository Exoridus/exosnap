#include "env_win32_audio.h"

#include <windows.h>

// initguid.h must precede the property-key headers so PKEY_Device_FriendlyName
// and PKEY_AudioEngine_DeviceFormat are DEFINED in this translation unit rather
// than left as unresolved externals.
#include <initguid.h>

// mmdeviceapi.h must precede functiondiscoverykeys_devpkey.h: the latter uses
// DEFINE_PROPERTYKEY without pulling in propkeydef.h itself, and relies on its
// includer having done so (mmdeviceapi.h -> propsys.h -> propkeydef.h). Each
// include sits in its own block so clang-format's sorter cannot reorder them.
#include <mmdeviceapi.h>

#include <functiondiscoverykeys_devpkey.h>

#include <audioclient.h>

#include <cstdio>
#include <map>
#include <string>

#include "env_win32_display.h" // WideToUtf8

namespace exosnap::envctl::win32 {
namespace {

// Minimal COM pointer. Deliberately not a shared utility: this tool has exactly
// one COM translation unit and a second abstraction would be more code than the
// thing it wraps.
template <typename T> class ComPtr {
  public:
    ComPtr() = default;
    ~ComPtr() {
        Reset();
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : pointer_(other.pointer_) {
        other.pointer_ = nullptr;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            pointer_ = other.pointer_;
            other.pointer_ = nullptr;
        }
        return *this;
    }

    T** Put() {
        Reset();
        return &pointer_;
    }
    void** PutVoid() {
        return reinterpret_cast<void**>(Put());
    }
    T* Get() const {
        return pointer_;
    }
    T* operator->() const {
        return pointer_;
    }
    explicit operator bool() const {
        return pointer_ != nullptr;
    }
    void Reset() {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

  private:
    T* pointer_{nullptr};
};

class ComScope {
  public:
    ComScope() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(result);
        // RPC_E_CHANGED_MODE means somebody already initialised this thread in
        // another mode; the interfaces still work, we just must not uninitialise.
        owns_ = result == S_OK;
    }
    ~ComScope() {
        if (owns_) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    bool Initialized() const {
        return initialized_;
    }

  private:
    bool initialized_{false};
    bool owns_{false};
};

std::string StateName(DWORD state) {
    switch (state) {
    case DEVICE_STATE_ACTIVE:
        return "active";
    case DEVICE_STATE_DISABLED:
        return "disabled";
    case DEVICE_STATE_NOTPRESENT:
        return "notpresent";
    case DEVICE_STATE_UNPLUGGED:
        return "unplugged";
    default:
        return "unknown";
    }
}

std::string FormatWave(const WAVEFORMATEX* format) {
    if (format == nullptr) {
        return {};
    }
    unsigned bits = format->wBitsPerSample;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (extensible->Samples.wValidBitsPerSample != 0) {
            bits = extensible->Samples.wValidBitsPerSample;
        }
    }
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%lu/%u/%u", static_cast<unsigned long>(format->nSamplesPerSec), bits,
                  static_cast<unsigned>(format->nChannels));
    return std::string(buffer);
}

std::string ReadDeviceFormat(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.Put()))) {
        return {};
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string formatted;
    if (SUCCEEDED(store->GetValue(PKEY_AudioEngine_DeviceFormat, &value)) && value.vt == VT_BLOB &&
        value.blob.cbSize >= sizeof(WAVEFORMATEX)) {
        formatted = FormatWave(reinterpret_cast<const WAVEFORMATEX*>(value.blob.pBlobData));
    }
    PropVariantClear(&value);
    return formatted;
}

std::string ReadFriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.Put()))) {
        return {};
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        name = WideToUtf8(value.pwszVal);
    }
    PropVariantClear(&value);
    return name;
}

// IAudioClient is only ACTIVATED, never Initialize()d: GetMixFormat needs no
// stream, so nothing is opened on the developer's endpoint.
std::string ReadMixFormat(IMMDevice* device) {
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.PutVoid()))) {
        return {};
    }
    WAVEFORMATEX* format = nullptr;
    if (FAILED(client->GetMixFormat(&format)) || format == nullptr) {
        return {};
    }
    const std::string formatted = FormatWave(format);
    CoTaskMemFree(format);
    return formatted;
}

std::string EndpointId(IMMDevice* device) {
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || id == nullptr) {
        return {};
    }
    const std::string result = WideToUtf8(id);
    CoTaskMemFree(id);
    return result;
}

} // namespace

std::vector<AudioEndpoint> EnumerateAudioEndpoints(std::string& error) {
    error.clear();
    std::vector<AudioEndpoint> endpoints;

    ComScope com;
    if (!com.Initialized()) {
        error = "CoInitializeEx failed";
        return endpoints;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                enumerator.PutVoid()))) {
        error = "CoCreateInstance(MMDeviceEnumerator) failed";
        return endpoints;
    }

    // The per-role defaults, read once. GetDefaultAudioEndpoint is the ONLY
    // documented way to observe them, and there is no documented way to change
    // them -- see the catalogue entry for `default-roles`.
    std::map<std::string, std::string> roles;
    const struct {
        EDataFlow flow;
        ERole role;
        const char* name;
    } role_queries[] = {
        {eRender, eConsole, "console"},
        {eRender, eMultimedia, "multimedia"},
        {eRender, eCommunications, "communications"},
        {eCapture, eConsole, "console"},
        {eCapture, eMultimedia, "multimedia"},
        {eCapture, eCommunications, "communications"},
    };
    for (const auto& query : role_queries) {
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(query.flow, query.role, device.Put())) || !device) {
            continue;
        }
        const std::string id = EndpointId(device.Get());
        if (id.empty()) {
            continue;
        }
        auto& list = roles[id];
        if (!list.empty()) {
            list += ",";
        }
        list += query.name;
    }

    const struct {
        EDataFlow flow;
        const char* kind;
    } flows[] = {{eRender, "audio-render"}, {eCapture, "audio-capture"}};

    for (const auto& flow : flows) {
        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(flow.flow, DEVICE_STATEMASK_ALL, collection.Put()))) {
            continue;
        }
        UINT count = 0;
        if (FAILED(collection->GetCount(&count))) {
            continue;
        }
        for (UINT index = 0; index < count; ++index) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(index, device.Put())) || !device) {
                continue;
            }
            AudioEndpoint endpoint;
            endpoint.kind = flow.kind;
            endpoint.endpoint_id = EndpointId(device.Get());
            if (endpoint.endpoint_id.empty()) {
                continue;
            }
            endpoint.friendly_name = ReadFriendlyName(device.Get());
            DWORD state = 0;
            endpoint.state = SUCCEEDED(device->GetState(&state)) ? StateName(state) : "unknown";
            endpoint.device_format = ReadDeviceFormat(device.Get());
            // Only an active endpoint can be activated for a mix format; a
            // disabled one reports "" rather than a fabricated value.
            if (state == DEVICE_STATE_ACTIVE) {
                endpoint.mix_format = ReadMixFormat(device.Get());
            }
            const auto role = roles.find(endpoint.endpoint_id);
            endpoint.default_roles = role != roles.end() ? role->second : "-";
            endpoints.push_back(endpoint);
        }
    }

    return endpoints;
}

} // namespace exosnap::envctl::win32
