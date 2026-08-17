#include "env_catalogue.h"

namespace exosnap::envctl {
namespace {

const std::vector<CatalogueEntry>& Table() {
    static const std::vector<CatalogueEntry> table = {
        // ---------------------------------------------------------------- display
        {device_kind::kDisplay, "hdr", CapabilityClass::MutateSafe, "onoff",
         "QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS) + DisplayConfigGetDeviceInfo("
         "DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2), falling back to "
         "DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO on older SDK/OS",
         "DisplayConfigSetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE), falling back to "
         "DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE",
         "Public SDK enumerants in wingdi.h with a documented structure; reversible and verified by re-reading "
         "the same path. Value is the user-enabled HDR toggle, not whether HDR is currently ACTIVE."},

        {device_kind::kDisplay, "advanced-color-mode", CapabilityClass::Read, "color-mode",
         "DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2::activeColorMode (SDR / WCG / HDR)", "derived state; no setter",
         "Reported separately from `hdr` and NEVER inferred from it: WCG is an advanced colour mode that is not "
         "HDR, and a panel can be HDR-enabled but not HDR-active."},

        {device_kind::kDisplay, "acm", CapabilityClass::Human, "onoff",
         "DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2::wideColorUserEnabled (UNAVAILABLE when the SDK/OS predates "
         "GET_ADVANCED_COLOR_INFO_2)",
         "none documented; the Settings toggle has no public API",
         "Automatic colour management. Read-only by policy. Never inferred from the HDR state -- they are "
         "independent user toggles that happen to share one Settings page."},

        {device_kind::kDisplay, "refresh-hz", CapabilityClass::MutateSafe, "hz",
         "EnumDisplaySettingsExW(ENUM_CURRENT_SETTINGS) -- the WHOLE DEVMODE is snapshotted, not just "
         "dmDisplayFrequency",
         "ChangeDisplaySettingsExW(CDS_UPDATEREGISTRY) with every coupled mode field held at its original value",
         "Resolution, colour depth and orientation must not move as a side effect: the apply carries the original "
         "width/height/bpp/orientation and the read-back confirms all of them, not only the frequency."},

        {device_kind::kDisplay, "mode", CapabilityClass::Read, "mode",
         "EnumDisplaySettingsExW(ENUM_CURRENT_SETTINGS) rendered as <w>x<h>@<hz>x<bpp>/<orientation>",
         "not mutated directly; see `refresh-hz`",
         "The full-mode fingerprint that makes a refresh-rate change auditable."},

        {device_kind::kDisplay, "dpi-scale", CapabilityClass::Human, "percent",
         "GetDpiForMonitor(MDT_EFFECTIVE_DPI) / GetScaleFactorForMonitor",
         "none documented; the per-monitor DPI setter is an undocumented DISPLAYCONFIG_DEVICE_INFO_TYPE",
         "Read-only by policy. An undocumented device-info type is exactly the class of mechanism this tool "
         "refuses to depend on."},

        {device_kind::kDisplay, "topology", CapabilityClass::Human, "text",
         "QueryDisplayConfig path/mode array rendered as a stable topology string",
         "SetDisplayConfig exists, but rearranging the desktop is the operator's decision",
         "Read-only by policy: moving somebody's monitors around is not a side effect a verification run may have."},

        // ----------------------------------------------------------- audio (both)
        {device_kind::kAudioRender, "endpoint-id", CapabilityClass::Read, "text",
         "IMMDeviceEnumerator::EnumAudioEndpoints + IMMDevice::GetId", "enumeration only",
         "The stable matching key for an audio alias. Friendly names are display-only."},

        {device_kind::kAudioRender, "friendly-name", CapabilityClass::Read, "text",
         "IPropertyStore PKEY_Device_FriendlyName", "enumeration only",
         "Display only. Two identical headsets produce two identical names."},

        {device_kind::kAudioRender, "endpoint-state", CapabilityClass::Physical, "endpoint-state",
         "IMMDevice::GetState (active / disabled / notpresent / unplugged)",
         "no API: unplug/replug and enable/disable are physical or Control Panel actions",
         "PHYSICAL. The orchestrator observes the transition; it never causes it."},

        {device_kind::kAudioRender, "device-format", CapabilityClass::Human, "wave-format",
         "IPropertyStore PKEY_AudioEngine_DeviceFormat rendered as <rate>/<bits>/<channels>",
         "no documented general setter; the Settings 'Default Format' drop-down has no public API",
         "The SHARED-MODE endpoint format. Reported separately from `mix-format` and never conflated with it."},

        {device_kind::kAudioRender, "mix-format", CapabilityClass::Read, "wave-format",
         "IAudioClient::GetMixFormat rendered as <rate>/<bits>/<channels>", "derived from the engine; no setter",
         "The AUDIO ENGINE mix format. A different thing from PKEY_AudioEngine_DeviceFormat -- it can differ in "
         "bit depth and in channel mask, and treating one as the other has produced wrong capture-format "
         "diagnoses before."},

        {device_kind::kAudioRender, "default-roles", CapabilityClass::Human, "roles",
         "IMMDeviceEnumerator::GetDefaultAudioEndpoint for eConsole/eMultimedia/eCommunications, compared against "
         "this endpoint's id",
         "only the undocumented IPolicyConfig COM interface",
         "Read-only by policy. IPolicyConfig is undocumented and is explicitly out of scope; the operator selects "
         "the default endpoint."},

        {device_kind::kAudioCapture, "endpoint-id", CapabilityClass::Read, "text",
         "IMMDeviceEnumerator::EnumAudioEndpoints + IMMDevice::GetId", "enumeration only",
         "The stable matching key for an audio alias."},

        {device_kind::kAudioCapture, "friendly-name", CapabilityClass::Read, "text",
         "IPropertyStore PKEY_Device_FriendlyName", "enumeration only", "Display only."},

        {device_kind::kAudioCapture, "endpoint-state", CapabilityClass::Physical, "endpoint-state",
         "IMMDevice::GetState (active / disabled / notpresent / unplugged)",
         "no API: unplug/replug and enable/disable are physical or Control Panel actions", "PHYSICAL."},

        {device_kind::kAudioCapture, "device-format", CapabilityClass::Human, "wave-format",
         "IPropertyStore PKEY_AudioEngine_DeviceFormat rendered as <rate>/<bits>/<channels>",
         "no documented general setter", "The SHARED-MODE endpoint format; reported separately from `mix-format`."},

        {device_kind::kAudioCapture, "mix-format", CapabilityClass::Read, "wave-format",
         "IAudioClient::GetMixFormat rendered as <rate>/<bits>/<channels>", "derived from the engine; no setter",
         "The AUDIO ENGINE mix format -- a different thing from the endpoint device format."},

        {device_kind::kAudioCapture, "default-roles", CapabilityClass::Human, "roles",
         "IMMDeviceEnumerator::GetDefaultAudioEndpoint for eConsole/eMultimedia/eCommunications",
         "only the undocumented IPolicyConfig COM interface", "Read-only by policy."},

        // ---------------------------------------------------------------- system
        {device_kind::kSystem, "apps-theme", CapabilityClass::Human, "light-dark",
         "Windows::UI::ViewManagement::UISettings when reachable, otherwise the documented Personalize location "
         "AppsUseLightTheme",
         "registry write only; no public setter",
         "Read-only by policy. A registry write is explicitly excluded as an implemented mutation."},

        {device_kind::kSystem, "system-theme", CapabilityClass::Human, "light-dark",
         "the documented Personalize location SystemUsesLightTheme", "registry write only; no public setter",
         "Read-only by policy."},
    };
    return table;
}

} // namespace

const std::vector<CatalogueEntry>& WindowsCapabilityCatalogue() {
    return Table();
}

std::vector<CatalogueEntry> CatalogueForKind(const std::string& kind) {
    std::vector<CatalogueEntry> entries;
    for (const auto& entry : Table()) {
        if (entry.device_kind == kind) {
            entries.push_back(entry);
        }
    }
    return entries;
}

std::vector<PropertyDescriptor> DescriptorsForAlias(const std::string& alias, const std::string& kind) {
    std::vector<PropertyDescriptor> descriptors;
    for (const auto& entry : CatalogueForKind(kind)) {
        PropertyDescriptor descriptor;
        descriptor.id = PropertyId{alias, entry.property};
        descriptor.capability = entry.capability;
        descriptor.kind = entry.value_kind;
        descriptor.read_mechanism = entry.read_mechanism;
        descriptor.mutate_mechanism = entry.mutate_mechanism;
        descriptor.note = entry.note;
        descriptors.push_back(descriptor);
    }
    return descriptors;
}

} // namespace exosnap::envctl
