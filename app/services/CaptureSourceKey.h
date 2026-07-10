#pragma once

// What a capture hub is keyed by. One hub per key; one capture per hub.
//
// The WGC hub keys by the native handle, because that is what
// IGraphicsCaptureItemInterop takes. The DXGI hub keys a monitor by its stable
// GDI device name instead — an HMONITOR does not survive a hot-plug, and
// surviving one is the whole point there (DxgiOdCaptureSrc::Reopen re-resolves
// by the same name).

#include <cstddef>
#include <cstdint>
#include <string>

namespace exosnap {

struct CaptureSourceKey {
    enum class Kind {
        Monitor,     // WGC monitor capture: native_id = HMONITOR
        Window,      // WGC window capture: native_id = HWND
        DxgiMonitor, // DXGI Output Duplication: device_name = "\\.\DISPLAYn"
    };

    Kind kind = Kind::Monitor;
    uintptr_t native_id = 0;  // HMONITOR or HWND; 0 for DxgiMonitor
    std::wstring device_name; // stable GDI device name; empty for WGC kinds

    friend bool operator==(const CaptureSourceKey&, const CaptureSourceKey&) = default;
};

struct CaptureSourceKeyHash {
    size_t operator()(const CaptureSourceKey& k) const noexcept {
        size_t h = static_cast<size_t>(k.native_id);
        if (!k.device_name.empty())
            h ^= std::hash<std::wstring>{}(k.device_name);
        return h ^ (static_cast<size_t>(k.kind) << 1);
    }
};

} // namespace exosnap
