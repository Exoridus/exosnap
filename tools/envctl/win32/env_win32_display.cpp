#include "env_win32_display.h"

#include <algorithm>
#include <cstdio>

#include <shellscalingapi.h>

namespace exosnap::envctl::win32 {
namespace {

// GET_ADVANCED_COLOR_INFO_2 / SET_HDR_STATE arrived with the Windows 11 GA SDK.
// Everything guarded by this either uses them or reports "unavailable"; nothing
// is ever inferred from a neighbouring flag to fill the gap.
#if defined(NTDDI_WIN11_GA) && defined(NTDDI_VERSION) && (NTDDI_VERSION >= NTDDI_WIN11_GA)
#define ENVCTL_HAS_ADVANCED_COLOR_2 1
#else
#define ENVCTL_HAS_ADVANCED_COLOR_2 0
#endif

std::string FormatLastError(const char* api, LONG code) {
    char buffer[192] = {};
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08lx)", api, static_cast<unsigned long>(code));
    return std::string(buffer);
}

struct MonitorSearch {
    std::wstring gdi_name;
    HMONITOR handle{nullptr};
};

BOOL CALLBACK MatchMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto* search = reinterpret_cast<MonitorSearch*>(parameter);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) && search->gdi_name == info.szDevice) {
        search->handle = monitor;
        return FALSE;
    }
    return TRUE;
}

// DMDO_* is an index (0..3), not an angle. The mode fingerprint has always been
// written in degrees, so the mapping lives in exactly one place. An undocumented
// value is carried through unmapped rather than replaced by a placeholder: a
// number Windows really returned is more useful than "?".
unsigned long OrientationDegrees(DWORD orientation) {
    switch (orientation) {
    case DMDO_DEFAULT:
        return 0;
    case DMDO_90:
        return 90;
    case DMDO_180:
        return 180;
    case DMDO_270:
        return 270;
    default:
        return static_cast<unsigned long>(orientation);
    }
}

} // namespace

std::string WideToUtf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    return result;
}

std::vector<DisplayTarget> EnumerateDisplays(std::string& error) {
    error.clear();
    std::vector<DisplayTarget> targets;

    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    LONG code = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
    if (code != ERROR_SUCCESS) {
        error = FormatLastError("GetDisplayConfigBufferSizes", code);
        return targets;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    code = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
    if (code != ERROR_SUCCESS) {
        error = FormatLastError("QueryDisplayConfig", code);
        return targets;
    }
    paths.resize(path_count);

    for (const auto& path : paths) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target_name{};
        target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target_name.header.size = sizeof(target_name);
        target_name.header.adapterId = path.targetInfo.adapterId;
        target_name.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target_name.header) != ERROR_SUCCESS) {
            continue;
        }

        DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name{};
        source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source_name.header.size = sizeof(source_name);
        source_name.header.adapterId = path.sourceInfo.adapterId;
        source_name.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS) {
            continue;
        }

        DisplayTarget entry;
        entry.stable_id = WideToUtf8(target_name.monitorDevicePath);
        entry.friendly_name = WideToUtf8(target_name.monitorFriendlyDeviceName);
        entry.gdi_name = WideToUtf8(source_name.viewGdiDeviceName);
        entry.adapter_id = path.targetInfo.adapterId;
        entry.target_id = path.targetInfo.id;
        entry.source_id = path.sourceInfo.id;

        char session[96] = {};
        std::snprintf(session, sizeof(session), "LUID:%08lx:%08lx/TARGET:%lu",
                      static_cast<unsigned long>(entry.adapter_id.HighPart),
                      static_cast<unsigned long>(entry.adapter_id.LowPart),
                      static_cast<unsigned long>(entry.target_id));
        entry.session_id = session;

        if (entry.stable_id.empty()) {
            // Without a device path there is no stable key, and a friendly name
            // is not an acceptable substitute. Skip rather than bind wrongly.
            continue;
        }
        targets.push_back(entry);
    }
    return targets;
}

