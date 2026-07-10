#pragma once

// What a capture hub is keyed by. One hub per key; one capture per hub.
//
// The WGC hub keys by the native handle, because that is what
// IGraphicsCaptureItemInterop takes. The DXGI hub, when it arrives, will key a
// monitor by its stable GDI device name instead -- an HMONITOR does not survive
// a hot-plug, and surviving one is the whole point there.

#include <cstddef>
#include <cstdint>

namespace exosnap {

struct CaptureSourceKey {
    enum class Kind { Monitor, Window };

    Kind kind = Kind::Monitor;
    uintptr_t native_id = 0; // HMONITOR or HWND, per kind

    friend bool operator==(const CaptureSourceKey&, const CaptureSourceKey&) = default;
};

struct CaptureSourceKeyHash {
    size_t operator()(const CaptureSourceKey& k) const noexcept {
        const size_t h = static_cast<size_t>(k.native_id);
        return h ^ (static_cast<size_t>(k.kind) << 1);
    }
};

} // namespace exosnap
