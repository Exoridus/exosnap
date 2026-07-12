#include "services/DisplayIdentityEnumerator.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

#include <QGuiApplication>
#include <QScreen>
#include <QString>

#include "diagnostics/AppLog.h"

namespace exosnap {

namespace {

std::string WideToUtf8(const wchar_t* w) {
    if (w == nullptr || w[0] == L'\0') {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

// Decode an EDID manufacturer id (as reported by DISPLAYCONFIG_TARGET_DEVICE_NAME)
// into its 3-letter PNP vendor code. The value is stored big-endian relative to
// our little-endian USHORT, so swap first, then peel three 5-bit letters.
// (The decode only needs to be CONSISTENT between save and restore — the same
// function runs on both sides — so an off-by-one vendor letter still matches.)
std::string DecodeEdidVendor(USHORT edid_manufacture_id) {
    if (edid_manufacture_id == 0) {
        return {};
    }
    const USHORT swapped = static_cast<USHORT>((edid_manufacture_id >> 8) | (edid_manufacture_id << 8));
    char v[4] = {0, 0, 0, 0};
    v[0] = static_cast<char>('A' + ((swapped >> 10) & 0x1F) - 1);
    v[1] = static_cast<char>('A' + ((swapped >> 5) & 0x1F) - 1);
    v[2] = static_cast<char>('A' + (swapped & 0x1F) - 1);
    for (int i = 0; i < 3; ++i) {
        if (v[i] < 'A' || v[i] > 'Z') {
            return {}; // not a clean PNP id
        }
    }
    return std::string(v, 3);
}

struct MonitorEntry {
    HMONITOR hmonitor = nullptr;
    std::wstring gdi_name; // szDevice, e.g. "\\.\DISPLAY1"
    PhysicalRect rc_physical;
};

BOOL CALLBACK CollectMonitorProc(HMONITOR hmon, HDC /*hdc*/, LPRECT /*rc*/, LPARAM userdata) {
    auto* list = reinterpret_cast<std::vector<MonitorEntry>*>(userdata);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmon, &mi) == FALSE) {
        return TRUE;
    }
    MonitorEntry e;
    e.hmonitor = hmon;
    e.gdi_name = mi.szDevice;
    e.rc_physical = PhysicalRect{mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom};
    list->push_back(std::move(e));
    return TRUE;
}

// One display's DisplayConfig target facts, keyed by the source GDI device name.
struct TargetFacts {
    std::string device_path;
    std::string edid_vendor;
    uint32_t edid_product = 0;
    std::string friendly_name;
};

// Build a map of GDI device name -> target facts via DisplayConfig. Mirrors the
// QuerySdrWhiteLevelNits loop (dxgi_od_capture_src.cpp): join each active path's
// source (viewGdiDeviceName) to its target (monitorDevicePath + EDID + friendly).
std::unordered_map<std::wstring, TargetFacts> QueryTargetFactsByGdiName() {
    std::unordered_map<std::wstring, TargetFacts> out;

    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return out;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        return out;
    }
    paths.resize(path_count);

    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
            continue;
        }

        TargetFacts facts;
        facts.device_path = WideToUtf8(target.monitorDevicePath);
        facts.edid_vendor = DecodeEdidVendor(target.edidManufactureId);
        facts.edid_product = target.edidProductCodeId;
        facts.friendly_name = WideToUtf8(target.monitorFriendlyDeviceName);
        out.emplace(std::wstring(source.viewGdiDeviceName), std::move(facts));
    }
    return out;
}

// Best-effort EDID serial via Qt, joined by GDI device name. Historically Qt's
// Windows QPA does not reliably populate this; empty is expected on much
// hardware, which is exactly why the matcher never guesses among serial-less
// twins. Returns empty when no QGuiApplication exists.
std::unordered_map<std::wstring, std::string> QuerySerialsByGdiName() {
    std::unordered_map<std::wstring, std::string> out;
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr) {
        return out;
    }
    for (QScreen* screen : QGuiApplication::screens()) {
        const std::string serial = screen->serialNumber().toStdString();
        if (!serial.empty()) {
            out.emplace(screen->name().toStdWString(), serial);
        }
    }
    return out;
}

} // namespace

std::vector<EnumeratedDisplayIdentity> EnumerateDisplayIdentities() {
    std::vector<MonitorEntry> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &CollectMonitorProc, reinterpret_cast<LPARAM>(&monitors));

    const std::unordered_map<std::wstring, TargetFacts> facts = QueryTargetFactsByGdiName();
    const std::unordered_map<std::wstring, std::string> serials = QuerySerialsByGdiName();

    std::vector<EnumeratedDisplayIdentity> out;
    out.reserve(monitors.size());

    int seq = 0;
    for (const MonitorEntry& mon : monitors) {
        EnumeratedDisplayIdentity e;
        e.hmonitor = reinterpret_cast<uintptr_t>(mon.hmonitor);
        e.rc_monitor_physical = mon.rc_physical;
        e.id.gdi_name = WideToUtf8(mon.gdi_name.c_str());
        e.id.seq_hint = ++seq;

        const auto it = facts.find(mon.gdi_name);
        if (it != facts.end()) {
            e.id.device_path = it->second.device_path;
            e.id.edid_vendor = it->second.edid_vendor;
            e.id.edid_product = it->second.edid_product;
            e.id.friendly_name = it->second.friendly_name;
        }

        const auto sit = serials.find(mon.gdi_name);
        if (sit != serials.end()) {
            e.id.serial = sit->second;
        }

        out.push_back(std::move(e));
    }

    // Serial-gate log: once per process, record whether real hardware populates
    // the EDID serial (matcher stage 2). Lets a live verify decide whether the
    // panel-follow-across-ports stage carries real data or should be dropped.
    static std::atomic<bool> logged{false};
    bool expected = false;
    if (logged.compare_exchange_strong(expected, true)) {
        for (const auto& e : out) {
            diagnostics::AppLog::info(
                QStringLiteral("DisplayIdentity"),
                QStringLiteral("enumerate gdi=%1 vendor=%2 product=%3 serial=%4 path=%5")
                    .arg(QString::fromStdString(e.id.gdi_name))
                    .arg(QString::fromStdString(e.id.edid_vendor))
                    .arg(e.id.edid_product)
                    .arg(e.id.serial.empty() ? QStringLiteral("<empty>") : QStringLiteral("<present>"))
                    .arg(e.id.device_path.empty() ? QStringLiteral("<empty>") : QStringLiteral("<present>")));
        }
    }

    return out;
}

} // namespace exosnap