AdvancedColorInfo ReadAdvancedColor(const DisplayTarget& target) {
    AdvancedColorInfo info;

#if ENVCTL_HAS_ADVANCED_COLOR_2
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 info2{};
    info2.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
    info2.header.size = sizeof(info2);
    info2.header.adapterId = target.adapter_id;
    info2.header.id = target.target_id;
    if (DisplayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS) {
        info.ok = true;
        info.info2_available = true;
        info.hdr_supported = info2.highDynamicRangeSupported != 0;
        info.hdr_user_enabled = info2.highDynamicRangeUserEnabled != 0;
        info.advanced_color_active = info2.advancedColorActive != 0;
        switch (info2.activeColorMode) {
        case DISPLAYCONFIG_ADVANCED_COLOR_MODE_SDR:
            info.active_color_mode = "sdr";
            break;
        case DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG:
            info.active_color_mode = "wcg";
            break;
        case DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR:
            info.active_color_mode = "hdr";
            break;
        default:
            info.active_color_mode = "unavailable";
            break;
        }
        // Automatic colour management is its own user toggle. It is reported
        // from its own bit and is never derived from the HDR state.
        info.acm = info2.wideColorSupported ? (info2.wideColorUserEnabled ? "on" : "off") : "unavailable";
        return info;
    }
#endif

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO legacy{};
    legacy.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    legacy.header.size = sizeof(legacy);
    legacy.header.adapterId = target.adapter_id;
    legacy.header.id = target.target_id;
    const LONG code = DisplayConfigGetDeviceInfo(&legacy.header);
    if (code != ERROR_SUCCESS) {
        info.error = FormatLastError("DisplayConfigGetDeviceInfo(GET_ADVANCED_COLOR_INFO)", code);
        info.active_color_mode = "unavailable";
        info.acm = "unavailable";
        return info;
    }
    info.ok = true;
    info.info2_available = false;
    info.hdr_supported = legacy.advancedColorSupported != 0;
    info.hdr_user_enabled = legacy.advancedColorEnabled != 0;
    info.advanced_color_active = legacy.advancedColorEnabled != 0;
    // The legacy struct cannot separate HDR from WCG, and it has no ACM bit at
    // all. Reporting "unavailable" is the honest answer; guessing from
    // advancedColorEnabled would silently conflate three different toggles.
    info.active_color_mode = "unavailable";
    info.acm = "unavailable";
    return info;
}

bool SetHdrState(const DisplayTarget& target, bool enable, std::string& error) {
    error.clear();

#if ENVCTL_HAS_ADVANCED_COLOR_2
    DISPLAYCONFIG_SET_HDR_STATE request{};
    request.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE;
    request.header.size = sizeof(request);
    request.header.adapterId = target.adapter_id;
    request.header.id = target.target_id;
    request.enableHdr = enable ? 1u : 0u;
    const LONG code = DisplayConfigSetDeviceInfo(&request.header);
    if (code == ERROR_SUCCESS) {
        return true;
    }
    error = FormatLastError("DisplayConfigSetDeviceInfo(SET_HDR_STATE)", code);
#endif

    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE fallback{};
    fallback.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    fallback.header.size = sizeof(fallback);
    fallback.header.adapterId = target.adapter_id;
    fallback.header.id = target.target_id;
    fallback.enableAdvancedColor = enable ? 1u : 0u;
    const LONG fallback_code = DisplayConfigSetDeviceInfo(&fallback.header);
    if (fallback_code == ERROR_SUCCESS) {
        error.clear();
        return true;
    }
    if (!error.empty()) {
        error += "; ";
    }
    error += FormatLastError("DisplayConfigSetDeviceInfo(SET_ADVANCED_COLOR_STATE)", fallback_code);
    return false;
}

DisplayMode ReadCurrentMode(const DisplayTarget& target) {
    DisplayMode mode;
    mode.devmode.dmSize = sizeof(DEVMODEW);
    const std::wstring gdi_name(target.gdi_name.begin(), target.gdi_name.end());
    if (EnumDisplaySettingsExW(gdi_name.c_str(), ENUM_CURRENT_SETTINGS, &mode.devmode, 0) == 0) {
        mode.error = "EnumDisplaySettingsExW(ENUM_CURRENT_SETTINGS) failed for " + target.gdi_name;
        return mode;
    }
    mode.ok = true;
    return mode;
}

DisplayModeFacts ToModeFacts(const DEVMODEW& mode) {
    DisplayModeFacts facts;
    facts.width = static_cast<unsigned long>(mode.dmPelsWidth);
    facts.height = static_cast<unsigned long>(mode.dmPelsHeight);
    // Verbatim. Windows has already truncated 59.94 to 59 by the time it lands
    // here, and correcting it back to a nominal 60 would invent a value the
    // read-back can never produce.
    facts.refresh_hz = static_cast<unsigned long>(mode.dmDisplayFrequency);
    facts.bits_per_pixel = static_cast<unsigned long>(mode.dmBitsPerPel);
    facts.orientation_degrees = OrientationDegrees(mode.dmDisplayOrientation);
    return facts;
}

std::string FormatMode(const DEVMODEW& mode) {
    return FormatModeFacts(ToModeFacts(mode));
}

DisplayModeList EnumerateModes(const DisplayTarget& target) {
    DisplayModeList list;
    const DisplayMode current = ReadCurrentMode(target);
    if (!current.ok) {
        list.error = current.error;
        return list;
    }
    list.current = ToModeFacts(current.devmode);

    const std::wstring gdi_name(target.gdi_name.begin(), target.gdi_name.end());
    for (DWORD index = 0;; ++index) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(DEVMODEW);
        if (EnumDisplaySettingsExW(gdi_name.c_str(), index, &mode, 0) == 0) {
            break;
        }
        list.all.push_back(ToModeFacts(mode));
    }
    if (list.all.empty()) {
        list.error = "EnumDisplaySettingsExW enumerated no modes for " + target.gdi_name;
        return list;
    }

    list.candidates = RefreshRateCandidates(list.current, list.all);
    list.ok = true;
    return list;
}

