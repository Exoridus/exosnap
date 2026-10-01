#include "DxgiVideoMemoryProvider.h"
#include "NvidiaGpuTelemetryProvider.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <exosnap/engine/gpu_surface_inventory.h>
#include <vector>

namespace exosnap::diagnostics {
namespace {

Microsoft::WRL::ComPtr<IDXGIAdapter1> FindAdapter(int64_t luid) {
    if (luid == 0)
        return {};
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return {};
    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(i, &adapter)))
            return {};
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)))
            continue;
        const auto id =
            (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart;
        if (id == static_cast<uint64_t>(luid))
            return adapter;
    }
}

template <typename Function> Function Resolve(HMODULE library, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(library, name));
}

struct NvmlLibrary {
    HMODULE module = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    int (*shutdown)() = nullptr;
    bool initialized = false;
    NvmlLibrary() {
        if (!module)
            return;
        const auto init = Resolve<int (*)()>(module, "nvmlInit_v2");
        shutdown = Resolve<int (*)()>(module, "nvmlShutdown");
        initialized = init && shutdown && init() == 0;
    }
    ~NvmlLibrary() {
        if (initialized)
            shutdown();
        if (module)
            FreeLibrary(module);
    }
    NvmlLibrary(const NvmlLibrary&) = delete;
    NvmlLibrary& operator=(const NvmlLibrary&) = delete;
};

// Layout of nvmlPciInfo_t for nvmlDeviceGetPciInfo_v3. Keep the legacy field:
// removing it shifts every following ABI field even though only the IDs are read.
struct NvmlPciInfo {
    char legacy_bus_id[16];
    unsigned domain;
    unsigned bus;
    unsigned device;
    unsigned device_id;
    unsigned subsystem_id;
    char bus_id[32];
};
static_assert(sizeof(NvmlPciInfo) == 68);

} // namespace

std::optional<size_t> MatchNvidiaAdapter(NvidiaAdapterIdentity target,
                                         std::span<const NvidiaAdapterIdentity> devices) noexcept {
    std::optional<size_t> match;
    for (size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].device_id != target.device_id || devices[i].subsystem_id != target.subsystem_id)
            continue;
        if (match)
            return std::nullopt;
        match = i;
    }
    return match;
}

GpuTelemetryReading ReadNvmlMeasurements(const NvmlReadApi& api, NvmlReadApi::Device device, int64_t luid) {
    GpuTelemetryReading reading;
    if (!device || luid == 0)
        return reading;
    reading.metadata.source = "NVIDIA NVML (device-wide)";
    reading.metadata.adapter_luid = luid;
    reading.metadata.observed_at = std::chrono::steady_clock::now();
    NvmlReadApi::Utilization utilization{};
    if (api.utilization && api.utilization(device, &utilization) == 0)
        reading.utilization_percent = utilization.gpu;
    unsigned value = 0, period = 0;
    if (api.encoder_utilization && api.encoder_utilization(device, &value, &period) == 0) {
        reading.encoder_utilization_percent = value;
        reading.encoder_sampling_period_us = period;
    }
    unsigned sessions = 0, fps = 0, latency = 0;
    if (api.encoder_stats && api.encoder_stats(device, &sessions, &fps, &latency) == 0) {
        reading.encoder_sessions = sessions;
        reading.encoder_average_fps = fps;
        reading.encoder_average_latency_us = latency;
    }
    if (api.temperature && api.temperature(device, 0, &value) == 0)
        reading.temperature_celsius = value;
    if (api.clock && api.clock(device, 0, &value) == 0)
        reading.graphics_clock_mhz = value;
    if (api.performance_state && api.performance_state(device, &value) == 0)
        reading.performance_state = value;
    return reading;
}

