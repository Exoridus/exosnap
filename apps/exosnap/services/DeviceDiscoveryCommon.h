#pragma once

namespace exosnap {

// Reason that triggered a device discovery refresh.
// Shared by all three notifier types (audio / webcam / display).
enum class DiscoveryReason {
    Startup,
    Rescan,
    DeviceAdded,
    DeviceRemoved,
    DeviceStateChanged,
    DefaultChanged,
    PropertyChanged,
    GeometryChanged,
};

inline const char* DiscoveryReasonName(DiscoveryReason reason) noexcept {
    switch (reason) {
    case DiscoveryReason::Startup:
        return "Startup";
    case DiscoveryReason::Rescan:
        return "Rescan";
    case DiscoveryReason::DeviceAdded:
        return "DeviceAdded";
    case DiscoveryReason::DeviceRemoved:
        return "DeviceRemoved";
    case DiscoveryReason::DeviceStateChanged:
        return "DeviceStateChanged";
    case DiscoveryReason::DefaultChanged:
        return "DefaultChanged";
    case DiscoveryReason::PropertyChanged:
        return "PropertyChanged";
    case DiscoveryReason::GeometryChanged:
        return "GeometryChanged";
    }
    return "Unknown";
}

} // namespace exosnap
