#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace exosnap::capability {

// GPU vendor, classified from the PCI vendor ID reported by DXGI
// (DXGI_ADAPTER_DESC1::VendorId). "Other" covers the software/WARP adapter
// (Microsoft Basic Render Driver) and any vendor ExoSnap does not yet
// recognize.
enum class AdapterVendor { Nvidia, Amd, Intel, Other };

// Discrete (dGPU) vs integrated (iGPU) heuristic. DXGI does not expose this
// directly; it is inferred from dedicated-vs-shared video memory (see
// ClassifyKind). "Unknown" is returned only when both memory counters are 0,
// which should not happen for a real hardware adapter.
enum class AdapterKind { Discrete, Integrated, Unknown };

// One DXGI adapter (GPU), as reported by IDXGIFactory1::EnumAdapters1. Pure
// data — no Qt types, no engine/UI coupling. Safe to construct in tests
// without touching DXGI.
struct AdapterInfo {
    std::string name;
    AdapterVendor vendor = AdapterVendor::Other;
    AdapterKind kind = AdapterKind::Unknown;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    // Packed LUID (LowPart in the low 32 bits, HighPart in the high 32 bits).
    // Uniquely identifies the adapter for the lifetime of the current desktop
    // session; stable enough to re-locate the same adapter across two
    // EnumerateAdapters() calls, but must not be persisted across reboots.
    int64_t luid = 0;
    uint64_t dedicated_video_memory_bytes = 0;
    uint64_t shared_system_memory_bytes = 0;
};

// Classifies a PCI vendor ID into an AdapterVendor. Pure function, no probing.
// Known IDs: NVIDIA 0x10DE, AMD 0x1002 (also 0x1022 for some APUs), Intel 0x8086.
AdapterVendor ClassifyVendor(uint32_t vendor_id) noexcept;

// Packs a Windows LUID (HighPart, LowPart) into the AdapterInfo::luid layout
// (LowPart in the low 32 bits, HighPart in the high 32 bits). Takes plain
// integers so this header stays free of Windows headers. Pure function.
inline int64_t PackAdapterLuid(int32_t high_part, uint32_t low_part) noexcept {
    return (static_cast<int64_t>(static_cast<uint32_t>(high_part)) << 32) | static_cast<int64_t>(low_part);
}

// Classifies discrete vs integrated from the DXGI-reported memory pools.
// Heuristic: an adapter with meaningfully more dedicated VRAM than shared
// system memory is discrete; one with little-to-no dedicated VRAM (relying on
// system RAM) is integrated. Pure function, no probing.
AdapterKind ClassifyKind(uint64_t dedicated_video_memory_bytes, uint64_t shared_system_memory_bytes) noexcept;

// Enumerates all DXGI adapters (EnumAdapters1) present on this system,
// excluding the software/WARP "Microsoft Basic Render Driver" adapter (it is
// never a real encode target). Re-enumerates on every call — no caching, so a
// "Rescan adapters" action can simply call this again after a hot-plug.
//
// Additive to the existing global CapabilitySet resolution (capability_set.h)
// — this does not replace or feed into it. Callers that want multi-adapter
// facts use this function directly.
std::vector<AdapterInfo> EnumerateAdapters();

} // namespace exosnap::capability