GpuTelemetryReading NvidiaGpuTelemetryProvider::Read(int64_t adapter_luid) {
    const auto adapter = FindAdapter(adapter_luid);
    DXGI_ADAPTER_DESC1 desc{};
    if (!adapter || FAILED(adapter->GetDesc1(&desc)) || desc.VendorId != 0x10DE)
        return {};
    NvmlLibrary library;
    if (!library.initialized)
        return {};
    using Device = NvmlReadApi::Device;
    const auto count_fn = Resolve<int (*)(unsigned*)>(library.module, "nvmlDeviceGetCount_v2");
    const auto handle_fn = Resolve<int (*)(unsigned, Device*)>(library.module, "nvmlDeviceGetHandleByIndex_v2");
    const auto pci_fn = Resolve<int (*)(Device, NvmlPciInfo*)>(library.module, "nvmlDeviceGetPciInfo_v3");
    unsigned count = 0;
    if (!count_fn || !handle_fn || !pci_fn || count_fn(&count) != 0 || count > 64)
        return {};
    std::vector<Device> handles(count);
    std::vector<NvidiaAdapterIdentity> identities(count);
    for (unsigned i = 0; i < count; ++i) {
        NvmlPciInfo pci{};
        // Incomplete enumeration cannot prove unique attribution.
        if (handle_fn(i, &handles[i]) != 0 || pci_fn(handles[i], &pci) != 0)
            return {};
        identities[i] = {pci.device_id, pci.subsystem_id};
    }
    const auto match = MatchNvidiaAdapter({(desc.DeviceId << 16) | desc.VendorId, desc.SubSysId}, identities);
    if (!match)
        return {};
    NvmlReadApi api;
    api.utilization = Resolve<decltype(api.utilization)>(library.module, "nvmlDeviceGetUtilizationRates");
    api.encoder_utilization =
        Resolve<decltype(api.encoder_utilization)>(library.module, "nvmlDeviceGetEncoderUtilization");
    api.encoder_stats = Resolve<decltype(api.encoder_stats)>(library.module, "nvmlDeviceGetEncoderStats");
    api.temperature = Resolve<decltype(api.temperature)>(library.module, "nvmlDeviceGetTemperature");
    api.clock = Resolve<decltype(api.clock)>(library.module, "nvmlDeviceGetClockInfo");
    api.performance_state = Resolve<decltype(api.performance_state)>(library.module, "nvmlDeviceGetPerformanceState");
    return ReadNvmlMeasurements(api, handles[*match], adapter_luid);
}

VideoMemoryReading DxgiVideoMemoryProvider::Read(int64_t adapter_luid) {
    VideoMemoryReading reading;
    reading.logical_surfaces_sampled = true;
    static_assert(reading.logical_surfaces.size() == engine::kGpuSurfaceOwnerNames.size());
    for (size_t i = 0; i < reading.logical_surfaces.size(); ++i) {
        const auto usage = engine::ReadGpuSurfaceUsage(static_cast<engine::GpuSurfaceOwner>(i));
        reading.logical_surfaces[i] = {engine::kGpuSurfaceOwnerNames[i], usage.bytes, usage.surfaces};
    }
    const auto adapter = FindAdapter(adapter_luid);
    Microsoft::WRL::ComPtr<IDXGIAdapter3> budget_adapter;
    if (!adapter || FAILED(adapter.As(&budget_adapter)))
        return reading;
    reading.metadata.source = "DXGI process video-memory budget";
    reading.metadata.adapter_luid = adapter_luid;
    reading.metadata.process_id = GetCurrentProcessId();
    reading.metadata.observed_at = std::chrono::steady_clock::now();
    const auto query = [&budget_adapter](DXGI_MEMORY_SEGMENT_GROUP group) -> std::optional<VideoMemoryBudget> {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(budget_adapter->QueryVideoMemoryInfo(0, group, &info)))
            return std::nullopt;
        return VideoMemoryBudget{info.CurrentUsage, info.Budget, info.CurrentReservation, info.AvailableForReservation};
    };
    reading.local = query(DXGI_MEMORY_SEGMENT_GROUP_LOCAL);
    reading.nonlocal = query(DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL);
    return reading;
}

} // namespace exosnap::diagnostics