bool SetRefreshHz(const DisplayTarget& target, DWORD hz, std::string& error) {
    error.clear();
    const DisplayMode current = ReadCurrentMode(target);
    if (!current.ok) {
        error = current.error;
        return false;
    }

    const std::wstring gdi_name(target.gdi_name.begin(), target.gdi_name.end());

    // Every coupled field is carried at its ORIGINAL value. Passing only
    // DM_DISPLAYFREQUENCY lets the driver pick "a" mode with that frequency,
    // which is how a refresh-rate test silently becomes a resolution change.
    DEVMODEW request = current.devmode;
    request.dmSize = sizeof(DEVMODEW);
    request.dmDisplayFrequency = hz;
    request.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS |
                       DM_DISPLAYORIENTATION | DM_POSITION;

    const LONG result = ChangeDisplaySettingsExW(gdi_name.c_str(), &request, nullptr, CDS_UPDATEREGISTRY, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        char buffer[128] = {};
        std::snprintf(buffer, sizeof(buffer), "ChangeDisplaySettingsExW returned %ld", static_cast<long>(result));
        error = buffer;
        return false;
    }

    // The coupling check. DISP_CHANGE_SUCCESSFUL does not promise that only the
    // frequency moved.
    //
    // It does not promise the frequency reached `hz` either, and that asymmetry
    // is worth stating plainly because it looks like a bug when it is met:
    // ChangeDisplaySettingsExW happily ACCEPTS a nominal dmDisplayFrequency the
    // panel never reports back. Ask a 59.94 Hz mode for 60 and this call returns
    // DISP_CHANGE_SUCCESSFUL, while EnumDisplaySettingsExW(ENUM_CURRENT_SETTINGS)
    // then reports 59 -- likewise around 24/30/120/240 on many panels. The value
    // space is therefore whatever EnumDisplaySettingsEx REPORTS, never what a
    // datasheet or a marketing number says, and a caller picks a rate from
    // `EnumerateModes` (CLI: `exosnap-envctl list-modes`).
    //
    // Deliberately NOT reconciled here with a tolerance. The transaction's
    // read-back comparison is exact, and it stays exact: "close enough" on the
    // read-back would hollow out the one guarantee the whole model rests on. A
    // mismatch is reported as a mismatch and rolled back.
    const DisplayMode after = ReadCurrentMode(target);
    if (!after.ok) {
        error = after.error;
        return false;
    }
    const bool coupled_moved = after.devmode.dmPelsWidth != current.devmode.dmPelsWidth ||
                               after.devmode.dmPelsHeight != current.devmode.dmPelsHeight ||
                               after.devmode.dmBitsPerPel != current.devmode.dmBitsPerPel ||
                               after.devmode.dmDisplayOrientation != current.devmode.dmDisplayOrientation;
    if (coupled_moved) {
        DEVMODEW revert = current.devmode;
        revert.dmSize = sizeof(DEVMODEW);
        revert.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS |
                          DM_DISPLAYORIENTATION | DM_POSITION;
        ChangeDisplaySettingsExW(gdi_name.c_str(), &revert, nullptr, CDS_UPDATEREGISTRY, nullptr);
        error = "refresh-rate change moved a coupled mode field (" + FormatMode(current.devmode) + " -> " +
                FormatMode(after.devmode) + "); the original mode was put back and the change is refused.";
        return false;
    }
    return true;
}

std::string ReadDpiScalePercent(const DisplayTarget& target, std::string& error) {
    error.clear();
    MonitorSearch search;
    search.gdi_name.assign(target.gdi_name.begin(), target.gdi_name.end());
    EnumDisplayMonitors(nullptr, nullptr, &MatchMonitor, reinterpret_cast<LPARAM>(&search));
    if (search.handle == nullptr) {
        error = "no HMONITOR matches " + target.gdi_name;
        return {};
    }
    UINT dpi_x = 0;
    UINT dpi_y = 0;
    const HRESULT result = GetDpiForMonitor(search.handle, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
    if (FAILED(result) || dpi_x == 0) {
        error = "GetDpiForMonitor failed";
        return {};
    }
    return std::to_string(static_cast<unsigned>((dpi_x * 100U + 48U) / 96U));
}

std::string ReadTopology(std::string& error) {
    const auto displays = EnumerateDisplays(error);
    if (!error.empty()) {
        return {};
    }
    std::vector<std::string> entries;
    for (const auto& display : displays) {
        const DisplayMode mode = ReadCurrentMode(display);
        if (!mode.ok) {
            continue;
        }
        char buffer[192] = {};
        std::snprintf(buffer, sizeof(buffer), "%s@%ld,%ld %s", display.gdi_name.c_str(),
                      static_cast<long>(mode.devmode.dmPosition.x), static_cast<long>(mode.devmode.dmPosition.y),
                      FormatMode(mode.devmode).c_str());
        entries.emplace_back(buffer);
    }
    std::sort(entries.begin(), entries.end());
    std::string topology;
    for (const auto& entry : entries) {
        if (!topology.empty()) {
            topology += ";";
        }
        topology += entry;
    }
    return topology;
}

} // namespace exosnap::envctl::win32
